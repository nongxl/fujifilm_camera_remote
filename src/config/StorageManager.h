#ifndef STORAGE_MANAGER_H
#define STORAGE_MANAGER_H

#include <Arduino.h>
#include <Preferences.h>

struct CameraWifiProfile {
    bool valid = false;
    String ssid = "";
    uint8_t channel = 1;
    uint8_t bssid[6] = {0};
    bool hasBssid = false;
};

class StorageManager {
public:
    static StorageManager& getInstance() {
        static StorageManager instance;
        return instance;
    }

    void begin() {
        m_prefs.begin("fuji_cfg", false);
    }

    bool hasPairedCamera() {
        return m_prefs.getBool("valid", false) && m_prefs.getString("ssid", "").length() > 0;
    }

    CameraWifiProfile loadProfile() {
        CameraWifiProfile profile;
        profile.valid = m_prefs.getBool("valid", false);
        profile.ssid = m_prefs.getString("ssid", "");
        profile.channel = m_prefs.getUChar("channel", 1);
        profile.hasBssid = m_prefs.getBool("has_bssid", false);
        if (profile.hasBssid) {
            m_prefs.getBytes("bssid", profile.bssid, 6);
        }
        return profile;
    }

    void saveProfile(const String& ssid, uint8_t channel, const uint8_t* bssid = nullptr) {
        m_prefs.putBool("valid", true);
        m_prefs.putString("ssid", ssid);
        m_prefs.putUChar("channel", channel);
        if (bssid) {
            m_prefs.putBool("has_bssid", true);
            m_prefs.putBytes("bssid", bssid, 6);
        } else {
            m_prefs.putBool("has_bssid", false);
        }
        Serial.printf("[Storage] Saved paired camera: SSID=%s, Channel=%d\n", ssid.c_str(), channel);
    }

    void clearProfile() {
        m_prefs.clear();
        Serial.println("[Storage] Cleared paired camera profile from NVS.");
    }

private:
    StorageManager() = default;
    Preferences m_prefs;
};

#endif // STORAGE_MANAGER_H
