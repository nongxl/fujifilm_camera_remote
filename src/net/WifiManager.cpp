#include "WifiManager.h"
#include <esp_wifi.h>

WifiManager::WifiManager() {
}

WifiManager::~WifiManager() {
}

void WifiManager::begin() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);
    delay(100);
    setState(WifiState::IDLE, "WiFi Initialized");
}

void WifiManager::startScan() {
    m_scannedAPs.clear();
    WiFi.scanDelete();
    setState(WifiState::SCANNING, "Scanning for WiFi networks...");
    m_scanStartTime = millis();
    WiFi.scanNetworks(true, false);
}

void WifiManager::connectTo(const String& ssid, const String& password) {
    setState(WifiState::CONNECTING, "Connecting to " + ssid);
    WiFi.disconnect();
    if (password.length() > 0) {
        WiFi.begin(ssid.c_str(), password.c_str());
    } else {
        WiFi.begin(ssid.c_str());
    }
    m_connectStartTime = millis();
}

void WifiManager::connectFast(const String& ssid, uint8_t channel, const uint8_t* bssid, const String& password) {
    setState(WifiState::CONNECTING, "Fast connecting to " + ssid);
    WiFi.disconnect();
    if (bssid) {
        WiFi.begin(ssid.c_str(), password.c_str(), (int32_t)channel, bssid, true);
    } else {
        WiFi.begin(ssid.c_str(), password.c_str(), (int32_t)channel, nullptr, true);
    }
    m_connectStartTime = millis();
}

void WifiManager::disconnect() {
    WiFi.disconnect();
    setState(WifiState::DISCONNECTED, "Disconnected");
}

void WifiManager::setState(WifiState state, const String& info) {
    m_state = state;
    if (m_stateCallback) {
        m_stateCallback(m_state, info);
    }
}

void WifiManager::update() {
    if (m_state == WifiState::SCANNING) {
        int16_t scanResult = WiFi.scanComplete();
        if (scanResult >= 0) {
            m_scannedAPs.clear();
            for (int i = 0; i < scanResult; ++i) {
                ScannedAP ap;
                ap.ssid = WiFi.SSID(i);
                ap.rssi = WiFi.RSSI(i);
                ap.encryptionType = WiFi.encryptionType(i);
                ap.channel = WiFi.channel(i);
                uint8_t* b = WiFi.BSSID(i);
                if (b) memcpy(ap.bssid, b, 6);
                ap.isFujiCamera = ap.ssid.startsWith("FUJIFILM-");
                m_scannedAPs.push_back(ap);
            }
            WiFi.scanDelete();
            setState(WifiState::SCAN_DONE, "Found " + String(m_scannedAPs.size()) + " networks");
        } else if (millis() - m_scanStartTime > 10000) {
            WiFi.scanDelete();
            setState(WifiState::IDLE, "Scan timeout");
        }
    } else if (m_state == WifiState::CONNECTING) {
        if (WiFi.status() == WL_CONNECTED) {
            WiFi.setSleep(false);
            esp_wifi_set_ps(WIFI_PS_NONE);
            setState(WifiState::CONNECTED, "IP: " + WiFi.localIP().toString());
        } else if (millis() - m_connectStartTime > CONNECT_TIMEOUT_MS) {
            setState(WifiState::CONNECT_FAILED, "Connection timeout");
            WiFi.disconnect();
        }
    } else if (m_state == WifiState::CONNECTED) {
        if (WiFi.status() != WL_CONNECTED) {
            setState(WifiState::DISCONNECTED, "Connection lost");
        }
    }
}
