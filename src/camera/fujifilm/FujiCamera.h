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

    bool syncProperties() override;
    const ExposureState& getExposureState() const override { return m_exposureState; }
    bool setPropertyValue(ExposurePropertyId propId, uint32_t value) override;
    bool adjustPropertyStep(ExposurePropertyId propId, int stepDelta) override;

    void update() override;

private:
    PtpIpClient m_ptp;
    CameraStatus m_status = CameraStatus::DISCONNECTED;
    String m_modelName = "Fuji Camera";
    ExposureState m_exposureState;
    bool m_liveViewActive = false;
    unsigned long m_lastPropertySync = 0;
};

#endif // FUJI_CAMERA_H
