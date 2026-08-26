#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <vector>
#include <functional>

enum class WifiState {
    IDLE,
    SCANNING,
    SCAN_DONE,
    CONNECTING,
    CONNECTED,
    CONNECT_FAILED,
    DISCONNECTED
};

struct ScannedAP {
    String ssid;
    int32_t rssi;
    uint8_t encryptionType;
    bool isFujiCamera;
};

class WifiManager {
public:
    using StateCallback = std::function<void(WifiState state, const String& info)>;

    WifiManager();
    ~WifiManager();

    void begin();
    void update();

    void startScan();
    bool isScanDone() const { return m_state == WifiState::SCAN_DONE; }
    const std::vector<ScannedAP>& getScannedAPs() const { return m_scannedAPs; }

    void connectTo(const String& ssid, const String& password = "");
    void disconnect();

    bool isConnected() const { return WiFi.status() == WL_CONNECTED; }
    IPAddress getLocalIP() const { return WiFi.localIP(); }
    IPAddress getGatewayIP() const { return WiFi.gatewayIP(); }

    void setStateCallback(StateCallback cb) { m_stateCallback = cb; }
    WifiState getState() const { return m_state; }

private:
    void setState(WifiState state, const String& info = "");

    WifiState m_state = WifiState::IDLE;
    std::vector<ScannedAP> m_scannedAPs;
    StateCallback m_stateCallback = nullptr;
    unsigned long m_connectStartTime = 0;
    static constexpr unsigned long CONNECT_TIMEOUT_MS = 15000;
};

#endif // WIFI_MANAGER_H
