#include "FujiLiveViewStream.h"

namespace {
static bool readJpegDimensions(const uint8_t* data, size_t len, int& outW, int& outH) {
    outW = 0;
    outH = 0;
    if (!data || len < 4 || data[0] != 0xFF || data[1] != 0xD8) return false;
    
    size_t i = 2;
    while (i + 8 < len) {
        if (data[i] != 0xFF) { 
            i++; 
            continue; 
        }
        uint8_t marker = data[i + 1];
        // SOF0, SOF1, SOF2 markers containing image dimensions
        if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
            outH = (data[i + 5] << 8) | data[i + 6];
            outW = (data[i + 7] << 8) | data[i + 8];
            return (outW > 0 && outH > 0);
        }
        if (marker == 0xD9 || marker == 0xDA) break; // EOI or Start of Scan
        if (i + 3 >= len) break;
        uint16_t blockLen = (data[i + 2] << 8) | data[i + 3];
        if (blockLen < 2) break;
        i += 2 + blockLen;
    }
    return false;
}
} // anonymous namespace

FujiLiveViewStream::FujiLiveViewStream() : m_canvas(&M5.Display) {
    m_assembleBuffer.reserve(65536);
    m_frameBuffer.reserve(65536);
}

FujiLiveViewStream::~FujiLiveViewStream() {
    stop();
    if (m_canvasInit) {
        m_canvas.deleteSprite();
        m_canvasInit = false;
    }
}

bool FujiLiveViewStream::start(const IPAddress& cameraIp, uint16_t port) {
    stop();
    m_cameraIp = cameraIp;
    m_port = port;
    m_running = true;
    m_hasNewFrame = false;
    m_state = StreamState::WAIT_HEADER;
    m_headerBytesRead = 0;
    m_payloadBytesRead = 0;
    m_assembleBuffer.clear();
    m_frameBuffer.clear();
    m_lastFrameTime = millis();
    m_lastFpsCalcTime = millis();
    m_frameCounter = 0;
    m_currentFps = 0.0f;

    Serial.printf("[LiveView] Connecting to %s:%d...\n", m_cameraIp.toString().c_str(), m_port);
    if (!m_client.connect(m_cameraIp, m_port)) {
        Serial.println("[LiveView] Initial connection failed, will retry in background.");
        return false;
    }
    m_client.setNoDelay(true);
    m_client.setTimeout(1);

    Serial.println("[LiveView] Connected to MJPEG Stream!");
    return true;
}

void FujiLiveViewStream::stop() {
    m_running = false;
    m_hasNewFrame = false;
    if (m_client.connected()) {
        m_client.stop();
    }
    m_assembleBuffer.clear();
    m_state = StreamState::WAIT_HEADER;
    m_headerBytesRead = 0;
    m_payloadBytesRead = 0;
    Serial.println("[LiveView] Stream stopped.");
}

void FujiLiveViewStream::update() {
    if (!m_running) return;

    // Stream Watchdog: Check for reconnect if stream is stalled
    if (!m_client.connected()) {
        if (millis() - m_lastFrameTime > STREAM_WATCHDOG_MS) {
            Serial.println("[LiveView] Watchdog: Reconnecting stream socket...");
            m_client.stop();
            if (m_client.connect(m_cameraIp, m_port)) {
                m_client.setNoDelay(true);
                m_client.setTimeout(1);
                Serial.println("[LiveView] Reconnected to MJPEG Stream!");
            }
            m_lastFrameTime = millis();
            m_state = StreamState::WAIT_HEADER;
            m_headerBytesRead = 0;
            m_payloadBytesRead = 0;
        }
        return;
    }

    // High-performance asynchronous non-blocking stream reader
    while (m_client.available() > 0) {
        if (m_state == StreamState::WAIT_HEADER) {
            // Read 4-byte total packet length in chunks
            int n = m_client.read(m_headerBytes + m_headerBytesRead, 4 - m_headerBytesRead);
            if (n > 0) {
                m_headerBytesRead += n;
            }

            if (m_headerBytesRead == 4) {
                uint32_t totalLen = 0;
                memcpy(&totalLen, m_headerBytes, 4);

                if (totalLen < 18 || totalLen > MAX_FRAME_SIZE) {
                    // Desync recovery: slide 1 byte
                    m_headerBytes[0] = m_headerBytes[1];
                    m_headerBytes[1] = m_headerBytes[2];
                    m_headerBytes[2] = m_headerBytes[3];
                    m_headerBytesRead = 3;
                    continue;
                }

                m_expectedPayloadLen = totalLen - 4;
                if (m_assembleBuffer.size() < m_expectedPayloadLen) {
                    m_assembleBuffer.resize(m_expectedPayloadLen);
                }
                m_payloadBytesRead = 0;
                m_headerBytesRead = 0;
                m_state = StreamState::READ_PAYLOAD;
            }
        }

        if (m_state == StreamState::READ_PAYLOAD) {
            size_t remaining = m_expectedPayloadLen - m_payloadBytesRead;
            int avail = m_client.available();
            if (avail > 0) {
                int toRead = std::min((size_t)avail, remaining);
                int n = m_client.read(m_assembleBuffer.data() + m_payloadBytesRead, toRead);
                if (n > 0) {
                    m_payloadBytesRead += n;
                }
            }

            if (m_payloadBytesRead >= m_expectedPayloadLen) {
                // Completed one full JPEG frame!
                if (m_expectedPayloadLen > 14) {
                    m_frameBuffer.assign(
                        m_assembleBuffer.begin() + 14,
                        m_assembleBuffer.begin() + m_expectedPayloadLen
                    );
                    m_hasNewFrame = true;
                    m_lastFrameTime = millis();
                    m_frameCounter++;

                    if (millis() - m_lastFpsCalcTime >= 1000) {
                        m_currentFps = (float)m_frameCounter * 1000.0f / (float)(millis() - m_lastFpsCalcTime);
                        m_frameCounter = 0;
                        m_lastFpsCalcTime = millis();
                    }
                }
                m_state = StreamState::WAIT_HEADER;
                m_headerBytesRead = 0;
                m_payloadBytesRead = 0;
            } else {
                // Yield to main loop
                break;
            }
        }
    }
}

bool FujiLiveViewStream::render(M5GFX& display, const String& expText) {
    if (m_frameBuffer.empty()) return false;

    // Ensure 240x135 internal DMA sprite canvas is initialized
    if (!m_canvasInit) {
        m_canvas.setPsram(false); // Internal RAM for high-speed DMA
        m_canvas.setColorDepth(16);
        void* ptr = m_canvas.createSprite(240, 135);
        if (ptr == nullptr && psramFound()) {
            m_canvas.setPsram(true);
            m_canvas.createSprite(240, 135);
        }
        m_canvasInit = true;
        m_canvas.setTextSize(1);
        m_canvas.setTextWrap(false);
    }

    // Detect actual JPEG resolution from camera
    int imgW = 0, imgH = 0;
    readJpegDimensions(m_frameBuffer.data(), m_frameBuffer.size(), imgW, imgH);
    if (imgW <= 0 || imgH <= 0) {
        imgW = 640;
        imgH = 424;
    }

    // Full screen height scaling: height fills 100% of display (135px, 0..134)
    // 3:2 camera JPEG: 640x424 -> 204x135
    float scale = 135.0f / (float)imgH;
    int scaledW = (int)(imgW * scale);
    int scaledH = 135;
    int drawX = (240 - scaledW) / 2;
    if (drawX < 0) drawX = 0;

    // Clear side pillar margins on canvas
    m_canvas.fillScreen(TFT_BLACK);

    // Draw full-height JPEG onto canvas
    m_canvas.drawJpg(
        m_frameBuffer.data(),
        m_frameBuffer.size(),
        drawX, 0,
        scaledW, scaledH,
        0, 0,
        scale, scale
    );

    // Draw floating transparent OSD with 1px drop shadow directly onto canvas
    if (expText.length() > 0) {
        // Bottom-Left: Exposure parameters
        m_canvas.setTextDatum(datum_t::bottom_left);
        m_canvas.setTextColor(TFT_BLACK);
        m_canvas.drawString(expText.c_str(), 5, 133); // Shadow
        m_canvas.setTextColor(TFT_WHITE);
        m_canvas.drawString(expText.c_str(), 4, 132);
    }

    // Bottom-Right: FPS counter
    char fpsBuf[16];
    snprintf(fpsBuf, sizeof(fpsBuf), "%.0f fps", m_currentFps);
    m_canvas.setTextDatum(datum_t::bottom_right);
    m_canvas.setTextColor(TFT_BLACK);
    m_canvas.drawString(fpsBuf, 237, 133); // Shadow
    m_canvas.setTextColor(0x00FF88);
    m_canvas.drawString(fpsBuf, 236, 132);

    // Atomic 1-shot DMA push to physical LCD (Zero Flicker!)
    m_canvas.pushSprite(&display, 0, 0);
    return true;
}
