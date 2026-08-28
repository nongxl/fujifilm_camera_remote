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

FujiLiveViewStream::FujiLiveViewStream() {
    m_assembleBuffer.reserve(65536);
    m_frameBuffer.reserve(65536);
}

FujiLiveViewStream::~FujiLiveViewStream() {
    stop();
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

    // High-performance asynchronous non-blocking parser
    while (m_client.available() > 0) {
        if (m_state == StreamState::WAIT_HEADER) {
            // Read 4-byte total packet length
            while (m_headerBytesRead < 4 && m_client.available() > 0) {
                m_headerBytes[m_headerBytesRead++] = (uint8_t)m_client.read();
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
                // Not enough bytes arrived yet, yield immediately to main loop!
                break;
            }
        }
    }
}

bool FujiLiveViewStream::render(M5GFX& display, int x, int y, int w, int h) {
    if (m_frameBuffer.empty()) return false;

    // Detect actual JPEG resolution from camera
    int imgW = 0, imgH = 0;
    readJpegDimensions(m_frameBuffer.data(), m_frameBuffer.size(), imgW, imgH);
    if (imgW <= 0 || imgH <= 0) {
        imgW = 640;
        imgH = 424;
    }

    // Full screen height scaling: height fills 100% of display height (h = 135px)
    float scale = (float)h / (float)imgH;
    int scaledW = (int)(imgW * scale);
    int scaledH = h;

    // Center horizontally in (x, y, w, h)
    int drawX = x + (w - scaledW) / 2;
    int drawY = y;
    if (drawX < 0) drawX = 0;

    // Clear left and right Letterbox margins once to prevent ghosting
    static int s_lastDrawX = -1;
    if (s_lastDrawX != drawX) {
        s_lastDrawX = drawX;
        if (drawX > 0) {
            display.fillRect(0, 0, drawX, h, TFT_BLACK);
            display.fillRect(drawX + scaledW, 0, w - (drawX + scaledW), h, TFT_BLACK);
        }
    }

    // Direct decode and render
    return display.drawJpg(
        m_frameBuffer.data(),
        m_frameBuffer.size(),
        drawX, drawY,
        scaledW, scaledH,
        0, 0,
        scale, scale
    );
}
