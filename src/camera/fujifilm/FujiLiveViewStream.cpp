#include "FujiLiveViewStream.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <esp_heap_caps.h>

FujiLiveViewStream::FujiLiveViewStream() 
    : m_canvas(&M5.Display) {
    m_assembleBuffer.reserve(65536);
    m_frameBuffer.reserve(65536);
}

FujiLiveViewStream::~FujiLiveViewStream() {
    stop();
    if (m_jpeg != nullptr) {
        jpeg_dec_close(m_jpeg);
        m_jpeg = nullptr;
    }
    if (m_jpegOutputBuffer != nullptr) {
        free(m_jpegOutputBuffer);
        m_jpegOutputBuffer = nullptr;
    }
    if (m_canvasInit) {
        m_canvas.deleteSprite();
        m_canvasInit = false;
    }
}

bool FujiLiveViewStream::ensureDecoder() {
    if (m_jpeg == nullptr) {
        jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
        config.output_type = JPEG_PIXEL_FORMAT_RGB565_BE;
        config.scale.width = JPEG_DECODE_WIDTH;   // 320
        config.scale.height = JPEG_DECODE_HEIGHT; // 240

        const jpeg_error_t rc = jpeg_dec_open(&config, &m_jpeg);
        if (rc != JPEG_ERR_OK || m_jpeg == nullptr) {
            Serial.printf("[LiveView] esp_new_jpeg open failed rc=%d\n", (int)rc);
            m_jpeg = nullptr;
            return false;
        }
    }

    if (m_jpegOutputBuffer == nullptr) {
        m_jpegOutputCapacity = (size_t)JPEG_DECODE_WIDTH * (size_t)JPEG_DECODE_HEIGHT * 2;
        m_jpegOutputBuffer = (uint8_t*)heap_caps_aligned_calloc(16, 1, m_jpegOutputCapacity, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (m_jpegOutputBuffer == nullptr && psramFound()) {
            m_jpegOutputBuffer = (uint8_t*)heap_caps_aligned_calloc(16, 1, m_jpegOutputCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        if (m_jpegOutputBuffer == nullptr) {
            Serial.println("[LiveView] Failed to allocate JPEG output buffer");
            return false;
        }
    }

    // Ensure 240x135 canvas is initialized in internal DMA SRAM
    if (!m_canvasInit) {
        m_canvas.setPsram(false);
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

    return true;
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

    int fd = m_client.fd();
    if (fd >= 0) {
        int nodelay = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        int rcvbuf = 32768;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    }

    ensureDecoder();
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
                int fd = m_client.fd();
                if (fd >= 0) {
                    int nodelay = 1;
                    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
                    int rcvbuf = 32768;
                    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
                }
                Serial.println("[LiveView] Reconnected to MJPEG Stream!");
            }
            m_lastFrameTime = millis();
            m_state = StreamState::WAIT_HEADER;
            m_headerBytesRead = 0;
            m_payloadBytesRead = 0;
        }
        return;
    }

    // Process all pending packets aggressively
    while (true) {
        if (!m_client.connected()) break;
        int avail = m_client.available();
        if (avail == 0 && m_state == StreamState::WAIT_HEADER) {
            break; // Yield if no new frame is starting
        }

        if (m_state == StreamState::WAIT_HEADER) {
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
            unsigned long startWait = millis();
            int fd = m_client.fd();

            while (remaining > 0) {
                int n = -1;
                if (fd >= 0) {
                    n = recv(fd, m_assembleBuffer.data() + m_payloadBytesRead, remaining, MSG_DONTWAIT);
                }
                if (n <= 0) {
                    avail = m_client.available();
                    if (avail > 0) {
                        int toRead = std::min((size_t)avail, remaining);
                        n = m_client.read(m_assembleBuffer.data() + m_payloadBytesRead, toRead);
                    }
                }

                if (n > 0) {
                    m_payloadBytesRead += n;
                    remaining -= n;
                    startWait = millis(); // Reset timeout on successful read
                } else {
                    if (millis() - startWait > 50) {
                        break; // Timeout waiting for next packet segment, yield to main loop
                    }
                    delay(1); // Micro-yield to Wi-Fi stack to fill buffer
                }
            }

            if (m_payloadBytesRead >= m_expectedPayloadLen) {
                // Completed one full packet! Find exact JPEG SOI marker (0xFF 0xD8)
                size_t soiPos = 0;
                bool foundSoi = false;
                for (size_t i = 0; i + 1 < m_expectedPayloadLen && i < 64; ++i) {
                    if (m_assembleBuffer[i] == 0xFF && m_assembleBuffer[i+1] == 0xD8) {
                        soiPos = i;
                        foundSoi = true;
                        break;
                    }
                }

                if (foundSoi && m_expectedPayloadLen > soiPos + 100) {
                    m_frameBuffer.assign(
                        m_assembleBuffer.begin() + soiPos,
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
                break; // Return immediately to render newly arrived frame!
            } else {
                break;
            }
        }
    }
}

bool FujiLiveViewStream::render(M5GFX& display, const String& expText) {
    if (m_frameBuffer.empty()) return false;
    if (!ensureDecoder()) return false;

    jpeg_dec_io_t io = {};
    jpeg_dec_header_info_t outInfo = {};
    io.inbuf = m_frameBuffer.data();
    io.inbuf_len = (int)m_frameBuffer.size();

    jpeg_error_t rc = jpeg_dec_parse_header(m_jpeg, &io, &outInfo);
    if (rc != JPEG_ERR_OK) {
        Serial.printf("[JPEG] Parse header failed: %d (bufLen=%d)\n", (int)rc, (int)m_frameBuffer.size());
        return false;
    }

    int outLength = 0;
    rc = jpeg_dec_get_outbuf_len(m_jpeg, &outLength);
    if (rc != JPEG_ERR_OK || outLength <= 0 || (size_t)outLength > m_jpegOutputCapacity) {
        Serial.printf("[JPEG] Invalid outbuf len: %d (rc=%d, cap=%u)\n", outLength, (int)rc, (unsigned)m_jpegOutputCapacity);
        return false;
    }

    io.outbuf = m_jpegOutputBuffer;
    rc = jpeg_dec_process(m_jpeg, &io);
    if (rc != JPEG_ERR_OK) {
        Serial.printf("[JPEG] Process failed: %d\n", (int)rc);
        return false;
    }

    // Hardware SIMD decoded dimensions (320 x 240 for 640x480 camera stream)
    const int decW = outInfo.width > 0 ? outInfo.width : JPEG_DECODE_WIDTH;
    const int decH = outInfo.height > 0 ? outInfo.height : JPEG_DECODE_HEIGHT;

    // Center-crop 320x240 into 240x135 on double-buffering canvas
    // Fills 100% of height and center-crops width cleanly
    const int visibleW = std::min(decW, 240); // 240
    const int visibleH = 135;
    const int cropX = (decW - visibleW) / 2; // (320 - 240) / 2 = 40
    const int cropY = (decH - visibleH) / 2; // (240 - 135) / 2 = 52
    const int drawX = (240 - visibleW) / 2; // 0
    const int drawY = 0;

    const uint16_t* pixels = reinterpret_cast<const uint16_t*>(m_jpegOutputBuffer);

    // Blit hardware decoded rows into double-buffering canvas
    if (!m_mirror) {
        for (int row = 0; row < visibleH; ++row) {
            const uint16_t* sRow = pixels + (cropY + row) * decW + cropX;
            uint16_t* dRow = (uint16_t*)m_canvas.getBuffer() + (drawY + row) * 240 + drawX;
            memcpy(dRow, sRow, visibleW * sizeof(uint16_t));
        }
    } else {
        for (int row = 0; row < visibleH; ++row) {
            const uint16_t* sRow = pixels + (cropY + row) * decW + cropX;
            uint16_t* dRow = (uint16_t*)m_canvas.getBuffer() + (drawY + row) * 240 + drawX;
            for (int col = 0; col < visibleW; ++col) {
                dRow[col] = sRow[visibleW - 1 - col];
            }
        }
    }

    // Draw floating transparent OSD with 1px drop shadow
    if (expText.length() > 0) {
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
