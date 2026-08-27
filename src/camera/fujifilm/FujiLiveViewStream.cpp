#include "FujiLiveViewStream.h"

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

    // High-performance block reader & SOI/EOI scanner
    uint8_t chunk[4096];
    while (m_client.available() > 0) {
        int n = m_client.read(chunk, sizeof(chunk));
        if (n <= 0) break;

        int pos = 0;
        while (pos < n) {
            if (!m_foundSOI) {
                // Search for 0xFF 0xD8 (SOI) in chunk
                bool found = false;
                for (int i = pos; i < n - 1; ++i) {
                    if (chunk[i] == 0xFF && chunk[i+1] == 0xD8) {
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
                    if (m_assembleBuffer.size() == 1 && m_assembleBuffer[0] == 0xFF && chunk[pos] == 0xD8) {
                        m_assembleBuffer.push_back(0xD8);
                        pos++;
                        m_foundSOI = true;
                    } else {
                        m_assembleBuffer.clear();
                        if (chunk[n-1] == 0xFF) m_assembleBuffer.push_back(0xFF);
                        break;
                    }
                }
            } else {
                // We have SOI, search for EOI (0xFF 0xD9)
                int eoiPos = -1;
                for (int i = pos; i < n - 1; ++i) {
                    if (chunk[i] == 0xFF && chunk[i+1] == 0xD9) {
                        eoiPos = i;
                        break;
                    }
                }

                if (eoiPos >= 0) {
                    // Append up to EOI
                    m_assembleBuffer.insert(m_assembleBuffer.end(), chunk + pos, chunk + eoiPos + 2);
                    pos = eoiPos + 2;

                    // Frame complete!
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
                    m_assembleBuffer.insert(m_assembleBuffer.end(), chunk + pos, chunk + n);
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

    // Draw JPEG using M5GFX hardware-accelerated decoder
    bool ok = display.drawJpg(m_frameBuffer.data(), m_frameBuffer.size(), x, y, w, h);
    return ok;
}
