#ifndef CAMERA_PROPERTIES_H
#define CAMERA_PROPERTIES_H

#include <stdint.h>
#include <Arduino.h>
#include <vector>

enum class ExposurePropertyId : uint16_t {
    APERTURE = 0x5007,
    SHUTTER_SPEED = 0x500D,
    ISO = 0x500F,
    EXPOSURE_COMPENSATION = 0x5010,
    WHITE_BALANCE = 0x5005,
    FOCUS_MODE = 0x500A
};

struct CameraProperty {
    ExposurePropertyId id = ExposurePropertyId::APERTURE;
    String name = "";
    uint32_t currentValue = 0;
    String currentFormatted = "--";
    std::vector<uint32_t> allowedValues;
    std::vector<String> allowedFormatted;
    int currentIndex = -1;

    CameraProperty() = default;
    CameraProperty(ExposurePropertyId propId, const String& propName)
        : id(propId), name(propName) {}
};

struct ExposureState {
    CameraProperty aperture = { ExposurePropertyId::APERTURE, "Aperture" };
    CameraProperty shutterSpeed = { ExposurePropertyId::SHUTTER_SPEED, "Shutter" };
    CameraProperty iso = { ExposurePropertyId::ISO, "ISO" };
    CameraProperty ev = { ExposurePropertyId::EXPOSURE_COMPENSATION, "EV" };
    CameraProperty whiteBalance = { ExposurePropertyId::WHITE_BALANCE, "WB" };
};

// Helper formatters
inline String formatAperture(uint32_t val) {
    if (val == 0 || val == 0xFFFF) return "---";
    float f = (float)(val & 0xFFFF) / 100.0f;
    return "f/" + String(f, 1);
}

inline String formatShutter(uint32_t val) {
    if (val == 0 || val == 0xFFFFFFFF) return "----";
    bool subsecond = (val & 0x80000000) != 0;
    uint32_t realVal = val & 0x00FFFFFF;
    if (subsecond) {
        float speed = (float)realVal / 1000.0f;
        if (speed >= 10.0f) {
            return "1/" + String((int)speed) + "s";
        } else {
            return "1/" + String(speed, 1) + "s";
        }
    } else {
        float s = (float)realVal / 1000.0f;
        if (s == 0) return "1s";
        return String(s, 1) + "s";
    }
}

inline String formatISO(uint32_t val) {
    if (val == 0 || val == 0xFFFFFFFF) return "Auto";
    bool isAuto = (val & 0x80000000) != 0;
    uint32_t realVal = val & 0x0000FFFF;
    if (isAuto) {
        if (realVal > 0) return "A" + String(realVal);
        return "Auto";
    }
    return String(realVal);
}

inline String formatEV(int32_t val) {
    int16_t signedVal = (int16_t)(val & 0xFFFF);
    if (signedVal == 0) return "0.0 EV";
    float ev = (float)signedVal / 1000.0f;
    return (ev > 0 ? "+" : "") + String(ev, 1) + " EV";
}

#endif // CAMERA_PROPERTIES_H
