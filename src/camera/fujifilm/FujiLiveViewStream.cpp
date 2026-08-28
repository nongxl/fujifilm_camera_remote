#include "FujiLiveViewStream.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <esp_heap_caps.h>

FujiLiveViewStream::FujiLiveViewStream() 
    : m_canvasA(&M5.Display), m_canvasB(&M5.Display) {
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
    if (m_rxBuffer != nullptr) {
        free(m_rxBuffer);
        m_rxBuffer = nullptr;
    }
    if (m_frameBufferA != nullptr) {
        free(m_frameBufferA);
        m_frameBufferA = nullptr;
    }
    if (m_frameBufferB != nullptr) {
        free(m_frameBufferB);
        m_frameBufferB = nullptr;
    }
    if (m_canvasInit) {
        m_canvasA.deleteSprite();
        m_canvasB.deleteSprite();
        m_canvasInit = false;
    }
}

bool FujiLiveViewStream::ensureBuffers() {
    if (m_rxBuffer == nullptr) {
        m_rxBuffer = (uint8_t*)heap_caps_malloc(RX_BUFFER_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!m_rxBuffer && psramFound()) {
            m_rxBuffer = (uint8_t*)heap_caps_malloc(RX_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
    }

    if (m_frameBufferA == nullptr) {
        m_frameBufferA = (uint8_t*)heap_caps_malloc(MAX_FRAME_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!m_frameBufferA && psramFound()) {
            m_frameBufferA = (uint8_t*)heap_caps_malloc(MAX_FRAME_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
    }

    if (m_frameBufferB == nullptr) {
        m_frameBufferB = (uint8_t*)heap_caps_malloc(MAX_FRAME_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!m_frameBufferB && psramFound()) {
            m_frameBufferB = (uint8_t*)heap_caps_malloc(MAX_FRAME_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
    }

    m_assembleBufPtr = m_frameBufferA;
    m_readyBufPtr = m_frameBufferB;

    if (!m_rxBuffer || !m_frameBufferA || !m_frameBufferB) {
        Serial.printf("[LiveView] Buffer allocation failed: rx=%p, A=%p, B=%p\n", m_rxBuffer, m_frameBufferA, m_frameBufferB);
        return false;
    }
    return true;
}

bool FujiLiveViewStream::hasNewFrame() {
    return m_hasNewFrame;
}

void FujiLiveViewStream::clearNewFrame() {
    m_hasNewFrame = false;
}

size_t FujiLiveViewStream::getFrameSize() {
    portENTER_CRITICAL(&m_frameMux);
    size_t sz = m_readyLen;
    portEXIT_CRITICAL(&m_frameMux);
    return sz;
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

    // Ensure 240x135 Ping-Pong canvases are initialized
    if (!m_canvasInit) {
        m_canvasA.setPsram(false);
        m_canvasA.setColorDepth(16);
        void* ptrA = m_canvasA.createSprite(240, 135);
        if (ptrA == nullptr && psramFound()) {
            m_canvasA.setPsram(true);
            m_canvasA.createSprite(240, 135);
        }
        m_canvasA.setTextSize(1);
        m_canvasA.setTextWrap(false);

        m_canvasB.setPsram(false);
        m_canvasB.setColorDepth(16);
        void* ptrB = m_canvasB.createSprite(240, 135);
        if (ptrB == nullptr && psramFound()) {
            m_canvasB.setPsram(true);
            m_canvasB.createSprite(240, 135);
        }
        m_canvasB.setTextSize(1);
        m_canvasB.setTextWrap(false);

        m_activeCanvas = 0;
        m_canvasInit = true;
    }

    return true;
}

bool FujiLiveViewStream::start(const IPAddress& cameraIp, uint16_t port) {
    stop();
    m_cameraIp = cameraIp;
    m_port = port;
    m_running = true;
    m_hasNewFrame = false;
    m_parseState = ParseState::SEEKING_SOI;
    m_prevByte = 0;
    m_assembleLen = 0;
    m_readyLen = 0;
    m_lastFrameTime = millis();
    m_lastFpsCalcTime = millis();
    m_lastRenderTime = millis();
    m_frameCounter = 0;
    m_currentFps = 0.0f;

    if (!ensureBuffers()) {
        Serial.println("[LiveView] Cannot start: buffer allocation failed");
        return false;
    }

    Serial.printf("[LiveView] Connecting to %s:%d...\n", m_cameraIp.toString().c_str(), m_port);
    if (!m_client.connect(m_cameraIp, m_port)) {
        Serial.println("[LiveView] Initial connection failed, will retry in background task.");
    } else {
        m_client.setNoDelay(true);
        m_client.setTimeout(1);

        int fd = m_client.fd();
        if (fd >= 0) {
            int nodelay = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
            int rcvbuf = 32768;
            setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
        }
        Serial.println("[LiveView] Connected to MJPEG Stream!");
    }

    ensureDecoder();

    // Spawn dedicated FreeRTOS stream receiver task pinned to Core 0 with 8KB stack
    xTaskCreatePinnedToCore(
        rxTaskTrampoline,
        "LiveViewRx",
        8192,
        this,
        5,
        &m_rxTaskHandle,
        0 // Core 0 alongside lwIP TCP stack
    );

    return true;
}

void FujiLiveViewStream::stop() {
    m_running = false;
    m_hasNewFrame = false;

    if (m_rxTaskHandle != nullptr) {
        vTaskDelete(m_rxTaskHandle);
        m_rxTaskHandle = nullptr;
    }

    if (m_client.connected()) {
        m_client.stop();
    }

    m_parseState = ParseState::SEEKING_SOI;
    m_prevByte = 0;
    m_assembleLen = 0;
    m_readyLen = 0;
    Serial.println("[LiveView] Stream stopped.");
}

void FujiLiveViewStream::rxTaskTrampoline(void* arg) {
    auto* self = static_cast<FujiLiveViewStream*>(arg);
    self->rxTaskLoop();
    vTaskDelete(nullptr);
}

void FujiLiveViewStream::processStreamData(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0 || !m_assembleBufPtr) return;

    for (size_t i = 0; i < len; ++i) {
        const uint8_t b = data[i];
        const bool isMarker = (m_prevByte == 0xFF);

        if (m_parseState == ParseState::SEEKING_SOI) {
            if (isMarker && b == 0xD8) {
                // Detected Start of Image (SOI 0xFF 0xD8)
                m_parseState = ParseState::IN_FRAME;
                m_assembleLen = 0;
                m_assembleBufPtr[m_assembleLen++] = 0xFF;
                m_assembleBufPtr[m_assembleLen++] = 0xD8;
            }
        } else {
            // Accumulating frame payload
            if (m_assembleLen < MAX_FRAME_SIZE) {
                m_assembleBufPtr[m_assembleLen++] = b;
            }

            if (isMarker && b == 0xD9) {
                // Detected End of Image (EOI 0xFF 0xD9)
                if (m_assembleLen > 256) {
                    portENTER_CRITICAL(&m_frameMux);
                    // Fast zero-copy pointer swap between assemble buffer and ready buffer
                    uint8_t* temp = m_readyBufPtr;
                    m_readyBufPtr = m_assembleBufPtr;
                    m_assembleBufPtr = temp;
                    m_readyLen = m_assembleLen;
                    m_hasNewFrame = true;
                    portEXIT_CRITICAL(&m_frameMux);

                    m_lastFrameTime = millis();
                    m_frameCounter++;
                    unsigned long now = millis();
                    if (now - m_lastFpsCalcTime >= 1000) {
                        m_currentFps = (float)m_frameCounter * 1000.0f / (float)(now - m_lastFpsCalcTime);
                        m_frameCounter = 0;
                        m_lastFpsCalcTime = now;
                    }
                }
                m_parseState = ParseState::SEEKING_SOI;
            }
        }
        m_prevByte = b;
    }
}

void FujiLiveViewStream::rxTaskLoop() {
    if (!m_rxBuffer) return;

    while (m_running) {
        if (!m_client.connected()) {
            if (millis() - m_lastFrameTime > STREAM_WATCHDOG_MS) {
                Serial.println("[LiveView Task] Watchdog reconnecting socket...");
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
                    Serial.println("[LiveView Task] Reconnected!");
                }
                m_lastFrameTime = millis();
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        int fd = m_client.fd();
        int bytesRead = -1;
        if (fd >= 0) {
            bytesRead = recv(fd, m_rxBuffer, RX_BUFFER_SIZE, MSG_DONTWAIT);
        }
        if (bytesRead <= 0) {
            int avail = m_client.available();
            if (avail > 0) {
                int toRead = std::min((int)RX_BUFFER_SIZE, avail);
                bytesRead = m_client.read(m_rxBuffer, toRead);
            }
        }

        if (bytesRead > 0) {
            processStreamData(m_rxBuffer, static_cast<size_t>(bytesRead));
        } else {
            // Micro-yield to lwIP TCP stack when socket rx buffer is drained
            vTaskDelay(1);
        }
    }
}

void FujiLiveViewStream::update() {
    // Background receiver task runs autonomously on Core 0!
}

bool FujiLiveViewStream::render(M5GFX& display, const String& expText) {
    if (!m_hasNewFrame || !m_readyBufPtr || m_readyLen == 0) return false;
    if (!ensureDecoder()) return false;

    // Direct hardware decoding from zero-copy ready frame buffer
    jpeg_dec_io_t io = {};
    jpeg_dec_header_info_t outInfo = {};
    io.inbuf = m_readyBufPtr;
    io.inbuf_len = (int)m_readyLen;

    m_hasNewFrame = false;

    jpeg_error_t rc = jpeg_dec_parse_header(m_jpeg, &io, &outInfo);
    if (rc != JPEG_ERR_OK) {
        return false;
    }

    int outLength = 0;
    rc = jpeg_dec_get_outbuf_len(m_jpeg, &outLength);
    if (rc != JPEG_ERR_OK || outLength <= 0 || (size_t)outLength > m_jpegOutputCapacity) {
        return false;
    }

    io.outbuf = m_jpegOutputBuffer;
    rc = jpeg_dec_process(m_jpeg, &io);
    if (rc != JPEG_ERR_OK) {
        return false;
    }

    // Hardware SIMD decoded dimensions (320 x 240 for 640x480 camera stream)
    const int decW = outInfo.width > 0 ? outInfo.width : JPEG_DECODE_WIDTH;
    const int decH = outInfo.height > 0 ? outInfo.height : JPEG_DECODE_HEIGHT;

    // Center-crop 320x240 into 240x135 on target double-buffering canvas
    const int visibleW = std::min(decW, 240); // 240
    const int visibleH = 135;
    const int cropX = (decW - visibleW) / 2; // (320 - 240) / 2 = 40
    const int cropY = (decH - visibleH) / 2; // (240 - 135) / 2 = 52
    const int drawX = (240 - visibleW) / 2; // 0
    const int drawY = 0;

    const uint16_t* pixels = reinterpret_cast<const uint16_t*>(m_jpegOutputBuffer);

    // Pick alternate canvas for double-buffering composition
    LGFX_Sprite& currentCanvas = (m_activeCanvas == 0) ? m_canvasA : m_canvasB;

    // Blit hardware decoded rows into chosen canvas
    if (!m_mirror) {
        for (int row = 0; row < visibleH; ++row) {
            const uint16_t* sRow = pixels + (cropY + row) * decW + cropX;
            uint16_t* dRow = (uint16_t*)currentCanvas.getBuffer() + (drawY + row) * 240 + drawX;
            memcpy(dRow, sRow, visibleW * sizeof(uint16_t));
        }
    } else {
        for (int row = 0; row < visibleH; ++row) {
            const uint16_t* sRow = pixels + (cropY + row) * decW + cropX;
            uint16_t* dRow = (uint16_t*)currentCanvas.getBuffer() + (drawY + row) * 240 + drawX;
            for (int col = 0; col < visibleW; ++col) {
                dRow[col] = sRow[visibleW - 1 - col];
            }
        }
    }

    // Draw sleek dark glassmorphism pill banner at bottom (height 17px, y: 116..133)
    currentCanvas.fillRoundRect(2, 116, 236, 17, 3, 0x1082); // Dark slate background
    currentCanvas.drawRoundRect(2, 116, 236, 17, 3, 0x2965); // Crisp subtle border

    // Left: Exposure parameters in crisp white
    if (expText.length() > 0) {
        currentCanvas.setTextDatum(datum_t::middle_left);
        currentCanvas.setTextColor(TFT_WHITE);
        currentCanvas.drawString(expText.c_str(), 8, 125);
    }

    // Right: FPS counter in high-visibility neon green
    char fpsBuf[16];
    snprintf(fpsBuf, sizeof(fpsBuf), "%.0f fps", m_currentFps);
    currentCanvas.setTextDatum(datum_t::middle_right);
    currentCanvas.setTextColor(0x00FF88);
    currentCanvas.drawString(fpsBuf, 232, 125);

    // Push completed canvas to display using official LovyanGFX pipeline
    currentCanvas.pushSprite(&display, 0, 0);

    // Swap active canvas index for next frame
    m_activeCanvas = 1 - m_activeCanvas;
    m_lastRenderTime = millis();
    return true;
}
