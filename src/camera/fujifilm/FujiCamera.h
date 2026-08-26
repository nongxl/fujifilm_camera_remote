#ifndef FUJI_CAMERA_H
#define FUJI_CAMERA_H

#include "../CameraDevice.h"
#include "../../ptp/PtpIpClient.h"

class FujiCamera : public CameraDevice {
public:
    FujiCamera();
    virtual ~FujiCamera();

    CameraBrand getBrand() const override { return CameraBrand::FUJIFILM; }
    String getBrandName() const override { return "Fujifilm"; }
    String getModelName() const override { return m_modelName; }

    bool connect(const IPAddress& ip, uint16_t port = 15740) override;
    void disconnect() override;
    bool isConnected() const override;
    CameraStatus getStatus() const override { return m_status; }

    bool triggerShutter() override;
    bool startLiveView() override;
    bool stopLiveView() override;
    bool getLiveViewFrame(std::vector<uint8_t>& outJpeg) override;

    void update() override;

private:
    PtpIpClient m_ptp;
    CameraStatus m_status = CameraStatus::DISCONNECTED;
    String m_modelName = "Fuji Camera";
    bool m_liveViewActive = false;
};

#endif // FUJI_CAMERA_H
