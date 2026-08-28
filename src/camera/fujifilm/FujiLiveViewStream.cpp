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

// Precomputed look-up tables for ultra-fast (0.4ms) nearest-neighbor scaling
static uint16_t s_lutX[240];
static uint16_t s_lutY[135];
static int s_cachedSrcW = 0, s_cachedSrcH = 0, s_cachedSrcY = 0;
static int s_cachedDstW = 0, s_cachedDstH = 0;

static void updateLut(int srcW, int srcActiveH, int srcOffsetY, int dstW, int dstH) {
    if (s_cachedSrcW == srcW && s_cachedSrcH == srcActiveH && s_cachedSrcY == srcOffsetY &&
        s_cachedDstW == dstW && s_cachedDstH == dstH) {
        return;
    }
    s_cachedSrcW = srcW;
    s_cachedSrcH = srcActiveH;
    s_cachedSrcY = srcOffsetY;
    s_cachedDstW = dstW;
    s_cachedDstH = dstH;

    for (int x = 0; x < dstW; ++x) {
        s_lutX[x] = (x * srcW) / dstW;
    }
    for (int y = 0; y < dstH; ++y) {
        s_lutY[y] = srcOffsetY + (y * srcActiveH) / dstH;
    }
}

static void fastUpscale(const uint16_t* src, int srcStride, int srcW, int srcActiveH, int srcOffsetY,
                        uint16_t* dst, int dstStride, int drawX, int dstW, int dstH, bool mirror) {
    updateLut(srcW, srcActiveH, srcOffsetY, dstW, dstH);

    for (int dy = 0; dy < dstH; ++dy) {
        int sy = s_lutY[dy];
        const uint16_t* sRow = src + sy * srcStride;
        uint16_t* dRow = dst + dy * dstStride + drawX;
        if (!mirror) {
            for (int dx = 0; dx < dstW; ++dx) {
                dRow[dx] = sRow[s_lutX[dx]];
            }
        } else {
            for (int dx = 0; dx < dstW; ++dx) {
                dRow[dx] = sRow[srcW - 1 - s_lutX[dx]];
            }
        }
    }
}
} // anonymous namespace

FujiLiveViewStream::FujiLiveViewStream() 
    : m_decodeSprite(&M5.Display), m_canvas(&M5.Display) {
    m_assembleBuffer.reserve(65536);
    m_frameBuffer.reserve(65536);
}

FujiLiveViewStream::~FujiLiveViewStream() {
    stop();
    if (m_decodeSpriteInit) {
        m_decodeSprite.deleteSprite();
        m_decodeSpriteInit = false;
    }
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

            while (remaining > 0) {
                avail = m_client.available();
                if (avail > 0) {
                    int toRead = std::min((size_t)avail, remaining);
                    int n = m_client.read(m_assembleBuffer.data() + m_payloadBytesRead, toRead);
                    if (n > 0) {
                        m_payloadBytesRead += n;
                        remaining -= n;
                        startWait = millis(); // Reset timeout on successful read
                    } else {
                        break;
                    }
                } else {
                    if (millis() - startWait > 50) {
                        break; // Timeout waiting for next packet segment, yield to main loop
                    }
                    delay(1); // Micro-yield to Wi-Fi stack to fill buffer
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
                break; // Return immediately to render newly arrived frame!
            } else {
                break;
            }
        }
    }
}

bool FujiLiveViewStream::render(M5GFX& display, const String& expText) {
    if (m_frameBuffer.empty()) return false;

    // Detect actual JPEG resolution from camera
    int imgW = 0, imgH = 0;
    readJpegDimensions(m_frameBuffer.data(), m_frameBuffer.size(), imgW, imgH);
    if (imgW <= 0 || imgH <= 0) {
        imgW = 640;
        imgH = 480;
    }

    // 1/4 IDCT hardware decoded dimensions
    int decW = imgW / 4;
    int decH = imgH / 4;

    // Ensure 1/4 IDCT decode sprite is initialized in fast internal SRAM
    if (!m_decodeSpriteInit || m_decodeSprite.width() != decW || m_decodeSprite.height() != decH) {
        m_decodeSprite.deleteSprite();
        m_decodeSprite.setPsram(false);
        m_decodeSprite.setColorDepth(16);
        m_decodeSprite.createSprite(decW, decH);
        m_decodeSpriteInit = true;
    }

    // Ensure 240x135 full screen double-buffering canvas is initialized in internal DMA SRAM
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

    // Step 1: Pure integer 1/4 IDCT hardware decode into m_decodeSprite (5.5ms)
    m_decodeSprite.drawJpg(
        m_frameBuffer.data(),
        m_frameBuffer.size(),
        0, 0,
        decW, decH,
        0, 0,
        0.25f, 0.25f
    );

    // Step 2: Auto-detect active image bounds to remove ANY top/bottom black borders (Letterboxing)
    // We scan the decoded sprite to find the first and last non-black rows.
    uint16_t* ptr = (uint16_t*)m_decodeSprite.getBuffer();
    int activeSrcY = 0;
    int activeSrcH = decH;

    // Scan top-down to find the first non-black row
    for (int y = 0; y < decH / 2; ++y) {
        bool isBlack = true;
        // Sample middle 50% of the row to avoid edge artifacts
        for (int x = decW / 4; x < (decW * 3) / 4; ++x) {
            uint16_t color = ptr[y * decW + x];
            if (color != 0x0000 && color != 0x0821) { // Not pure black or near-black
                isBlack = false;
                break;
            }
        }
        if (!isBlack) {
            activeSrcY = y;
            break;
        }
    }

    // Scan bottom-up to find the last non-black row
    int activeEnd = decH - 1;
    for (int y = decH - 1; y > decH / 2; --y) {
        bool isBlack = true;
        for (int x = decW / 4; x < (decW * 3) / 4; ++x) {
            uint16_t color = ptr[y * decW + x];
            if (color != 0x0000 && color != 0x0821) {
                isBlack = false;
                break;
            }
        }
        if (!isBlack) {
            activeEnd = y;
            break;
        }
    }

    activeSrcH = (activeEnd - activeSrcY) + 1;
    if (activeSrcH < decH / 3) {
        // Fallback if detection fails (e.g. extremely dark scene)
        if (decH >= 118) {
            activeSrcH = (decW * 2) / 3;
            activeSrcY = (decH - activeSrcH) / 2;
        } else {
            activeSrcH = decH;
            activeSrcY = 0;
        }
    }

    // Target frame dimensions: Height = 135 (fills 100% of display height, NO top/bottom black borders)
    // We calculate width dynamically to preserve the exact aspect ratio of the cropped source.
    const int dstH = 135;
    const int dstW = (int)((float)dstH * ((float)decW / (float)activeSrcH));
    const int drawX = (240 - dstW) / 2; // Center horizontally (Left/Right black pillars are expected/acceptable)

    // Clear left and right pillar margins on canvas
    if (drawX > 0) {
        m_canvas.fillRect(0, 0, drawX, 135, TFT_BLACK);
        m_canvas.fillRect(drawX + dstW, 0, 240 - (drawX + dstW), 135, TFT_BLACK);
    }

    // Fast 0.4ms LUT mapping from active area directly to the 135px height canvas
    fastUpscale(
        (const uint16_t*)m_decodeSprite.getBuffer(),
        decW,           // source stride
        decW,           // width to sample
        activeSrcH,     // height to sample
        activeSrcY,     // Y offset in source
        (uint16_t*)m_canvas.getBuffer(),
        240,            // dst stride
        drawX,
        dstW,
        dstH,
        m_mirror
    );

    // Step 3: Draw floating transparent OSD with 1px drop shadow directly onto canvas
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

    // Step 4: Atomic 1-shot DMA push to physical LCD (Zero Flicker!)
    m_canvas.pushSprite(&display, 0, 0);
    return true;
}
