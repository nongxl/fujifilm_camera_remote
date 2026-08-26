#pragma once
#include <Arduino.h>

// ─────────────────────────────────────────────────────────────
//  M5StickS3 Hardware Template Config
// ─────────────────────────────────────────────────────────────

static constexpr int    SCREEN_W_P         = 135;
static constexpr int    SCREEN_H_P         = 240;

static constexpr int    SCREEN_W_L         = 240;
static constexpr int    SCREEN_H_L         = 135;

static constexpr float  IMU_LPF_ALPHA      = 0.92f;
static constexpr float  IMU_DEADZONE       = 0.12f;

static constexpr uint8_t SYSTEM_VOLUME     = 120;
static constexpr uint8_t SYSTEM_BRIGHTNESS = 80;

static constexpr int    VIBR_PIN           = 0;
static constexpr int    VIBR_PWM_CHANNEL   = 2;
static constexpr int    VIBR_PWM_FREQ      = 10000;
static constexpr int    VIBR_PWM_BITS      = 8;
