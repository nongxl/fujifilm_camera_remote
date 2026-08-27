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
    std::vector<uint8_t> buffer(totalLen);
    
    PtpIpHeader header;
    header.length = totalLen;
    header.type = type;
    
    memcpy(buffer.data(), &header, sizeof(header));
    
    if (payloadLen > 0 && payload != nullptr) {
        memcpy(buffer.data() + sizeof(header), payload, payloadLen);
    }

    Serial.print("[PTP/IP] Sending Packet HEX: ");
    for (size_t i = 0; i < totalLen; i++) {
        Serial.printf("%02X ", buffer[i]);
    }
    Serial.println();

    if (m_client.write(buffer.data(), totalLen) != totalLen) {
        Serial.println("[PTP/IP] Failed to write complete packet to socket");
        return false;
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

bool PtpIpClient::sendFujiInitCommandRequest(const String& clientName) {
    // Fuji cameras use a PROPRIETARY init packet format (from libfuji):
    // Total packet: exactly 0x52 (82) bytes
    // Layout: [length:4][type:4][version:4][guid:16][device_name:54]
    // This is DIFFERENT from standard PTP/IP which uses: [guid][name][version]
    
    const uint32_t FUJI_TOTAL_PACKET_SIZE = 0x52; // 82 bytes
    const uint32_t FUJI_PAYLOAD_SIZE = FUJI_TOTAL_PACKET_SIZE - sizeof(PtpIpHeader); // 74 bytes
    const uint32_t FUJI_PROTOCOL_VERSION = 0x8f53e4f2;
    
    std::vector<uint8_t> payload(FUJI_PAYLOAD_SIZE, 0); // Zero-fill

    // 1. Protocol Version (4 bytes) - FIRST in Fuji format (not last!)
    uint32_t version = FUJI_PROTOCOL_VERSION;
    memcpy(payload.data(), &version, 4);

    // 2. GUID (16 bytes, split as 4x uint32_t)
    memcpy(payload.data() + 4, m_guid, 16);

    // 3. Device Name (UTF-16LE, remaining 54 bytes, zero-padded)
    size_t nameOffset = 20; // 4 (version) + 16 (guid)
    size_t maxNameBytes = FUJI_PAYLOAD_SIZE - nameOffset; // 54 bytes = 27 UTF-16 chars
    for (size_t i = 0; i < clientName.length() && (nameOffset + 2) <= FUJI_PAYLOAD_SIZE; ++i) {
        payload[nameOffset++] = (uint8_t)clientName[i];
        payload[nameOffset++] = 0; // High byte for ASCII -> UTF-16LE
    }
    // Null terminator already present from zero-fill

    Serial.printf("[PTP/IP] Sending FUJI InitCommandReq (total=%u, payload=%u, version=0x%08X, name='%s')...\n",
                  FUJI_TOTAL_PACKET_SIZE, FUJI_PAYLOAD_SIZE, FUJI_PROTOCOL_VERSION, clientName.c_str());

    // Hex dump first 40 bytes of payload for debug
    Serial.print("[PTP/IP] Payload hex: ");
    for (size_t i = 0; i < 40 && i < payload.size(); ++i) {
        Serial.printf("%02X ", payload[i]);
    }
    Serial.println("...");

    if (!sendPacket(PTP_PKT_INIT_CMD_REQ, payload.data(), payload.size())) {
        Serial.println("[PTP/IP] sendPacket failed for Fuji InitCommandReq!");
        return false;
    }

    // Wait for response with 15s timeout (Fuji cameras can be slow)
    uint32_t respType = 0;
    std::vector<uint8_t> respPayload;
    if (!readPacket(respType, respPayload, 15000)) {
        Serial.println("[PTP/IP] Failed to read Fuji InitCommandAck response!");
        return false;
    }

    Serial.printf("[PTP/IP] Fuji response type=%u, payloadSize=%u\n", respType, (unsigned)respPayload.size());

    if (respPayload.size() >= 4) {
        uint32_t code = 0;
        memcpy(&code, respPayload.data(), 4);
        Serial.printf("[PTP/IP] Response Code/Value = 0x%08X\n", code);
    }

    // Hex dump response payload
    if (respPayload.size() > 0) {
        Serial.print("[PTP/IP] Response hex: ");
        for (size_t i = 0; i < respPayload.size() && i < 60; ++i) {
            Serial.printf("%02X ", respPayload[i]);
        }
        Serial.println();
    }

    if (respType == PTP_PKT_INIT_FAIL) {
        uint32_t reason = 0;
        if (respPayload.size() >= 4) memcpy(&reason, respPayload.data(), 4);
        Serial.printf("[PTP/IP] Fuji Init REJECTED (reason: 0x%08X)!\n", reason);
        return false;
    }

    if (respType != PTP_PKT_INIT_CMD_ACK) {
        Serial.printf("[PTP/IP] Unexpected Fuji response type=%u\n", respType);
        return false;
    }

    // Extract Connection Number from Fuji ACK
    if (respPayload.size() >= 4) {
        memcpy(&m_connectionNumber, respPayload.data(), 4);
        Serial.printf("[PTP/IP] Fuji Connection Number = 0x%08X\n", m_connectionNumber);
    }
    
    Serial.println("[PTP/IP] Fuji Init Command ACCEPTED!");
    return true;
}

bool PtpIpClient::sendFujiPacket(uint16_t index, uint16_t opCode, uint32_t txId, const uint8_t* payload, size_t payloadLen) {
    if (!isConnected()) return false;

    uint32_t totalLen = sizeof(FujiPacketHeader) + payloadLen;
    std::vector<uint8_t> buffer(totalLen);

    FujiPacketHeader header;
    header.length = totalLen;
    header.index = index;
    header.code = opCode;
    header.txId = txId;

    memcpy(buffer.data(), &header, sizeof(header));
    if (payloadLen > 0 && payload != nullptr) {
        memcpy(buffer.data() + sizeof(header), payload, payloadLen);
    }

    Serial.printf("[FujiPTP] Tx: len=%u, index=%u, op=0x%04X, txId=%u, payloadLen=%u\n",
                  totalLen, index, opCode, txId, (unsigned)payloadLen);
    Serial.print("[FujiPTP] Tx HEX: ");
    for (size_t i = 0; i < totalLen; i++) {
        Serial.printf("%02X ", buffer[i]);
    }
    Serial.println();

    if (m_client.write(buffer.data(), totalLen) != totalLen) {
        Serial.println("[FujiPTP] Failed to write complete packet");
        return false;
    }
    return true;
}

bool PtpIpClient::receiveFujiPacket(uint16_t& outIndex, uint16_t& outCode, uint32_t& outTxId, std::vector<uint8_t>& outPayload, uint32_t timeoutMs) {
    if (!isConnected()) return false;

    // First read total length (4 bytes)
    uint32_t totalLen = 0;
    if (!readExact((uint8_t*)&totalLen, sizeof(totalLen), timeoutMs)) {
        return false;
    }

    if (totalLen < sizeof(FujiPacketHeader)) {
        Serial.printf("[FujiPTP] Invalid packet length: %u\n", totalLen);
        return false;
    }

    // Next read remainder of header: index (2), code (2), txId (4) -> 8 bytes
    struct {
        uint16_t index;
        uint16_t code;
        uint32_t txId;
    } __attribute__((packed)) hdrRest;

    if (!readExact((uint8_t*)&hdrRest, sizeof(hdrRest), timeoutMs)) {
        return false;
    }

    outIndex = hdrRest.index;
    outCode = hdrRest.code;
    outTxId = hdrRest.txId;

    size_t payloadLen = totalLen - sizeof(FujiPacketHeader);
    outPayload.resize(payloadLen);
    if (payloadLen > 0) {
        if (!readExact(outPayload.data(), payloadLen, timeoutMs)) {
            return false;
        }
    }

    Serial.printf("[FujiPTP] Rx: len=%u, index=%u, code=0x%04X, txId=%u, payloadLen=%u\n",
                  totalLen, outIndex, outCode, outTxId, (unsigned)payloadLen);
    return true;
}

bool PtpIpClient::executeFujiOperation(uint16_t opCode, 
                                      const std::vector<uint8_t>& payload,
                                      std::vector<uint8_t>* outData,
                                      uint16_t* outRespCode) {
    if (!isConnected()) return false;

    m_transactionId++;
    uint32_t txId = m_transactionId;

    if (!sendFujiPacket(1, opCode, txId, payload.empty() ? nullptr : payload.data(), payload.size())) {
        return false;
    }

    // Fuji can return a Data packet followed by a Response packet, or just a Response packet
    bool gotResponse = false;
    while (!gotResponse) {
        uint16_t index = 0, code = 0;
        uint32_t rxTxId = 0;
        std::vector<uint8_t> rxPayload;
        if (!receiveFujiPacket(index, code, rxTxId, rxPayload, 10000)) {
            return false;
        }

        if (index == 2) {
            // Data payload packet
            if (outData) {
                outData->insert(outData->end(), rxPayload.begin(), rxPayload.end());
            }
        } else if (index == 3) {
            // Response packet (code is return code, e.g. 0x2001)
            if (outRespCode) *outRespCode = code;
            gotResponse = true;
            return (code == PTP_RC_OK || code == PTP_RC_SessionAlreadyOpened);
        } else {
            // Unknown or unexpected index
            Serial.printf("[FujiPTP] Unexpected packet index: %u\n", index);
            return false;
        }
    }
    return true;
}

bool PtpIpClient::executeFujiTwoPartOperation(uint16_t opCode,
                                            const std::vector<uint8_t>& part1Data,
                                            const std::vector<uint8_t>& part2Data,
                                            uint16_t* outRespCode) {
    if (!isConnected()) return false;

    m_transactionId++;
    uint32_t txId = m_transactionId;

    // Send Part 1 (index = 1)
    if (!sendFujiPacket(1, opCode, txId, part1Data.empty() ? nullptr : part1Data.data(), part1Data.size())) {
        return false;
    }

    // Send Part 2 (index = 2)
    if (!sendFujiPacket(2, opCode, txId, part2Data.empty() ? nullptr : part2Data.data(), part2Data.size())) {
        return false;
    }

    // Wait for response (index = 3)
    uint16_t index = 0, code = 0;
    uint32_t rxTxId = 0;
    std::vector<uint8_t> rxPayload;
    if (!receiveFujiPacket(index, code, rxTxId, rxPayload, 10000)) {
        return false;
    }

    if (outRespCode) *outRespCode = code;
    return (index == 3 && (code == PTP_RC_OK || code == PTP_RC_SessionAlreadyOpened));
}

bool PtpIpClient::sendOpenSession(uint32_t sessionId) {
    std::vector<uint8_t> payload(4);
    memcpy(payload.data(), &sessionId, 4);
    uint16_t respCode = 0;
    bool success = executeFujiOperation(PTP_OC_OpenSession, payload, nullptr, &respCode);
    if (success && (respCode == PTP_RC_OK || respCode == PTP_RC_SessionAlreadyOpened)) {
        m_sessionId = sessionId;
        return true;
    }
    return false;
}

bool PtpIpClient::sendCloseSession() {
    uint16_t respCode = 0;
    bool success = executeFujiOperation(PTP_OC_CloseSession, {}, nullptr, &respCode);
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
