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

    // Render latest frame to display with auto centering & scaling
    bool render(M5GFX& display, int x = 0, int y = 0, int w = 240, int h = 135);

    float getFps() const { return m_currentFps; }
    size_t getFrameSize() const { return m_frameBuffer.size(); }

    void setMirror(bool mirror) { m_mirror = mirror; }
    bool isMirror() const { return m_mirror; }

private:
    void resetBuffer();

    WiFiClient m_client;
    IPAddress m_cameraIp;
    uint16_t m_port = 55742;
    bool m_running = false;
    bool m_hasNewFrame = false;
    bool m_mirror = false;

    // Buffer for assembling current incoming JPEG frame
    std::vector<uint8_t> m_assembleBuffer;
    // Buffer storing latest completed JPEG frame
    std::vector<uint8_t> m_frameBuffer;

    bool m_foundSOI = false;
    unsigned long m_lastFrameTime = 0;
    unsigned long m_lastFpsCalcTime = 0;
    uint32_t m_frameCounter = 0;
    float m_currentFps = 0.0f;

    static constexpr size_t MAX_FRAME_SIZE = 131072; // 128KB max per JPEG frame
    static constexpr unsigned long STREAM_WATCHDOG_MS = 3000;
};

#endif // FUJI_LIVE_VIEW_STREAM_H
