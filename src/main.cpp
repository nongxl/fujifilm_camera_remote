#include <Arduino.h>
#include "../.pio/libdeps/m5stack-sticks3/lvgl/lvgl.h"
#define M5GFX_USING_REAL_LVGL 1
#include <M5Unified.h>

#include "net/WifiManager.h"
#include "camera/fujifilm/FujiCamera.h"
#include "camera/fujifilm/FujiLiveViewStream.h"
#include "ui/ImuManager.h"
#include "config/StorageManager.h"

/* Global instances */
static WifiManager g_wifiManager;
static FujiCamera g_camera;
static FujiLiveViewStream g_liveViewStream;
static ImuManager g_imu;

/* LVGL display buffer */
static uint8_t *g_drawBuffer = nullptr;
static lv_obj_t *g_statusLabel = nullptr;
static lv_obj_t *g_infoLabel = nullptr;
static lv_obj_t *g_btnLabel = nullptr;

/* Parameter control UI elements */
static lv_obj_t *g_paramContainer = nullptr;
static lv_obj_t *g_isoCard = nullptr;
static lv_obj_t *g_apertureCard = nullptr;
static lv_obj_t *g_shutterCard = nullptr;
static lv_obj_t *g_evCard = nullptr;

static lv_obj_t *g_isoValLabel = nullptr;
static lv_obj_t *g_apertureValLabel = nullptr;
static lv_obj_t *g_shutterValLabel = nullptr;
static lv_obj_t *g_evValLabel = nullptr;

/* IMU Slider UI overlay elements */
static lv_obj_t *g_sliderContainer = nullptr;
static lv_obj_t *g_sliderTitleLabel = nullptr;
static lv_obj_t *g_sliderValLabel = nullptr;
static lv_obj_t *g_sliderBar = nullptr;

enum class SelectedParam {
    ISO,
    APERTURE,
    SHUTTER,
    EV
};

static SelectedParam g_selectedParam = SelectedParam::ISO;

/* Parameter Adjustment state */
static int g_candidateIndex = 0;
static std::vector<uint32_t> g_currentAllowedValues;
static std::vector<String> g_currentAllowedFormatted;
static ExposurePropertyId g_currentPropertyId = ExposurePropertyId::ISO;

/* LiveView & Orientation state */
static bool g_liveViewLocked = false;
static bool g_inLiveViewMode = false;
static unsigned long g_lastBtnBPressTime = 0;
static IPAddress g_cameraIP;

/* System state */
enum class AppState {
    IDLE,
    SCANNING_WIFI,
    WIFI_FOUND,
    CONNECTING_WIFI,
    CONNECTING_CAMERA,
    CAMERA_READY,
    CAMERA_ADJUSTING_PARAM
};

static AppState g_appState = AppState::IDLE;
static String g_targetSSID = "";
static CameraWifiProfile g_savedProfile;

/* Display flushing callback for LVGL 9 */
void my_disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);
    
    M5.Display.startWrite();
    M5.Display.setAddrWindow(area->x1, area->y1, w, h);
    M5.Display.pushPixels((uint16_t *)px_map, w * h, true);
    M5.Display.endWrite();
    
    lv_display_flush_ready(disp);
}

void updateUI(const String& status, const String& info, const String& btnHint)
{
    if (g_statusLabel) lv_label_set_text(g_statusLabel, status.c_str());
    if (g_infoLabel) lv_label_set_text(g_infoLabel, info.c_str());
    if (g_btnLabel) lv_label_set_text(g_btnLabel, btnHint.c_str());
}

void updateParameterCards()
{
    if (!g_paramContainer) return;

    const auto& exp = g_camera.getExposureState();
    if (g_isoValLabel) lv_label_set_text(g_isoValLabel, exp.iso.currentFormatted.c_str());
    if (g_apertureValLabel) lv_label_set_text(g_apertureValLabel, exp.aperture.currentFormatted.c_str());
    if (g_shutterValLabel) lv_label_set_text(g_shutterValLabel, exp.shutterSpeed.currentFormatted.c_str());
    if (g_evValLabel) lv_label_set_text(g_evValLabel, exp.ev.currentFormatted.c_str());

    // Highlight selected card with semi-transparent background
    auto highlightCard = [](lv_obj_t* card, bool selected) {
        if (!card) return;
        lv_obj_set_style_border_color(card, selected ? lv_color_hex(0x00FF88) : lv_color_hex(0x444444), 0);
        lv_obj_set_style_border_width(card, selected ? 2 : 1, 0);
        lv_obj_set_style_bg_color(card, selected ? lv_color_hex(0x202020) : lv_color_hex(0x101010), 0);
        lv_obj_set_style_bg_opa(card, selected ? LV_OPA_80 : LV_OPA_60, 0);
    };

    highlightCard(g_isoCard, g_selectedParam == SelectedParam::ISO);
    highlightCard(g_apertureCard, g_selectedParam == SelectedParam::APERTURE);
    highlightCard(g_shutterCard, g_selectedParam == SelectedParam::SHUTTER);
    highlightCard(g_evCard, g_selectedParam == SelectedParam::EV);
}

void showDashboard(bool show)
{
    if (g_paramContainer) {
        if (show) {
            lv_obj_clear_flag(g_paramContainer, LV_OBJ_FLAG_HIDDEN);
            if (g_statusLabel) lv_obj_add_flag(g_statusLabel, LV_OBJ_FLAG_HIDDEN);
            if (g_infoLabel) lv_obj_add_flag(g_infoLabel, LV_OBJ_FLAG_HIDDEN);
            if (g_btnLabel) lv_obj_add_flag(g_btnLabel, LV_OBJ_FLAG_HIDDEN); // Remove bottom hint in dashboard
            updateParameterCards();
        } else {
            lv_obj_add_flag(g_paramContainer, LV_OBJ_FLAG_HIDDEN);
            if (g_statusLabel) lv_obj_clear_flag(g_statusLabel, LV_OBJ_FLAG_HIDDEN);
            if (g_infoLabel) lv_obj_clear_flag(g_infoLabel, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void createParamCard(lv_obj_t* parent, const char* title, lv_obj_t*& outCard, lv_obj_t*& outValLabel)
{
    outCard = lv_obj_create(parent);
    lv_obj_set_size(outCard, 114, 44);
    lv_obj_set_style_bg_color(outCard, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(outCard, LV_OPA_70, 0); // Matching top status bar semi-transparent dark glass
    lv_obj_set_style_border_color(outCard, lv_color_hex(0x3186), 0);
    lv_obj_set_style_border_width(outCard, 1, 0);
    lv_obj_set_style_radius(outCard, 6, 0);
    lv_obj_set_style_pad_all(outCard, 3, 0);
    lv_obj_clear_flag(outCard, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* titleLbl = lv_label_create(outCard);
    lv_label_set_text(titleLbl, title);
    lv_obj_set_style_text_color(titleLbl, lv_color_hex(0x888888), 0);
    lv_obj_align(titleLbl, LV_ALIGN_TOP_LEFT, 4, 1);

    outValLabel = lv_label_create(outCard);
    lv_label_set_text(outValLabel, "--");
    lv_obj_set_style_text_color(outValLabel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(outValLabel, LV_ALIGN_BOTTOM_RIGHT, -4, -1);
}

static String g_sliderTitle = "TILT ADJUST";

void enterAdjustMode()
{
    const auto& exp = g_camera.getExposureState();
    const CameraProperty* targetProp = nullptr;
    const char* title = "ISO";

    switch (g_selectedParam) {
        case SelectedParam::ISO:
            g_currentPropertyId = ExposurePropertyId::ISO;
            targetProp = &exp.iso;
            title = "TILT ADJUST: ISO";
            break;
        case SelectedParam::APERTURE:
            g_currentPropertyId = ExposurePropertyId::APERTURE;
            targetProp = &exp.aperture;
            title = "TILT ADJUST: APERTURE";
            break;
        case SelectedParam::SHUTTER:
            g_currentPropertyId = ExposurePropertyId::SHUTTER_SPEED;
            targetProp = &exp.shutterSpeed;
            title = "TILT ADJUST: SHUTTER";
            break;
        case SelectedParam::EV:
            g_currentPropertyId = ExposurePropertyId::EXPOSURE_COMPENSATION;
            targetProp = &exp.ev;
            title = "TILT ADJUST: EV";
            break;
    }

    if (!targetProp || targetProp->allowedValues.empty()) return;

    g_currentAllowedValues = targetProp->allowedValues;
    g_currentAllowedFormatted = targetProp->allowedFormatted;

    // Find current index
    g_candidateIndex = 0;
    for (size_t i = 0; i < g_currentAllowedValues.size(); ++i) {
        if (g_currentAllowedValues[i] == targetProp->currentValue) {
            g_candidateIndex = (int)i;
            break;
        }
    }

    g_sliderTitle = title;
    g_imu.reset();
    g_appState = AppState::CAMERA_ADJUSTING_PARAM;

    Serial.printf("[UI] Entered IMU Adjust Mode for %s (Index=%d)\n", title, g_candidateIndex);
}

void exitAdjustMode(bool applyChange)
{
    if (applyChange && g_candidateIndex >= 0 && g_candidateIndex < (int)g_currentAllowedValues.size()) {
        uint32_t valToSet = g_currentAllowedValues[g_candidateIndex];
        Serial.printf("[UI] Applying parameter %d = %u\n", (int)g_currentPropertyId, (unsigned)valToSet);
        g_camera.setPropertyValue(g_currentPropertyId, valToSet);
    }

    g_appState = AppState::CAMERA_READY;
}

void setup()
{
    auto cfg = M5.config();
    M5.begin(cfg);
    M5.Display.setRotation(1); // Landscape mode (240x135)
    
    Serial.begin(115200);
    delay(300);

    Serial.println("\n=================================");
    Serial.println("  Fujifilm Camera Remote - Phase 4");
    Serial.println("=================================");

    // Initialize Storage & IMU
    StorageManager::getInstance().begin();
    g_savedProfile = StorageManager::getInstance().loadProfile();
    g_imu.begin();

    // Initialize LVGL 9 (Used for Pairing / Connecting screens)
    lv_init();
    lv_tick_set_cb((lv_tick_get_cb_t)millis);
    
    uint32_t w = M5.Display.width();
    uint32_t h = M5.Display.height();
    size_t bufSize = w * h / 8 * sizeof(lv_color_t);
    g_drawBuffer = (uint8_t *)heap_caps_malloc(bufSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    
    lv_display_t *disp = lv_display_create(w, h);
    lv_display_set_flush_cb(disp, my_disp_flush);
    lv_display_set_buffers(disp, g_drawBuffer, NULL, bufSize, LV_DISPLAY_RENDER_MODE_PARTIAL);
    
    // Create UI layout
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101010), 0);

    // Title / Status (Used for connecting/idle pages)
    g_statusLabel = lv_label_create(scr);
    lv_label_set_text(g_statusLabel, "FUJI REMOTE");
    lv_obj_set_style_text_color(g_statusLabel, lv_color_hex(0x00FF88), 0);
    lv_obj_set_style_text_align(g_statusLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_statusLabel, LV_ALIGN_TOP_MID, 0, 4);

    // Info details
    g_infoLabel = lv_label_create(scr);
    if (g_savedProfile.valid && g_savedProfile.ssid.length() > 0) {
        lv_label_set_text(g_infoLabel, ("Paired: " + g_savedProfile.ssid).c_str());
    } else {
        lv_label_set_text(g_infoLabel, "Ready to Connect");
    }
    lv_obj_set_style_text_color(g_infoLabel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(g_infoLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(g_infoLabel, 230);
    lv_obj_align(g_infoLabel, LV_ALIGN_CENTER, 0, -8);

    // Action Hint (Clean 2-line layout at bottom)
    g_btnLabel = lv_label_create(scr);
    if (g_savedProfile.valid && g_savedProfile.ssid.length() > 0) {
        lv_label_set_text(g_btnLabel, "[A] Fast Connect\n[Hold B 3s] Reset Pairing");
    } else {
        lv_label_set_text(g_btnLabel, "[A] Scan Wi-Fi\n[B] Reset");
    }
    lv_obj_set_style_text_color(g_btnLabel, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_text_align(g_btnLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(g_btnLabel, 230);
    lv_obj_align(g_btnLabel, LV_ALIGN_BOTTOM_MID, 0, -3);

    // Initialize Network Manager
    g_wifiManager.setStateCallback([](WifiState state, const String& info) {
        Serial.printf("[WiFi] State: %d, Info: %s\n", (int)state, info.c_str());
    });
    g_wifiManager.begin();
}

void loop()
{
    M5.update();
    g_wifiManager.update();
    if (!g_inLiveViewMode) {
        g_camera.update();
    }
    g_imu.update();
    g_liveViewStream.update();

    if (g_appState == AppState::CAMERA_READY || g_appState == AppState::CAMERA_ADJUSTING_PARAM) {
        if (g_liveViewStream.hasNewFrame()) {
            const auto& exp = g_camera.getExposureState();
            String expText = "ISO " + exp.iso.currentFormatted + "  " + exp.aperture.currentFormatted + "  " + exp.shutterSpeed.currentFormatted;
            if (g_liveViewStream.isMirror()) expText += " [M]";

            LiveViewOverlay overlay;
            overlay.showMenu = !g_inLiveViewMode;
            overlay.selectedParam = (int)g_selectedParam;
            overlay.isoVal = exp.iso.currentFormatted;
            overlay.aptVal = exp.aperture.currentFormatted;
            overlay.shtVal = exp.shutterSpeed.currentFormatted;
            overlay.evVal = exp.ev.currentFormatted;

            if (g_appState == AppState::CAMERA_ADJUSTING_PARAM) {
                overlay.showSlider = true;
                overlay.sliderTitle = g_sliderTitle;
                overlay.sliderVal = (g_candidateIndex >= 0 && g_candidateIndex < (int)g_currentAllowedFormatted.size()) ? g_currentAllowedFormatted[g_candidateIndex] : "";
                overlay.sliderIndex = g_candidateIndex;
                overlay.sliderTotal = (int)g_currentAllowedValues.size();
            }

            g_liveViewStream.render(M5.Display, expText, &overlay);
            g_liveViewStream.clearNewFrame();
        }

        if (!g_inLiveViewMode && g_appState == AppState::CAMERA_READY) {
            // IMU 2D Tilt Gesture: Up/Down/Left/Right grid navigation
            int dx = 0, dy = 0;
            if (g_imu.getGridNavDelta(dx, dy)) {
                int row = (g_selectedParam == SelectedParam::SHUTTER || g_selectedParam == SelectedParam::EV) ? 1 : 0;
                int col = (g_selectedParam == SelectedParam::APERTURE || g_selectedParam == SelectedParam::EV) ? 1 : 0;

                if (dx != 0) col = (dx > 0) ? 1 : 0;
                if (dy != 0) row = (dy > 0) ? 1 : 0;

                if (row == 0 && col == 0) g_selectedParam = SelectedParam::ISO;
                else if (row == 0 && col == 1) g_selectedParam = SelectedParam::APERTURE;
                else if (row == 1 && col == 0) g_selectedParam = SelectedParam::SHUTTER;
                else if (row == 1 && col == 1) g_selectedParam = SelectedParam::EV;

                Serial.printf("[UI] IMU Grid Nav -> Selected: %d (row=%d, col=%d)\n", (int)g_selectedParam, row, col);
            }
        }
    } else if (g_appState == AppState::CAMERA_ADJUSTING_PARAM) {
        // IMU tilt step processing for parameter value
        int stepDelta = g_imu.getStepDelta();
        if (stepDelta != 0 && !g_currentAllowedValues.empty()) {
            int newIdx = g_candidateIndex + stepDelta;
            if (newIdx < 0) newIdx = 0;
            if (newIdx >= (int)g_currentAllowedValues.size()) newIdx = (int)g_currentAllowedValues.size() - 1;

            if (newIdx != g_candidateIndex) {
                g_candidateIndex = newIdx;
                Serial.printf("[IMU] Tilted step -> Index %d: %s\n", g_candidateIndex, g_currentAllowedFormatted[g_candidateIndex].c_str());
            }
        }
    }

    // Button A interaction
    if (M5.BtnA.wasPressed()) {
        if (g_appState == AppState::IDLE) {
            if (g_savedProfile.valid && g_savedProfile.ssid.length() > 0) {
                // NVS Fast Connect
                g_targetSSID = g_savedProfile.ssid;
                g_appState = AppState::CONNECTING_WIFI;
                updateUI("FAST CONNECT", g_targetSSID, "Connecting...\n[B] Cancel");
                Serial.printf("[App] Fast connecting to %s (Ch:%d)...\n", g_targetSSID.c_str(), g_savedProfile.channel);
                g_wifiManager.connectFast(g_savedProfile.ssid, g_savedProfile.channel, g_savedProfile.hasBssid ? g_savedProfile.bssid : nullptr);
            } else {
                // Scan Wi-Fi
                Serial.println("[UI] Starting WiFi scan...");
                g_appState = AppState::SCANNING_WIFI;
                updateUI("SCANNING...", "Searching for Fuji AP", "Please wait...\n[B] Cancel");
                g_wifiManager.startScan();
            }
        } else if (g_appState == AppState::WIFI_FOUND) {
            Serial.printf("[UI] Connecting to %s...\n", g_targetSSID.c_str());
            g_appState = AppState::CONNECTING_WIFI;
            updateUI("CONNECTING...", g_targetSSID, "Joining Wi-Fi...\n[B] Cancel");
            g_wifiManager.connectTo(g_targetSSID);
        } else if (g_appState == AppState::CAMERA_READY) {
            if (g_inLiveViewMode) {
                // Short press A in LiveView: Trigger Shutter
                Serial.println("[UI] Triggering Shutter from LiveView!");
                bool ok = g_camera.triggerShutter();
                Serial.printf("[Camera] Shutter result: %s\n", ok ? "SUCCESS" : "FAILED");
            } else {
                // Short press A in Dashboard: Enter Adjust Mode for highlighted card!
                Serial.println("[UI] BtnA pressed: Entering adjust mode for selected param");
                enterAdjustMode();
            }
        } else if (g_appState == AppState::CAMERA_ADJUSTING_PARAM) {
            // Short press A in adjust mode: Confirm and Set value
            exitAdjustMode(true);
        }
    }

    // Button B interaction
    if (M5.BtnB.wasPressed()) {
        unsigned long now = millis();
        bool isDoubleClick = (now - g_lastBtnBPressTime <= 350);
        g_lastBtnBPressTime = now;

        if (g_appState == AppState::CAMERA_READY) {
            if (g_inLiveViewMode) {
                if (isDoubleClick) {
                    // Double-click B in LiveView: Toggle Selfie Mirror
                    g_liveViewStream.setMirror(!g_liveViewStream.isMirror());
                    Serial.printf("[UI] LiveView Mirror: %d\n", g_liveViewStream.isMirror());
                } else {
                    // Single-click B in LiveView: Enter Dashboard menu
                    g_inLiveViewMode = false;
                    Serial.println("[UI] Entered Dashboard menu");
                }
            } else {
                // In Dashboard mode: Short press B directly exits menu and returns to LiveView!
                g_inLiveViewMode = true;
                Serial.println("[UI] Returned to LiveView");
            }
        } else if (g_appState == AppState::CAMERA_ADJUSTING_PARAM) {
            // Short press B in adjust mode: Cancel
            exitAdjustMode(false);
        } else if (g_appState == AppState::IDLE) {
            // Reset connection
            Serial.println("[UI] Resetting state...");
            g_camera.disconnect();
            g_wifiManager.disconnect();
            g_liveViewStream.stop();
            if (g_savedProfile.valid) {
                updateUI("FUJI REMOTE", "Paired: " + g_savedProfile.ssid, "[A] Fast Connect\n[Hold B 3s] Reset Pairing");
            } else {
                updateUI("FUJI REMOTE", "Ready to Connect", "[A] Scan Wi-Fi\n[B] Reset");
            }
        }
    }

    // Long press Button B in IDLE (3 seconds): Clear paired profile
    if (M5.BtnB.pressedFor(3000) && g_appState == AppState::IDLE) {
        StorageManager::getInstance().clearProfile();
        g_savedProfile.valid = false;
        g_savedProfile.ssid = "";
        updateUI("RESET DONE", "Paired camera cleared", "[A] Scan Wi-Fi\n[B] Reset");
        Serial.println("[UI] Cleared NVS profile by long-press B.");
        delay(300);
    }

    // State machine transitions
    if (g_appState == AppState::SCANNING_WIFI) {
        if (g_wifiManager.isScanDone()) {
            const auto& aps = g_wifiManager.getScannedAPs();
            bool foundFuji = false;
            uint8_t fujiChannel = 1;
            uint8_t fujiBssid[6] = {0};

            for (const auto& ap : aps) {
                Serial.printf("  AP: %s (RSSI: %d, Fuji: %d, Ch: %d)\n", ap.ssid.c_str(), ap.rssi, ap.isFujiCamera, ap.channel);
                if (ap.isFujiCamera && !foundFuji) {
                    g_targetSSID = ap.ssid;
                    fujiChannel = ap.channel;
                    memcpy(fujiBssid, ap.bssid, 6);
                    foundFuji = true;
                }
            }

            if (foundFuji) {
                g_appState = AppState::WIFI_FOUND;
                // Save to NVS
                StorageManager::getInstance().saveProfile(g_targetSSID, fujiChannel, fujiBssid);
                g_savedProfile = StorageManager::getInstance().loadProfile();
                updateUI("CAMERA FOUND", g_targetSSID, "[A] Connect\n[B] Re-scan");
            } else if (!aps.empty()) {
                g_targetSSID = aps[0].ssid;
                g_appState = AppState::WIFI_FOUND;
                updateUI("AP FOUND", g_targetSSID, "[A] Connect\n[B] Re-scan");
            } else {
                g_appState = AppState::IDLE;
                updateUI("NOT FOUND", "No camera detected", "[A] Re-scan\n[B] Reset");
            }
        }
    } else if (g_appState == AppState::CONNECTING_WIFI) {
        if (g_wifiManager.isConnected()) {
            g_appState = AppState::CONNECTING_CAMERA;
            updateUI("PTP/IP CONNECT", "Handshaking with camera...", "Connecting...\n[B] Retry");
            
            g_cameraIP = g_wifiManager.getGatewayIP();
            if (g_cameraIP == IPAddress(0, 0, 0, 0)) {
                g_cameraIP = IPAddress(192, 168, 0, 1);
            }
            Serial.printf("[App] WiFi Connected! Camera IP: %s\n", g_cameraIP.toString().c_str());

            if (g_camera.connect(g_cameraIP, 55740)) {
                Serial.println("[App] PTP/IP Connected and Session Opened successfully!");
                g_appState = AppState::CAMERA_READY;
                g_inLiveViewMode = true;
                showDashboard(false);
                M5.Display.fillScreen(TFT_BLACK);
                g_liveViewStream.start(g_cameraIP);
                updateUI("LIVE VIEW", g_targetSSID, "[A] Shoot  |  [B] Dash\n[Dbl B] Mirror");
            } else {
                Serial.println("[App] PTP/IP Handshake Failed!");
                updateUI("PTP FAILED", "Press OK on Camera", "[A] Re-connect\n[B] Retry");
            }
        } else if (g_wifiManager.getState() == WifiState::CONNECT_FAILED) {
            g_appState = AppState::IDLE;
            updateUI("WIFI FAILED", "Could not join AP", "[A] Re-scan\n[B] Reset");
        }
    }

    if (!g_inLiveViewMode) {
        lv_timer_handler();
        delay(5);
    }
}

