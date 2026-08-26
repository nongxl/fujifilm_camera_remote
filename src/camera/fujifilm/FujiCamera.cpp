#include "FujiCamera.h"

FujiCamera::FujiCamera() {
}

FujiCamera::~FujiCamera() {
    disconnect();
}

bool FujiCamera::connect(const IPAddress& ip, uint16_t port) {
    m_status = CameraStatus::CONNECTING;
    
    // 1. TCP Connection
    if (!m_ptp.connect(ip, port, 5000)) {
        m_status = CameraStatus::ERROR_STATE;
        return false;
    }

    // 2. PTP/IP Init Command Request
    if (!m_ptp.sendInitCommandRequest("M5StickS3 Remote")) {
        m_ptp.disconnect();
        m_status = CameraStatus::ERROR_STATE;
        return false;
    }

    // 3. Open PTP Session
    if (!m_ptp.sendOpenSession(1)) {
        m_ptp.disconnect();
        m_status = CameraStatus::ERROR_STATE;
        return false;
    }

    m_status = CameraStatus::SESSION_OPENED;
    return true;
}

void FujiCamera::disconnect() {
    if (m_status == CameraStatus::SESSION_OPENED) {
        if (m_liveViewActive) {
            stopLiveView();
        }
        m_ptp.sendCloseSession();
    }
    m_ptp.disconnect();
    m_status = CameraStatus::DISCONNECTED;
    m_liveViewActive = false;
}

bool FujiCamera::isConnected() const {
    return m_status == CameraStatus::SESSION_OPENED;
}

bool FujiCamera::triggerShutter() {
    if (!isConnected()) return false;

    // Send Fujifilm capture command (or standard PTP InitiateCapture)
    uint16_t respCode = 0;
    std::vector<uint32_t> params = { 0x00000000 };
    bool ok = m_ptp.executeOperation(PTP_OC_InitiateCapture, params, nullptr, nullptr, &respCode);
    if (!ok || respCode != PTP_RC_OK) {
        // Fallback to Fuji vendor code
        ok = m_ptp.executeOperation(FUJI_OC_InitiateCapture, params, nullptr, nullptr, &respCode);
    }
    return ok && (respCode == PTP_RC_OK);
}

bool FujiCamera::startLiveView() {
    if (!isConnected()) return false;

    uint16_t respCode = 0;
    bool ok = m_ptp.executeOperation(FUJI_OC_StartLiveView, {}, nullptr, nullptr, &respCode);
    if (ok && respCode == PTP_RC_OK) {
        m_liveViewActive = true;
        return true;
    }
    return false;
}

bool FujiCamera::stopLiveView() {
    if (!isConnected()) return false;

    uint16_t respCode = 0;
    bool ok = m_ptp.executeOperation(FUJI_OC_StopLiveView, {}, nullptr, nullptr, &respCode);
    m_liveViewActive = false;
    return ok;
}

bool FujiCamera::getLiveViewFrame(std::vector<uint8_t>& outJpeg) {
    if (!isConnected() || !m_liveViewActive) return false;

    uint16_t respCode = 0;
    bool ok = m_ptp.executeOperation(FUJI_OC_GetLiveViewFrame, {}, &outJpeg, nullptr, &respCode);
    return ok && (respCode == PTP_RC_OK) && !outJpeg.empty();
}

void FujiCamera::update() {
    // Keepalive or polling if needed
}
