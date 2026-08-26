#include "PtpIpClient.h"
#include <WiFi.h>

PtpIpClient::PtpIpClient() {
    // Stable client GUID derived from device MAC address
    uint8_t mac[6] = {0};
    WiFi.macAddress(mac);
    memset(m_guid, 0, 16);
    m_guid[0] = 0x00;
    m_guid[1] = 0x00;
    m_guid[2] = 0x00;
    m_guid[3] = 0x00;
    m_guid[4] = 0x00;
    m_guid[5] = 0x00;
    m_guid[6] = 0x00;
    m_guid[7] = 0x00;
    m_guid[8] = 0xFF;
    m_guid[9] = 0xFF;
    memcpy(m_guid + 10, mac, 6);
}

PtpIpClient::~PtpIpClient() {
    disconnect();
}

bool PtpIpClient::connect(const IPAddress& ip, uint16_t port, uint32_t timeoutMs) {
    disconnect();
    m_client.setTimeout(timeoutMs / 1000);
    if (!m_client.connect(ip, port, timeoutMs)) {
        return false;
    }
    m_transactionId = 0;
    return true;
}

void PtpIpClient::disconnect() {
    if (m_client.connected()) {
        m_client.stop();
    }
    m_sessionId = 0;
    m_connectionNumber = 0;
}

bool PtpIpClient::isConnected() {
    return m_client.connected();
}

bool PtpIpClient::sendPacket(uint32_t type, const uint8_t* payload, size_t payloadLen) {
    if (!isConnected()) return false;

    uint32_t totalLen = sizeof(PtpIpHeader) + payloadLen;
    PtpIpHeader header;
    header.length = totalLen;
    header.type = type;

    if (m_client.write((const uint8_t*)&header, sizeof(header)) != sizeof(header)) {
        return false;
    }

    if (payloadLen > 0 && payload != nullptr) {
        if (m_client.write(payload, payloadLen) != payloadLen) {
            return false;
        }
    }
    return true;
}

bool PtpIpClient::readExact(uint8_t* buffer, size_t len, uint32_t timeoutMs) {
    size_t totalRead = 0;
    unsigned long start = millis();
    while (totalRead < len) {
        if (millis() - start > timeoutMs) {
            Serial.printf("[PTP/IP] readExact timeout! Read %d of %d bytes\n", (int)totalRead, (int)len);
            return false;
        }
        int available = m_client.available();
        if (available > 0) {
            size_t toRead = std::min((size_t)available, len - totalRead);
            int bytesRead = m_client.read(buffer + totalRead, toRead);
            if (bytesRead <= 0) {
                Serial.println("[PTP/IP] Socket closed by remote peer while reading");
                return false;
            }
            totalRead += bytesRead;
        } else {
            if (!m_client.connected()) {
                Serial.printf("[PTP/IP] Client disconnected while reading! Read %d of %d bytes\n", (int)totalRead, (int)len);
                return false;
            }
            delay(5);
        }
    }
    return true;
}

bool PtpIpClient::readPacket(uint32_t& outType, std::vector<uint8_t>& outPayload, uint32_t timeoutMs) {
    PtpIpHeader header;
    if (!readExact((uint8_t*)&header, sizeof(header), timeoutMs)) {
        return false;
    }

    Serial.printf("[PTP/IP] Rx Packet Header: len=%u, type=%u\n", header.length, header.type);

    if (header.length < sizeof(PtpIpHeader)) {
        Serial.printf("[PTP/IP] Invalid packet header length: %u\n", header.length);
        return false;
    }

    outType = header.type;
    size_t payloadLen = header.length - sizeof(PtpIpHeader);
    outPayload.resize(payloadLen);

    if (payloadLen > 0) {
        if (!readExact(outPayload.data(), payloadLen, timeoutMs)) {
            return false;
        }
    }
    return true;
}

bool PtpIpClient::sendInitCommandRequest(const String& clientName) {
    std::vector<uint8_t> payload;
    // 1. GUID (16 bytes)
    payload.insert(payload.end(), m_guid, m_guid + 16);

    // 2. Friendly Name (UTF-16LE, null terminated)
    for (size_t i = 0; i < clientName.length(); ++i) {
        payload.push_back((uint8_t)clientName[i]);
        payload.push_back(0); // High byte for ASCII in UTF-16LE
    }
    payload.push_back(0); // Null terminator (2 bytes in UTF-16)
    payload.push_back(0);

    // 3. Protocol Version (4 bytes, e.g. 0x00010000 = 1.0)
    uint32_t version = 0x00010000;
    payload.insert(payload.end(), (uint8_t*)&version, (uint8_t*)&version + 4);

    Serial.printf("[PTP/IP] Sending InitCommandReq (%u bytes payload, clientName='%s')...\n", 
                  (unsigned)payload.size(), clientName.c_str());

    if (!sendPacket(PTP_PKT_INIT_CMD_REQ, payload.data(), payload.size())) {
        Serial.println("[PTP/IP] sendPacket failed for InitCommandReq!");
        return false;
    }

    // Wait for InitCommandAck (type = 2) with 10s timeout
    uint32_t respType = 0;
    std::vector<uint8_t> respPayload;
    if (!readPacket(respType, respPayload, 10000)) {
        Serial.println("[PTP/IP] Failed to read InitCommandAck response!");
        return false;
    }

    Serial.printf("[PTP/IP] Got response type=%u, payloadSize=%u\n", respType, (unsigned)respPayload.size());

    if (respPayload.size() >= 4) {
        uint32_t code = 0;
        memcpy(&code, respPayload.data(), 4);
        Serial.printf("[PTP/IP] Response Code/Reason = 0x%08X\n", code);
    }

    if (respType == PTP_PKT_INIT_FAIL) {
        uint32_t reason = 0;
        if (respPayload.size() >= 4) memcpy(&reason, respPayload.data(), 4);
        Serial.printf("[PTP/IP] Init_Command_Request REJECTED by camera (InitFail reason: 0x%08X)!\n", reason);
        if (reason == 1) {
            Serial.println("[PTP/IP] Reason 1 (Rejected Initiator): Camera rejected new device. Please press [OK/Change] on camera screen to pair with new device!");
        } else if (reason == 2) {
            Serial.println("[PTP/IP] Reason 2 (Busy): Camera is busy.");
        }
        return false;
    }

    if (respType != PTP_PKT_INIT_CMD_ACK || respPayload.size() < 4) {
        Serial.printf("[PTP/IP] Unexpected response to InitCommandReq (type=%u)\n", respType);
        return false;
    }

    // Extract Connection Number
    memcpy(&m_connectionNumber, respPayload.data(), 4);
    Serial.printf("[PTP/IP] Connection Number = 0x%08X\n", m_connectionNumber);
    return true;
}

bool PtpIpClient::sendOpenSession(uint32_t sessionId) {
    std::vector<uint32_t> params = { sessionId };
    uint16_t respCode = 0;
    bool success = executeOperation(PTP_OC_OpenSession, params, nullptr, nullptr, &respCode);
    if (success && (respCode == PTP_RC_OK || respCode == PTP_RC_SessionAlreadyOpened)) {
        m_sessionId = sessionId;
        return true;
    }
    return false;
}

bool PtpIpClient::sendCloseSession() {
    uint16_t respCode = 0;
    bool success = executeOperation(PTP_OC_CloseSession, {}, nullptr, nullptr, &respCode);
    m_sessionId = 0;
    return success;
}

bool PtpIpClient::executeOperation(uint16_t opCode, 
                                    const std::vector<uint32_t>& params,
                                    std::vector<uint8_t>* outData,
                                    std::vector<uint32_t>* outResponseParams,
                                    uint16_t* outResponseCode) {
    if (!isConnected()) return false;

    m_transactionId++;
    uint32_t txId = m_transactionId;

    // Build Operation Request Payload
    std::vector<uint8_t> reqPayload;
    uint32_t dataPhase = (outData != nullptr) ? PTP_DATA_PHASE_IN : PTP_DATA_PHASE_NONE;
    
    // Data phase info (4 bytes)
    reqPayload.insert(reqPayload.end(), (uint8_t*)&dataPhase, (uint8_t*)&dataPhase + 4);
    // OpCode (2 bytes)
    reqPayload.insert(reqPayload.end(), (uint8_t*)&opCode, (uint8_t*)&opCode + 2);
    // Transaction ID (4 bytes)
    reqPayload.insert(reqPayload.end(), (uint8_t*)&txId, (uint8_t*)&txId + 4);

    // Parameters (up to 5)
    for (uint32_t p : params) {
        reqPayload.insert(reqPayload.end(), (uint8_t*)&p, (uint8_t*)&p + 4);
    }

    if (!sendPacket(PTP_PKT_OPER_REQ, reqPayload.data(), reqPayload.size())) {
        return false;
    }

    // Loop reading packets until we get OperationResponse (type = 7)
    bool gotResponse = false;
    while (!gotResponse) {
        uint32_t pktType = 0;
        std::vector<uint8_t> pktPayload;
        if (!readPacket(pktType, pktPayload)) {
            return false;
        }

        if (pktType == PTP_PKT_START_DATA) {
            // Start of data stream
            if (outData) {
                outData->clear();
            }
        } else if (pktType == PTP_PKT_DATA || pktType == PTP_PKT_END_DATA) {
            // Payload format: TransactionID (4 bytes) + Data
            if (pktPayload.size() >= 4 && outData) {
                outData->insert(outData->end(), pktPayload.begin() + 4, pktPayload.end());
            }
        } else if (pktType == PTP_PKT_OPER_RESP) {
            // Payload format: ResponseCode (2 bytes) + TransactionID (4 bytes) + Params...
            if (pktPayload.size() >= 6) {
                uint16_t respCode = 0;
                memcpy(&respCode, pktPayload.data(), 2);
                if (outResponseCode) *outResponseCode = respCode;

                if (outResponseParams && pktPayload.size() > 6) {
                    size_t paramBytes = pktPayload.size() - 6;
                    size_t numParams = paramBytes / 4;
                    outResponseParams->resize(numParams);
                    memcpy(outResponseParams->data(), pktPayload.data() + 6, numParams * 4);
                }
                gotResponse = true;
            }
        }
    }
    return true;
}
