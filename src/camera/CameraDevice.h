#ifndef CAMERA_DEVICE_H
#define CAMERA_DEVICE_H

#include <Arduino.h>
#include <IPAddress.h>
#include <vector>

enum class CameraBrand {
    UNKNOWN,
    FUJIFILM,
    SONY,
    CANON,
    NIKON,
    PANASONIC
};

enum class CameraStatus {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    SESSION_OPENED,
    CAPTURING,
    ERROR_STATE
};

class CameraDevice {
public:
    virtual ~CameraDevice() = default;

    virtual CameraBrand getBrand() const = 0;
    virtual String getBrandName() const = 0;
    virtual String getModelName() const = 0;

    virtual bool connect(const IPAddress& ip, uint16_t port = 15740) = 0;
    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;
    virtual CameraStatus getStatus() const = 0;

    virtual bool triggerShutter() = 0;
    virtual bool startLiveView() = 0;
    virtual bool stopLiveView() = 0;
    virtual bool getLiveViewFrame(std::vector<uint8_t>& outJpeg) = 0;

    virtual void update() = 0;
};

#endif // CAMERA_DEVICE_H
