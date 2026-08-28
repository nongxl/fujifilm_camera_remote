#ifndef FUJI_LIVE_VIEW_STREAM_H
#define FUJI_LIVE_VIEW_STREAM_H

#include <Arduino.h>
#include <WiFiClient.h>
#include <M5Unified.h>
#include <esp_jpeg_dec.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class FujiLiveViewStream {
public:
    FujiLiveViewStream();
    ~FujiLiveViewStream();

    bool start(const IPAddress& cameraIp, uint16_t port = 55742);
    void stop();
    void update();

    bool isRunning() const { return m_running; }
    bool isConnected() { return m_client.connected(); }
    bool hasNewFrame();
    void clearNewFrame();

    // Render latest frame using esp_new_jpeg SIMD hardware decoder with transparent floating OSD
    bool render(M5GFX& display, const String& expText = "");

    float getFps() const { return m_currentFps; }
    size_t getFrameSize();

    void setMirror(bool mirror) { m_mirror = mirror; }
    bool isMirror() const { return m_mirror; }

private:
    enum class ParseState {
        SEEKING_SOI,
        IN_FRAME
    };

    bool ensureDecoder();
    void processStreamData(const uint8_t* data, size_t len);
    static void rxTaskTrampoline(void* arg);
    void rxTaskLoop();

    WiFiClient m_client;
    IPAddress m_cameraIp;
    uint16_t m_port = 55742;
    volatile bool m_running = false;
    volatile bool m_hasNewFrame = false;
    bool m_mirror = false;

    // FreeRTOS background task handle & mutex
    TaskHandle_t m_rxTaskHandle = nullptr;
    portMUX_TYPE m_frameMux = portMUX_INITIALIZER_UNLOCKED;

    // Streaming MJPEG State Machine (SOI/EOI tracker)
    ParseState m_parseState = ParseState::SEEKING_SOI;
    uint8_t m_prevByte = 0;
    size_t m_assembleLen = 0;

    // Zero-copy Ping-Pong DMA frame buffers
    uint8_t* m_rxBuffer = nullptr;
    uint8_t* m_frameBufferA = nullptr;
    uint8_t* m_frameBufferB = nullptr;
    uint8_t* m_assembleBufPtr = nullptr;
    uint8_t* m_readyBufPtr = nullptr;
    size_t m_readyLen = 0;

    // Hardware SIMD JPEG decoder handle and aligned DMA output buffer
    jpeg_dec_handle_t m_jpeg = nullptr;
    uint8_t* m_jpegOutputBuffer = nullptr;
    size_t m_jpegOutputCapacity = 0;

    // Final composition double-buffering canvas (240x135, internal DMA SRAM)
    LGFX_Sprite m_canvas;
    bool m_canvasInit = false;

    unsigned long m_lastFrameTime = 0;
    unsigned long m_lastFpsCalcTime = 0;
    uint32_t m_frameCounter = 0;
    float m_currentFps = 0.0f;

    static constexpr size_t RX_BUFFER_SIZE = 8192;
    static constexpr size_t MAX_FRAME_SIZE = 131072; // 128KB max per JPEG frame
    static constexpr unsigned long STREAM_WATCHDOG_MS = 3000;
    static constexpr uint16_t JPEG_DECODE_WIDTH = 320;
    static constexpr uint16_t JPEG_DECODE_HEIGHT = 240;
};

#endif // FUJI_LIVE_VIEW_STREAM_H
