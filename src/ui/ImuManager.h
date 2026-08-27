#ifndef IMU_MANAGER_H
#define IMU_MANAGER_H

#include <M5Unified.h>

enum class DeviceOrientation {
    PORTRAIT,
    LANDSCAPE
};

class ImuManager {
public:
    ImuManager() = default;

    void begin() {
        m_lastStepTime = millis();
        m_filteredTilt = 0.0f;
        m_stableOrientation = DeviceOrientation::LANDSCAPE; // Default to landscape
        m_candidateOrientation = DeviceOrientation::LANDSCAPE;
        m_orientationChangeStartTime = millis();
    }

    // Call frequently in loop()
    void update() {
        if (!M5.Imu.isEnabled()) return;

        float ax = 0, ay = 0, az = 0;
        if (M5.Imu.getAccel(&ax, &ay, &az)) {
            // Calculate tilt angle for slider adjust
            float rawTilt = 0.0f;
            if (fabs(az) > 0.05f || fabs(ax) > 0.05f) {
                rawTilt = atan2f(-ax, sqrtf(ay * ay + az * az)) * (180.0f / M_PI);
            }

            // Low-pass filter (alpha = 0.25)
            m_filteredTilt = m_filteredTilt * 0.75f + rawTilt * 0.25f;

            // Orientation detection with hysteresis
            // Vertical (Portrait): |ay| > 0.60G
            // Horizontal (Landscape): |ay| < 0.40G
            DeviceOrientation instantOrientation = m_stableOrientation;
            if (fabs(ay) > 0.60f) {
                instantOrientation = DeviceOrientation::PORTRAIT;
            } else if (fabs(ay) < 0.40f) {
                instantOrientation = DeviceOrientation::LANDSCAPE;
            }

            if (instantOrientation != m_candidateOrientation) {
                m_candidateOrientation = instantOrientation;
                m_orientationChangeStartTime = millis();
            } else {
                // If candidate remains stable for 400ms, apply change
                if (m_candidateOrientation != m_stableOrientation) {
                    if (millis() - m_orientationChangeStartTime >= ORIENTATION_STABLE_MS) {
                        m_stableOrientation = m_candidateOrientation;
                        m_orientationChanged = true;
                        Serial.printf("[IMU] Orientation switched to: %s\n", 
                            m_stableOrientation == DeviceOrientation::LANDSCAPE ? "LANDSCAPE" : "PORTRAIT");
                    }
                }
            }
        }
    }

    DeviceOrientation getOrientation() const {
        return m_stableOrientation;
    }

    bool checkAndClearOrientationChanged() {
        if (m_orientationChanged) {
            m_orientationChanged = false;
            return true;
        }
        return false;
    }

    // Returns current smooth tilt angle (-90.0 to +90.0 degrees)
    float getTiltAngle() const {
        return m_filteredTilt;
    }

    // Returns -1 (step left/down), +1 (step right/up), or 0 (no step / deadzone)
    int getStepDelta() {
        float absTilt = fabsf(m_filteredTilt);
        if (absTilt < DEADZONE_DEG) {
            return 0;
        }

        // Calculate repeat interval based on tilt magnitude (steeper = faster)
        uint32_t interval = 400;
        if (absTilt > 15.0f) {
            float factor = (absTilt - 15.0f) / 35.0f; // 0.0 to 1.0
            if (factor > 1.0f) factor = 1.0f;
            interval = (uint32_t)(400 - factor * 280); // 400ms down to 120ms
        }

        if (millis() - m_lastStepTime >= interval) {
            m_lastStepTime = millis();
            return (m_filteredTilt > 0) ? 1 : -1;
        }

        return 0;
    }

    void reset() {
        m_lastStepTime = millis();
        m_filteredTilt = 0.0f;
    }

private:
    static constexpr float DEADZONE_DEG = 12.0f;
    static constexpr unsigned long ORIENTATION_STABLE_MS = 400; // 400ms debounce
    float m_filteredTilt = 0.0f;
    unsigned long m_lastStepTime = 0;

    DeviceOrientation m_stableOrientation = DeviceOrientation::LANDSCAPE;
    DeviceOrientation m_candidateOrientation = DeviceOrientation::LANDSCAPE;
    unsigned long m_orientationChangeStartTime = 0;
    bool m_orientationChanged = false;
};

#endif // IMU_MANAGER_H
