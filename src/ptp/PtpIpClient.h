#ifndef PTP_IP_CLIENT_H
#define PTP_IP_CLIENT_H

#include <Arduino.h>
#include <WiFiClient.h>
#include <vector>
#include "PtpTypes.h"

class PtpIpClient {
public:
    PtpIpClient();
    ~PtpIpClient();

    bool connect(const IPAddress& ip, uint16_t port = 15740, uint32_t timeoutMs = 5000);
    void disconnect();
    bool isConnected();

    // High level commands
    bool sendInitCommandRequest(const String& clientName = "M5StickS3");
    bool sendFujiInitCommandRequest(const String& clientName = "HackedClient");
    bool sendOpenSession(uint32_t sessionId = 1);
    bool sendCloseSession();

    // Fuji proprietary operation execution
    bool executeFujiOperation(uint16_t opCode, 
                             const std::vector<uint8_t>& payload = {},
                             std::vector<uint8_t>* outData = nullptr,
                             uint16_t* outRespCode = nullptr);

    bool executeFujiTwoPartOperation(uint16_t opCode,
                                    const std::vector<uint8_t>& part1Data,
                                    const std::vector<uint8_t>& part2Data,
                                    uint16_t* outRespCode = nullptr);

    // Standard PTP/IP operation execution
    bool executeOperation(uint16_t opCode, 
                          const std::vector<uint32_t>& params = {},
                          std::vector<uint8_t>* outData = nullptr,
                          std::vector<uint32_t>* outResponseParams = nullptr,
                          uint16_t* outResponseCode = nullptr);

    uint32_t getSessionId() const { return m_sessionId; }
    uint32_t getConnectionNumber() const { return m_connectionNumber; }

private:
    bool sendPacket(uint32_t type, const uint8_t* payload, size_t payloadLen);
    bool readExact(uint8_t* buffer, size_t len, uint32_t timeoutMs = 5000);
    bool readPacket(uint32_t& outType, std::vector<uint8_t>& outPayload, uint32_t timeoutMs = 5000);

    // Fuji-specific packet sending & receiving
    bool sendFujiPacket(uint16_t index, uint16_t opCode, uint32_t txId, const uint8_t* payload = nullptr, size_t payloadLen = 0);
    bool receiveFujiPacket(uint16_t& outIndex, uint16_t& outCode, uint32_t& outTxId, std::vector<uint8_t>& outPayload, uint32_t timeoutMs = 5000);

    WiFiClient m_client;
    uint32_t m_transactionId = 0;
    uint32_t m_sessionId = 0;
    uint32_t m_connectionNumber = 0;
    uint8_t m_guid[16];
};

#endif // PTP_IP_CLIENT_H
