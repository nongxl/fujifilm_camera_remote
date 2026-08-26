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
    if (val == 0) return "Auto";
    float f = (float)val / 100.0f;
    return "f/" + String(f, 1);
}

inline String formatShutter(uint32_t val) {
    if (val == 0) return "Auto";
    if (val >= 1000000) {
        return String(val / 1000000) + "s";
    }
    float s = 1000000.0f / (float)val;
    return "1/" + String((int)s);
}

inline String formatISO(uint32_t val) {
    if (val == 0) return "Auto";
    return String(val);
}

inline String formatEV(int32_t val) {
    if (val == 0) return "0.0";
    float ev = (float)val / 1000.0f;
    return (ev > 0 ? "+" : "") + String(ev, 1);
}

#endif // CAMERA_PROPERTIES_H
