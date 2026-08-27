#include "FujiCamera.h"
#include <WiFiClient.h>

// Fuji port definitions (from fujiptp.h)
#define FUJI_CMD_IP_PORT      55740
#define FUJI_EVENT_IP_PORT    55741
#define FUJI_LIVEVIEW_IP_PORT 55742

FujiCamera::FujiCamera() {
    // Populate standard photographic exposure steps
    // ISO values
    m_exposureState.iso.allowedValues = { 0, 125, 160, 200, 250, 320, 400, 500, 640, 800, 1000, 1250, 1600, 2000, 2500, 3200, 6400, 12800 };
    for (auto val : m_exposureState.iso.allowedValues) {
        m_exposureState.iso.allowedFormatted.push_back(formatISO(val));
    }

    // Aperture values (stored as f-number * 100, e.g. 140 = f/1.4, 280 = f/2.8)
    m_exposureState.aperture.allowedValues = { 100, 140, 180, 200, 280, 350, 400, 560, 800, 1100, 1600, 2200 };
    for (auto val : m_exposureState.aperture.allowedValues) {
        m_exposureState.aperture.allowedFormatted.push_back(formatAperture(val));
    }

    // Shutter speed values (stored in microseconds: 1/8000s = 125us, 1/1000s = 1000us, 1/250s = 4000us, 1/60s = 16666us, 1s = 1000000us)
    m_exposureState.shutterSpeed.allowedValues = { 125, 250, 500, 1000, 2000, 4000, 8000, 16666, 33333, 66666, 125000, 250000, 500000, 1000000 };
    for (auto val : m_exposureState.shutterSpeed.allowedValues) {
        m_exposureState.shutterSpeed.allowedFormatted.push_back(formatShutter(val));
    }

    // EV compensation values (stored as EV * 1000: -3000 to +3000 in 1/3 EV steps)
    m_exposureState.ev.allowedValues = { (uint32_t)-3000, (uint32_t)-2000, (uint32_t)-1000, 0, 1000, 2000, 3000 };
    for (auto val : m_exposureState.ev.allowedValues) {
        m_exposureState.ev.allowedFormatted.push_back(formatEV((int32_t)val));
    }
}

FujiCamera::~FujiCamera() {
    disconnect();
}

bool FujiCamera::connect(const IPAddress& ip, uint16_t port) {
    m_status = CameraStatus::CONNECTING;

    // Candidate IPs to try: Gateway IP, 192.168.0.1, 192.168.133.1
    std::vector<IPAddress> candidateIPs = { ip };
    IPAddress fujiDefault(192, 168, 0, 1);
    if (ip != fujiDefault) {
        candidateIPs.push_back(fujiDefault);
    }
    IPAddress fujiSubnet(192, 168, 133, 1);
    if (ip != fujiSubnet) {
        candidateIPs.push_back(fujiSubnet);
    }

    // Fuji cameras listen on TCP 55740 for Wi-Fi PTP/IP, or 15740
    std::vector<uint16_t> candidatePorts = { 55740, 15740 };
    if (port != 15740 && port != 55740) {
        candidatePorts.insert(candidatePorts.begin(), port);
    }

    bool tcpConnected = false;
    IPAddress activeIP;
    uint16_t activePort = 0;

    for (const auto& targetIP : candidateIPs) {
        for (uint16_t targetPort : candidatePorts) {
            Serial.printf("[FujiCamera] Connecting TCP to %s:%d...\n", targetIP.toString().c_str(), targetPort);
            if (m_ptp.connect(targetIP, targetPort, 2000)) {
                Serial.printf("[FujiCamera] TCP Connected successfully to %s:%d!\n", targetIP.toString().c_str(), targetPort);
                tcpConnected = true;
                activeIP = targetIP;
                activePort = targetPort;
                break;
            }
            delay(100);
        }
        if (tcpConnected) break;
    }

    if (!tcpConnected) {
        Serial.println("[FujiCamera] TCP Connection failed on all candidate endpoints.");
        m_status = CameraStatus::ERROR_STATE;
        return false;
    }

    // 2. Fuji PTP/IP Init Command Request (proprietary format)
    Serial.println("[FujiCamera] Sending Fuji Init Command Request...");
    bool initOk = m_ptp.sendFujiInitCommandRequest("HackedClient");
    if (!initOk) {
        Serial.println("[FujiCamera] Init Command Request failed!");
        m_ptp.disconnect();
        m_status = CameraStatus::ERROR_STATE;
        return false;
    }
    Serial.println("[FujiCamera] Init Command Request ACKed!");

    // 3. Open Event Connection (required by Fuji before operations)
    Serial.println("[FujiCamera] Opening Event Connection...");
    if (!openEventConnection(ip, FUJI_EVENT_IP_PORT)) {
        Serial.println("[FujiCamera] Event connection failed, trying command port...");
        // Some Fuji cameras use same port for events
        if (!openEventConnection(ip, FUJI_CMD_IP_PORT)) {
            Serial.println("[FujiCamera] Event connection failed on all ports!");
            m_ptp.disconnect();
            m_status = CameraStatus::ERROR_STATE;
            return false;
        }
    }
    Serial.println("[FujiCamera] Event Connection established!");

    // 4. Open PTP Session
    Serial.println("[FujiCamera] Sending Open Session (SessionID = 1)...");
    if (!m_ptp.sendOpenSession(1)) {
        Serial.println("[FujiCamera] Open Session failed!");
        m_ptp.disconnect();
        m_status = CameraStatus::ERROR_STATE;
        return false;
    }
    Serial.println("[FujiCamera] Session Opened successfully!");

    m_status = CameraStatus::SESSION_OPENED;
    
    // Initial parameter synchronization
    syncProperties();

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

    uint16_t respCode = 0;
    std::vector<uint32_t> params = { 0x00000000 };
    bool ok = m_ptp.executeOperation(PTP_OC_InitiateCapture, params, nullptr, nullptr, &respCode);
    if (!ok || respCode != PTP_RC_OK) {
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

bool FujiCamera::syncProperties() {
    if (!isConnected()) return false;

    // Read DevicePropValue for ISO, Aperture, Shutter, EV
    std::vector<ExposurePropertyId> props = {
        ExposurePropertyId::ISO,
        ExposurePropertyId::APERTURE,
        ExposurePropertyId::SHUTTER_SPEED,
        ExposurePropertyId::EXPOSURE_COMPENSATION
    };

    for (auto propId : props) {
        std::vector<uint8_t> data;
        uint16_t respCode = 0;
        bool ok = m_ptp.executeOperation(PTP_OC_GetDevicePropValue, { (uint32_t)propId }, &data, nullptr, &respCode);
        if (ok && respCode == PTP_RC_OK && !data.empty()) {
            uint32_t val = 0;
            if (data.size() == 1) val = data[0];
            else if (data.size() == 2) memcpy(&val, data.data(), 2);
            else if (data.size() >= 4) memcpy(&val, data.data(), 4);

            switch (propId) {
                case ExposurePropertyId::ISO:
                    m_exposureState.iso.currentValue = val;
                    m_exposureState.iso.currentFormatted = formatISO(val);
                    break;
                case ExposurePropertyId::APERTURE:
                    m_exposureState.aperture.currentValue = val;
                    m_exposureState.aperture.currentFormatted = formatAperture(val);
                    break;
                case ExposurePropertyId::SHUTTER_SPEED:
                    m_exposureState.shutterSpeed.currentValue = val;
                    m_exposureState.shutterSpeed.currentFormatted = formatShutter(val);
                    break;
                case ExposurePropertyId::EXPOSURE_COMPENSATION:
                    m_exposureState.ev.currentValue = val;
                    m_exposureState.ev.currentFormatted = formatEV((int32_t)val);
                    break;
                default:
                    break;
            }
        }
    }
    return true;
}

bool FujiCamera::setPropertyValue(ExposurePropertyId propId, uint32_t value) {
    if (!isConnected()) return false;

    uint16_t respCode = 0;
    // SetDevicePropValue
    bool ok = m_ptp.executeOperation(PTP_OC_SetDevicePropValue, { (uint32_t)propId, value }, nullptr, nullptr, &respCode);
    if (!ok || respCode != PTP_RC_OK) {
        // Fallback to Fuji Vendor property set
        ok = m_ptp.executeOperation(FUJI_OC_SetDevicePropValueEx, { (uint32_t)propId, value }, nullptr, nullptr, &respCode);
    }

    if (ok && respCode == PTP_RC_OK) {
        // Update local cache
        switch (propId) {
            case ExposurePropertyId::ISO:
                m_exposureState.iso.currentValue = value;
                m_exposureState.iso.currentFormatted = formatISO(value);
                break;
            case ExposurePropertyId::APERTURE:
                m_exposureState.aperture.currentValue = value;
                m_exposureState.aperture.currentFormatted = formatAperture(value);
                break;
            case ExposurePropertyId::SHUTTER_SPEED:
                m_exposureState.shutterSpeed.currentValue = value;
                m_exposureState.shutterSpeed.currentFormatted = formatShutter(value);
                break;
            case ExposurePropertyId::EXPOSURE_COMPENSATION:
                m_exposureState.ev.currentValue = value;
                m_exposureState.ev.currentFormatted = formatEV((int32_t)value);
                break;
            default:
                break;
        }
        return true;
    }
    return false;
}

bool FujiCamera::adjustPropertyStep(ExposurePropertyId propId, int stepDelta) {
    CameraProperty* targetProp = nullptr;
    switch (propId) {
        case ExposurePropertyId::ISO: targetProp = &m_exposureState.iso; break;
        case ExposurePropertyId::APERTURE: targetProp = &m_exposureState.aperture; break;
        case ExposurePropertyId::SHUTTER_SPEED: targetProp = &m_exposureState.shutterSpeed; break;
        case ExposurePropertyId::EXPOSURE_COMPENSATION: targetProp = &m_exposureState.ev; break;
        default: return false;
    }

    if (targetProp->allowedValues.empty()) return false;

    // Find current index
    int idx = 0;
    for (size_t i = 0; i < targetProp->allowedValues.size(); ++i) {
        if (targetProp->allowedValues[i] == targetProp->currentValue) {
            idx = (int)i;
            break;
        }
    }

    int newIdx = idx + stepDelta;
    if (newIdx < 0) newIdx = 0;
    if (newIdx >= (int)targetProp->allowedValues.size()) newIdx = targetProp->allowedValues.size() - 1;

    uint32_t nextVal = targetProp->allowedValues[newIdx];
    return setPropertyValue(propId, nextVal);
}

void FujiCamera::update() {
    if (isConnected() && millis() - m_lastPropertySync > 3000) {
        m_lastPropertySync = millis();
        syncProperties();
    }
}

bool FujiCamera::openEventConnection(const IPAddress& ip, uint16_t port) {
    Serial.printf("[FujiCamera] Connecting Event TCP to %s:%u...\n", ip.toString().c_str(), port);
    
    m_eventClient.setTimeout(5);
    if (!m_eventClient.connect(ip, port, 5000)) {
        Serial.printf("[FujiCamera] Event TCP connection failed to %s:%u\n", ip.toString().c_str(), port);
        return false;
    }
    Serial.printf("[FujiCamera] Event TCP connected to %s:%u\n", ip.toString().c_str(), port);

    // Build Init_Event_Request packet
    // Standard PTP/IP: [length:4][type:4=3][connectionNumber:4] = 12 bytes total
    // But Fuji may expect the same proprietary format...
    // Try standard format first (simpler)
    uint32_t connNum = m_ptp.getConnectionNumber();
    
    // Packet: [length:4][type:4][connectionNumber:4]
    uint32_t pktLen = 12;
    uint32_t pktType = PTP_PKT_INIT_EVENT_REQ;  // type = 3
    
    Serial.printf("[FujiCamera] Sending Init_Event_Request (connNum=0x%08X)...\n", connNum);
    
    if (m_eventClient.write((const uint8_t*)&pktLen, 4) != 4 ||
        m_eventClient.write((const uint8_t*)&pktType, 4) != 4 ||
        m_eventClient.write((const uint8_t*)&connNum, 4) != 4) {
        Serial.println("[FujiCamera] Failed to send Init_Event_Request!");
        m_eventClient.stop();
        return false;
    }

    // Read response header (8 bytes: length + type)
    uint8_t respHeader[8] = {0};
    size_t totalRead = 0;
    unsigned long start = millis();
    while (totalRead < 8) {
        if (millis() - start > 10000) {
            Serial.printf("[FujiCamera] Event response timeout! Read %u of 8 bytes\n", (unsigned)totalRead);
            m_eventClient.stop();
            return false;
        }
        int avail = m_eventClient.available();
        if (avail > 0) {
            int n = m_eventClient.read(respHeader + totalRead, std::min((size_t)avail, 8 - totalRead));
            if (n > 0) totalRead += n;
        } else if (!m_eventClient.connected()) {
            Serial.println("[FujiCamera] Event client disconnected while reading!");
            return false;
        } else {
            delay(10);
        }
    }
    
    uint32_t respLen = 0, respType = 0;
    memcpy(&respLen, respHeader, 4);
    memcpy(&respType, respHeader + 4, 4);
    
    Serial.printf("[FujiCamera] Event response: len=%u, type=%u\n", respLen, respType);
    
    // Read remaining payload if any
    if (respLen > 8) {
        size_t payloadLen = respLen - 8;
        std::vector<uint8_t> payload(payloadLen);
        totalRead = 0;
        start = millis();
        while (totalRead < payloadLen) {
            if (millis() - start > 5000) break;
            int avail = m_eventClient.available();
            if (avail > 0) {
                int n = m_eventClient.read(payload.data() + totalRead, std::min((size_t)avail, payloadLen - totalRead));
                if (n > 0) totalRead += n;
            } else {
                delay(10);
            }
        }
        Serial.print("[FujiCamera] Event payload hex: ");
        for (size_t i = 0; i < totalRead && i < 20; ++i) {
            Serial.printf("%02X ", payload[i]);
        }
        Serial.println();
    }
    
    if (respType == PTP_PKT_INIT_EVENT_ACK) {
        Serial.println("[FujiCamera] Init_Event_Request ACCEPTED!");
        return true;
    } else if (respType == PTP_PKT_INIT_FAIL) {
        Serial.println("[FujiCamera] Init_Event_Request REJECTED!");
        m_eventClient.stop();
        return false;
    }
    
    Serial.printf("[FujiCamera] Unexpected event response type=%u\n", respType);
    // Keep connection open even for unexpected type - might still work
    return true;
}
