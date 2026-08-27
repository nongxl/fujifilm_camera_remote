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
    m_foundSOI = false;
    m_assembleBuffer.clear();
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
    m_client.setTimeout(2);

    Serial.println("[LiveView] Connected to MJPEG Stream!");
    return true;
}

void FujiLiveViewStream::stop() {
    m_running = false;
    m_foundSOI = false;
    if (m_client.connected()) {
        m_client.stop();
    }
    m_assembleBuffer.clear();
    Serial.println("[LiveView] Stream stopped.");
}

void FujiLiveViewStream::update() {
    if (!m_running) return;

    // Stream Watchdog: Check for reconnect if stream is stalled
    if (!m_client.connected()) {
        if (millis() - m_lastFrameTime > STREAM_WATCHDOG_MS) {
            Serial.println("[LiveView] Watchdog triggered: Reconnecting stream socket...");
            m_client.stop();
            if (m_client.connect(m_cameraIp, m_port)) {
                m_client.setNoDelay(true);
                m_client.setTimeout(2);
                Serial.println("[LiveView] Reconnected to MJPEG Stream!");
            }
            m_lastFrameTime = millis();
        }
        return;
    }

    // High-performance 8KB socket reader & block parser (using heap member buffer)
    while (m_client.available() > 0) {
        int n = m_client.read(m_chunk, CHUNK_SIZE);
        if (n <= 0) break;

        int pos = 0;
        while (pos < n) {
            if (!m_foundSOI) {
                // Search for 0xFF 0xD8 (SOI) in chunk
                bool found = false;
                for (int i = pos; i < n - 1; ++i) {
                    if (m_chunk[i] == 0xFF && m_chunk[i+1] == 0xD8) {
                        m_assembleBuffer.clear();
                        m_assembleBuffer.push_back(0xFF);
                        m_assembleBuffer.push_back(0xD8);
                        pos = i + 2;
                        m_foundSOI = true;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    // Check edge byte across chunk boundaries
                    if (m_assembleBuffer.size() == 1 && m_assembleBuffer[0] == 0xFF && m_chunk[pos] == 0xD8) {
                        m_assembleBuffer.push_back(0xD8);
                        pos++;
                        m_foundSOI = true;
                    } else {
                        m_assembleBuffer.clear();
                        if (m_chunk[n-1] == 0xFF) m_assembleBuffer.push_back(0xFF);
                        break;
                    }
                }
            } else {
                // We have SOI, search for EOI (0xFF 0xD9)
                int eoiPos = -1;
                for (int i = pos; i < n - 1; ++i) {
                    if (m_chunk[i] == 0xFF && m_chunk[i+1] == 0xD9) {
                        eoiPos = i;
                        break;
                    }
                }

                if (eoiPos >= 0) {
                    // Append up to EOI
                    m_assembleBuffer.insert(m_assembleBuffer.end(), m_chunk + pos, m_chunk + eoiPos + 2);
                    pos = eoiPos + 2;

                    // Full Frame Completed!
                    m_frameBuffer = m_assembleBuffer;
                    m_hasNewFrame = true;
                    m_lastFrameTime = millis();
                    m_frameCounter++;

                    // Calculate FPS every 1 second
                    if (millis() - m_lastFpsCalcTime >= 1000) {
                        m_currentFps = (float)m_frameCounter * 1000.0f / (float)(millis() - m_lastFpsCalcTime);
                        m_frameCounter = 0;
                        m_lastFpsCalcTime = millis();
                    }

                    // Reset assembler for next frame
                    m_assembleBuffer.clear();
                    m_foundSOI = false;
                } else {
                    // No EOI in this chunk, append entire remaining chunk
                    m_assembleBuffer.insert(m_assembleBuffer.end(), m_chunk + pos, m_chunk + n);
                    pos = n;

                    if (m_assembleBuffer.size() > MAX_FRAME_SIZE) {
                        m_assembleBuffer.clear();
                        m_foundSOI = false;
                    }
                }
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
        imgH = 480;
    }

    // Use 1/4 hardware integer IDCT scale (0.25f)
    // 640x424 (3:2) -> 160x106, 640x480 (4:3) -> 160x120
    // This executes in only ~6ms via pure IDCT and bypasses the slow affine resampler!
    const float scale = 0.25f;
    int scaledW = imgW / 4;
    int scaledH = imgH / 4;

    // Center the full uncropped camera composition in (x, y, w, h) with clean Letterbox borders
    int drawX = x + (w - scaledW) / 2;
    int drawY = y + (h - scaledH) / 2;
    if (drawX < 0) drawX = 0;
    if (drawY < 0) drawY = 0;

    // Direct fast IDCT decode and render
    return display.drawJpg(
        m_frameBuffer.data(),
        m_frameBuffer.size(),
        drawX, drawY,
        scaledW, scaledH,
        0, 0,
        scale, scale
    );
}
