#ifndef FUJI_LIVE_VIEW_STREAM_H
#define FUJI_LIVE_VIEW_STREAM_H

#include <Arduino.h>
#include <WiFiClient.h>
#include <vector>
#include <M5Unified.h>

class FujiLiveViewStream {
public:
    FujiLiveViewStream();
    ~FujiLiveViewStream();

    bool start(const IPAddress& cameraIp, uint16_t port = 55742);
    void stop();
    void update();

    bool isRunning() const { return m_running; }
    bool isConnected() { return m_client.connected(); }
    bool hasNewFrame() const { return m_hasNewFrame; }
    void clearNewFrame() { m_hasNewFrame = false; }

    // Render latest frame with full-height 135px and transparent floating OSD (Zero Flicker via Double-Buffered Canvas)
    bool render(M5GFX& display, const String& expText = "");

    float getFps() const { return m_currentFps; }
    size_t getFrameSize() const { return m_frameBuffer.size(); }

    void setMirror(bool mirror) { m_mirror = mirror; }
    bool isMirror() const { return m_mirror; }

private:
    enum class StreamState {
        WAIT_HEADER,
        READ_PAYLOAD
    };

    WiFiClient m_client;
    IPAddress m_cameraIp;
    uint16_t m_port = 55742;
    bool m_running = false;
    bool m_hasNewFrame = false;
    bool m_mirror = false;

    // Asynchronous framed packet parser state
    StreamState m_state = StreamState::WAIT_HEADER;
    uint8_t m_headerBytes[4] = {0};
    uint8_t m_headerBytesRead = 0;
    size_t m_expectedPayloadLen = 0;
    size_t m_payloadBytesRead = 0;

    // Buffers for assembly and frame storage
    std::vector<uint8_t> m_assembleBuffer;
    std::vector<uint8_t> m_frameBuffer;

    // Hardware double-buffering canvas (64.8KB in internal DMA RAM)
    LGFX_Sprite m_canvas;
    bool m_canvasInit = false;

    unsigned long m_lastFrameTime = 0;
    unsigned long m_lastFpsCalcTime = 0;
    uint32_t m_frameCounter = 0;
    float m_currentFps = 0.0f;

    static constexpr size_t MAX_FRAME_SIZE = 131072; // 128KB max per JPEG frame
    static constexpr unsigned long STREAM_WATCHDOG_MS = 3000;
};

#endif // FUJI_LIVE_VIEW_STREAM_H
