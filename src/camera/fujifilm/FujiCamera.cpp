#include "FujiCamera.h"
#include <WiFiClient.h>

// Fuji port definitions (from fujiptp.h)
#define FUJI_CMD_IP_PORT      55740
#define FUJI_EVENT_IP_PORT    55741
#define FUJI_LIVEVIEW_IP_PORT 55742

FujiCamera::FujiCamera() {
    // Default exposure values fallback
    m_exposureState.iso.allowedValues = { 0, 100, 200, 400, 800, 1600, 3200, 6400, 12800, 25600 };
    for (auto val : m_exposureState.iso.allowedValues) {
        m_exposureState.iso.allowedFormatted.push_back(formatISO(val));
    }
    m_exposureState.iso.currentValue = 200;
    m_exposureState.iso.currentFormatted = formatISO(200);

    m_exposureState.aperture.allowedValues = { 100, 140, 200, 280, 400, 560, 800, 1100, 1600, 2200 };
    for (auto val : m_exposureState.aperture.allowedValues) {
        m_exposureState.aperture.allowedFormatted.push_back(formatAperture(val));
    }
    m_exposureState.aperture.currentValue = 280;
    m_exposureState.aperture.currentFormatted = formatAperture(280);

    m_exposureState.shutterSpeed.allowedValues = { 
        (uint32_t)(0x80000000 | 4000000), // 1/4000s
        (uint32_t)(0x80000000 | 2000000), // 1/2000s
        (uint32_t)(0x80000000 | 1000000), // 1/1000s
        (uint32_t)(0x80000000 | 500000),  // 1/500s
        (uint32_t)(0x80000000 | 250000),  // 1/250s
        (uint32_t)(0x80000000 | 125000),  // 1/125s
        (uint32_t)(0x80000000 | 60000),   // 1/60s
        (uint32_t)(0x80000000 | 30000),   // 1/30s
        (uint32_t)(0x80000000 | 15000),   // 1/15s
        1000,                             // 1s
        2000                              // 2s
    };
    for (auto val : m_exposureState.shutterSpeed.allowedValues) {
        m_exposureState.shutterSpeed.allowedFormatted.push_back(formatShutter(val));
    }
    m_exposureState.shutterSpeed.currentValue = 0x80000000 | 250000;
    m_exposureState.shutterSpeed.currentFormatted = formatShutter(0x80000000 | 250000);

    m_exposureState.ev.allowedValues = { (uint32_t)-3000, (uint32_t)-2000, (uint32_t)-1000, 0, 1000, 2000, 3000 };
    for (auto val : m_exposureState.ev.allowedValues) {
        m_exposureState.ev.allowedFormatted.push_back(formatEV((int32_t)val));
    }
    m_exposureState.ev.currentValue = 0;
    m_exposureState.ev.currentFormatted = formatEV(0);
}

FujiCamera::~FujiCamera() {
    disconnect();
}

bool FujiCamera::connect(const IPAddress& ip, uint16_t port) {
    m_status = CameraStatus::CONNECTING;

    // Candidate IPs to try: Gateway IP, 192.168.0.1
    std::vector<IPAddress> candidateIPs = { ip };
    IPAddress fujiDefault(192, 168, 0, 1);
    if (ip != fujiDefault) {
        candidateIPs.push_back(fujiDefault);
    }

    bool tcpConnected = false;
    for (const auto& targetIP : candidateIPs) {
        Serial.printf("[FujiCamera] Connecting TCP to %s:%d...\n", targetIP.toString().c_str(), FUJI_CMD_IP_PORT);
        if (m_ptp.connect(targetIP, FUJI_CMD_IP_PORT, 2000)) {
            Serial.printf("[FujiCamera] TCP Connected successfully to %s:%d!\n", targetIP.toString().c_str(), FUJI_CMD_IP_PORT);
            tcpConnected = true;
            m_cameraIp = targetIP;
            break;
        }
        delay(100);
    }

    if (!tcpConnected) {
        Serial.println("[FujiCamera] TCP Connection failed on all candidate endpoints.");
        m_status = CameraStatus::ERROR_STATE;
        return false;
    }

    // Step 1: Fuji PTP/IP Init Command Request (82 bytes)
    Serial.println("[FujiCamera] Step 1: Sending Fuji Init Command Request...");
    if (!m_ptp.sendFujiInitCommandRequest("HackedClient")) {
        Serial.println("[FujiCamera] Init Command Request failed!");
        m_ptp.disconnect();
        m_status = CameraStatus::ERROR_STATE;
        return false;
    }
    Serial.println("[FujiCamera] Step 1: Init ACKed!");
    delay(50); // Small pause per libfuji/fuji-cam-wifi-tool

    // Step 2: Open Session (16 bytes, OpCode 0x1002, SessionID = 1)
    Serial.println("[FujiCamera] Step 2: Sending Open Session (0x1002)...");
    if (!m_ptp.sendOpenSession(1)) {
        Serial.println("[FujiCamera] Step 2: Open Session failed!");
        m_ptp.disconnect();
        m_status = CameraStatus::ERROR_STATE;
        return false;
    }
    Serial.println("[FujiCamera] Step 2: Session Opened successfully!");

    // Step 3: Two-part Set Mode (0xDF01 -> 0x0005 = Remote Mode)
    Serial.println("[FujiCamera] Step 3: Setting Remote Mode (0xDF01 -> 5)...");
    std::vector<uint8_t> modePart1 = { 0x01, 0xDF, 0x00, 0x00 };
    std::vector<uint8_t> modePart2 = { 0x05, 0x00 };
    uint16_t respCode = 0;
    if (!m_ptp.executeFujiTwoPartOperation(PTP_OC_SetDevicePropValue, modePart1, modePart2, &respCode)) {
        Serial.printf("[FujiCamera] Step 3: Warning - Set Mode returned 0x%04X\n", respCode);
    }

    // Step 4: Get Remote Version (0xDF24)
    Serial.println("[FujiCamera] Step 4: Getting Remote Version (0xDF24)...");
    std::vector<uint8_t> verData;
    std::vector<uint8_t> getVerPayload = { 0x24, 0xDF, 0x00, 0x00 };
    m_ptp.executeFujiOperation(PTP_OC_GetDevicePropValue, getVerPayload, &verData, &respCode);

    // Step 5: Set Remote Version (0xDF24 -> 2)
    Serial.println("[FujiCamera] Step 5: Setting Remote Version (0xDF24 -> 2)...");
    std::vector<uint8_t> setVerPart1 = { 0x24, 0xDF, 0x00, 0x00 };
    std::vector<uint8_t> setVerPart2 = { 0xFF, 0x00, 0x02, 0x00 };
    m_ptp.executeFujiTwoPartOperation(PTP_OC_SetDevicePropValue, setVerPart1, setVerPart2, &respCode);

    // Step 6: Get Camera Capabilities (0x902B)
    Serial.println("[FujiCamera] Step 6: Getting Camera Capabilities (0x902B)...");
    std::vector<uint8_t> capsData;
    if (m_ptp.executeFujiOperation(FUJI_OC_StartLiveView /* 0x902B */, {}, &capsData, &respCode)) {
        Serial.printf("[FujiCamera] Step 6: Got %u bytes of capabilities data!\n", (unsigned)capsData.size());
        if (!capsData.empty()) {
            parseCapabilities(capsData.data(), capsData.size());
        }
    }

    // Step 7: Initiate Remote Control (0x101C)
    Serial.println("[FujiCamera] Step 7: Initiating Remote Control (0x101C)...");
    std::vector<uint8_t> remoteInitPayload = { 0, 0, 0, 0, 0, 0, 0, 0 };
    if (!m_ptp.executeFujiOperation(PTP_OC_InitiateOpenCapture /* 0x101C */, remoteInitPayload, nullptr, &respCode)) {
        Serial.printf("[FujiCamera] Step 7: Warning - Initiate Remote Control returned 0x%04X\n", respCode);
    }

    Serial.println("[FujiCamera] *** FULLY CONNECTED IN REMOTE CONTROL MODE! ***");
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

    Serial.println("[FujiCamera] Triggering Shutter (0x100E)...");
    std::vector<uint8_t> shutterPayload = { 0, 0, 0, 0, 0, 0, 0, 0 };
    uint16_t respCode = 0;
    bool ok = m_ptp.executeFujiOperation(PTP_OC_InitiateCapture, shutterPayload, nullptr, &respCode);
    Serial.printf("[FujiCamera] Shutter response: ok=%d, code=0x%04X\n", ok, respCode);
    return ok && (respCode == PTP_RC_OK);
}

bool FujiCamera::startLiveView() {
    if (!isConnected()) return false;

    if (m_liveViewActive && m_liveViewClient.connected()) {
        return true;
    }

    Serial.printf("[FujiCamera] Connecting Live View stream to %s:%d...\n", m_cameraIp.toString().c_str(), FUJI_LIVEVIEW_IP_PORT);
    if (!m_liveViewClient.connect(m_cameraIp, FUJI_LIVEVIEW_IP_PORT, 2000)) {
        Serial.println("[FujiCamera] Failed to connect Live View socket!");
        return false;
    }

    Serial.println("[FujiCamera] Live View stream connected!");
    m_liveViewActive = true;
    return true;
}

bool FujiCamera::stopLiveView() {
    if (m_liveViewClient.connected()) {
        m_liveViewClient.stop();
    }
    m_liveViewActive = false;
    return true;
}

bool FujiCamera::getLiveViewFrame(std::vector<uint8_t>& outJpeg) {
    if (!isConnected() || !m_liveViewActive || !m_liveViewClient.connected()) {
        return false;
    }

    if (!m_liveViewClient.available()) {
        return false;
    }

    // Read 4 bytes total length prefix
    uint32_t totalLen = 0;
    size_t r = m_liveViewClient.readBytes((char*)&totalLen, sizeof(totalLen));
    if (r != sizeof(totalLen) || totalLen < 18 || totalLen > 500000) {
        return false;
    }

    size_t payloadLen = totalLen - sizeof(totalLen);
    std::vector<uint8_t> buffer(payloadLen);
    size_t totalRead = 0;
    unsigned long start = millis();

    while (totalRead < payloadLen && millis() - start < 1000) {
        int avail = m_liveViewClient.available();
        if (avail > 0) {
            size_t toRead = std::min((size_t)avail, payloadLen - totalRead);
            int n = m_liveViewClient.read(buffer.data() + totalRead, toRead);
            if (n > 0) totalRead += n;
        } else {
            delay(2);
        }
    }

    if (totalRead != payloadLen) {
        return false;
    }

    // First 14 bytes are Fuji frame header (e.g. frame counter), remainder is JPEG
    if (payloadLen > 14) {
        outJpeg.assign(buffer.begin() + 14, buffer.end());
        return true;
    }

    return false;
}

void FujiCamera::parseCapabilities(const uint8_t* data, size_t len) {
    if (len < 12) return;

    const uint8_t* ptr = data + 12; // Skip 12 bytes header
    size_t remaining = len - 12;

    while (remaining >= 4) {
        uint32_t subMsgLen = 0;
        memcpy(&subMsgLen, ptr, 4);
        if (subMsgLen < 4 || subMsgLen > remaining) break;

        const uint8_t* subPtr = ptr + 4;
        size_t subPayloadLen = subMsgLen - 4;

        if (subPayloadLen >= 5) {
            uint16_t propCode = 0;
            uint16_t dataType = 0;
            uint8_t getSet = 0;
            memcpy(&propCode, subPtr, 2);
            memcpy(&dataType, subPtr + 2, 2);
            getSet = subPtr[4];

            size_t valSize = 2;
            if (dataType == 1 || dataType == 2) valSize = 1;
            else if (dataType == 5 || dataType == 6) valSize = 4;

            size_t offset = 5;
            uint32_t defaultVal = 0, currentVal = 0;
            if (offset + valSize * 2 <= subPayloadLen) {
                memcpy(&defaultVal, subPtr + offset, valSize);
                offset += valSize;
                memcpy(&currentVal, subPtr + offset, valSize);
                offset += valSize;
            }

            uint8_t formFlag = 0;
            if (offset < subPayloadLen) {
                formFlag = subPtr[offset++];
            }

            std::vector<uint32_t> allowedList;
            if (formFlag == 2 && offset + 2 <= subPayloadLen) {
                uint16_t count = 0;
                memcpy(&count, subPtr + offset, 2);
                offset += 2;
                for (uint16_t i = 0; i < count && (offset + valSize) <= subPayloadLen; i++) {
                    uint32_t v = 0;
                    memcpy(&v, subPtr + offset, valSize);
                    allowedList.push_back(v);
                    offset += valSize;
                }
            }

            // Map to exposure state
            if (propCode == FUJI_DPC_ISO) {
                m_exposureState.iso.currentValue = currentVal;
                m_exposureState.iso.currentFormatted = formatISO(currentVal);
                if (!allowedList.empty()) {
                    m_exposureState.iso.allowedValues = allowedList;
                    m_exposureState.iso.allowedFormatted.clear();
                    for (auto v : allowedList) m_exposureState.iso.allowedFormatted.push_back(formatISO(v));
                }
            } else if (propCode == FUJI_DPC_Aperture) {
                m_exposureState.aperture.currentValue = currentVal;
                m_exposureState.aperture.currentFormatted = formatAperture(currentVal);
                if (!allowedList.empty()) {
                    m_exposureState.aperture.allowedValues = allowedList;
                    m_exposureState.aperture.allowedFormatted.clear();
                    for (auto v : allowedList) m_exposureState.aperture.allowedFormatted.push_back(formatAperture(v));
                }
            } else if (propCode == FUJI_DPC_ShutterSpeed) {
                m_exposureState.shutterSpeed.currentValue = currentVal;
                m_exposureState.shutterSpeed.currentFormatted = formatShutter(currentVal);
                if (!allowedList.empty()) {
                    m_exposureState.shutterSpeed.allowedValues = allowedList;
                    m_exposureState.shutterSpeed.allowedFormatted.clear();
                    for (auto v : allowedList) m_exposureState.shutterSpeed.allowedFormatted.push_back(formatShutter(v));
                }
            } else if (propCode == FUJI_DPC_ExposureCompensation) {
                m_exposureState.ev.currentValue = currentVal;
                m_exposureState.ev.currentFormatted = formatEV((int32_t)currentVal);
                if (!allowedList.empty()) {
                    m_exposureState.ev.allowedValues = allowedList;
                    m_exposureState.ev.allowedFormatted.clear();
                    for (auto v : allowedList) m_exposureState.ev.allowedFormatted.push_back(formatEV((int32_t)v));
                }
            }
        }

        ptr += subMsgLen;
        remaining -= subMsgLen;
    }
}

bool FujiCamera::syncProperties() {
    if (!isConnected()) return false;

    // Query individual properties via 0x1015 (GetDevicePropValue)
    struct PropQuery {
        ExposurePropertyId id;
        uint16_t fujiProp;
    };
    PropQuery queries[] = {
        { ExposurePropertyId::ISO, FUJI_DPC_ISO },
        { ExposurePropertyId::APERTURE, FUJI_DPC_Aperture },
        { ExposurePropertyId::SHUTTER_SPEED, FUJI_DPC_ShutterSpeed },
        { ExposurePropertyId::EXPOSURE_COMPENSATION, FUJI_DPC_ExposureCompensation }
    };

    for (const auto& q : queries) {
        std::vector<uint8_t> propPayload = { (uint8_t)(q.fujiProp & 0xFF), (uint8_t)(q.fujiProp >> 8), 0, 0 };
        std::vector<uint8_t> outVal;
        uint16_t rCode = 0;
        if (m_ptp.executeFujiOperation(PTP_OC_GetDevicePropValue, propPayload, &outVal, &rCode) && rCode == PTP_RC_OK && !outVal.empty()) {
            uint32_t val = 0;
            if (outVal.size() >= 4) memcpy(&val, outVal.data(), 4);
            else if (outVal.size() == 2) memcpy(&val, outVal.data(), 2);
            else if (outVal.size() == 1) val = outVal[0];

            switch (q.id) {
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
                default: break;
            }
        }
    }
    return true;
}

bool FujiCamera::setPropertyValue(ExposurePropertyId propId, uint32_t value) {
    if (!isConnected()) return false;

    uint16_t fujiProp = 0;
    size_t valBytes = 4;
    switch (propId) {
        case ExposurePropertyId::ISO:
            fujiProp = FUJI_DPC_ISO;
            valBytes = 4;
            break;
        case ExposurePropertyId::APERTURE:
            fujiProp = FUJI_DPC_Aperture;
            valBytes = 2;
            break;
        case ExposurePropertyId::SHUTTER_SPEED:
            fujiProp = FUJI_DPC_ShutterSpeed;
            valBytes = 4;
            break;
        case ExposurePropertyId::EXPOSURE_COMPENSATION:
            fujiProp = FUJI_DPC_ExposureCompensation;
            valBytes = 4;
            break;
        default:
            return false;
    }

    std::vector<uint8_t> part1(4);
    memcpy(part1.data(), &fujiProp, 2);

    std::vector<uint8_t> part2(valBytes);
    memcpy(part2.data(), &value, valBytes);

    uint16_t respCode = 0;
    bool ok = m_ptp.executeFujiTwoPartOperation(PTP_OC_SetDevicePropValue, part1, part2, &respCode);
    if (ok && respCode == PTP_RC_OK) {
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
    if (isConnected() && millis() - m_lastPropertySync > 5000) {
        m_lastPropertySync = millis();
        syncProperties();
    }
}



