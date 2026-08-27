#include "FujiLiveViewStream.h"

FujiLiveViewStream::FujiLiveViewStream() {
    m_assembleBuffer.reserve(32768);
    m_frameBuffer.reserve(32768);
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
    m_client.setNoDelay(true);
    m_client.setTimeout(2);
    if (!m_client.connect(m_cameraIp, m_port)) {
        Serial.println("[LiveView] Initial connection failed, will retry in background.");
        return false;
    }

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
            m_client.connect(m_cameraIp, m_port);
            m_lastFrameTime = millis();
        }
        return;
    }

    // Read incoming stream data
    uint8_t tempBuf[2048];
    while (m_client.available() > 0) {
        int bytesRead = m_client.read(tempBuf, sizeof(tempBuf));
        if (bytesRead <= 0) break;

        for (int i = 0; i < bytesRead; ++i) {
            uint8_t byte = tempBuf[i];

            if (!m_foundSOI) {
                // Look for SOI (0xFF 0xD8)
                if (m_assembleBuffer.empty()) {
                    if (byte == 0xFF) {
                        m_assembleBuffer.push_back(byte);
                    }
                } else if (m_assembleBuffer.size() == 1 && m_assembleBuffer[0] == 0xFF) {
                    if (byte == 0xD8) {
                        m_assembleBuffer.push_back(byte);
                        m_foundSOI = true;
                    } else if (byte != 0xFF) {
                        m_assembleBuffer.clear();
                    }
                }
            } else {
                // Accumulate JPEG bytes
                m_assembleBuffer.push_back(byte);

                // Look for EOI (0xFF 0xD9)
                size_t sz = m_assembleBuffer.size();
                if (sz >= 2 && m_assembleBuffer[sz - 2] == 0xFF && m_assembleBuffer[sz - 1] == 0xD9) {
                    // Completed full frame!
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
                } else if (sz >= MAX_FRAME_SIZE) {
                    // Frame corrupted or overflow, reset
                    m_assembleBuffer.clear();
                    m_foundSOI = false;
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
