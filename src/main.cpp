#include <Arduino.h>
#include "../.pio/libdeps/m5stack-sticks3/lvgl/lvgl.h"
#define M5GFX_USING_REAL_LVGL 1
#include <M5Unified.h>

#include "net/WifiManager.h"
#include "camera/fujifilm/FujiCamera.h"

/* Global instances */
static WifiManager g_wifiManager;
static FujiCamera g_camera;

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

enum class SelectedParam {
    ISO,
    APERTURE,
    SHUTTER,
    EV
};

static SelectedParam g_selectedParam = SelectedParam::ISO;

/* System state */
enum class AppState {
    IDLE,
    SCANNING_WIFI,
    WIFI_FOUND,
    CONNECTING_WIFI,
    CONNECTING_CAMERA,
    CAMERA_READY
};

static AppState g_appState = AppState::IDLE;
static String g_targetSSID = "";

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

    // Highlight selected card
    auto highlightCard = [](lv_obj_t* card, bool selected) {
        if (!card) return;
        lv_obj_set_style_border_color(card, selected ? lv_color_hex(0x00FF88) : lv_color_hex(0x444444), 0);
        lv_obj_set_style_border_width(card, selected ? 2 : 1, 0);
        lv_obj_set_style_bg_color(card, selected ? lv_color_hex(0x282828) : lv_color_hex(0x1E1E1E), 0);
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
            if (g_infoLabel) lv_obj_add_flag(g_infoLabel, LV_OBJ_FLAG_HIDDEN);
            updateParameterCards();
        } else {
            lv_obj_add_flag(g_paramContainer, LV_OBJ_FLAG_HIDDEN);
            if (g_infoLabel) lv_obj_clear_flag(g_infoLabel, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void createParamCard(lv_obj_t* parent, const char* title, lv_obj_t*& outCard, lv_obj_t*& outValLabel)
{
    outCard = lv_obj_create(parent);
    lv_obj_set_size(outCard, 105, 42);
    lv_obj_set_style_bg_color(outCard, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_border_color(outCard, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(outCard, 1, 0);
    lv_obj_set_style_radius(outCard, 4, 0);
    lv_obj_set_style_pad_all(outCard, 2, 0);
    lv_obj_clear_flag(outCard, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* titleLbl = lv_label_create(outCard);
    lv_label_set_text(titleLbl, title);
    lv_obj_set_style_text_color(titleLbl, lv_color_hex(0x888888), 0);
    lv_obj_align(titleLbl, LV_ALIGN_TOP_LEFT, 2, 1);

    outValLabel = lv_label_create(outCard);
    lv_label_set_text(outValLabel, "--");
    lv_obj_set_style_text_color(outValLabel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(outValLabel, LV_ALIGN_BOTTOM_RIGHT, -2, -1);
}

void setup()
{
    auto cfg = M5.config();
    M5.begin(cfg);
    Serial.begin(115200);
    delay(300);

    Serial.println("\n=================================");
    Serial.println("  Fujifilm Camera Remote - Phase 3");
    Serial.println("=================================");

    // Initialize LVGL 9
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

    // Title / Status
    g_statusLabel = lv_label_create(scr);
    lv_label_set_text(g_statusLabel, "FUJI REMOTE");
    lv_obj_set_style_text_color(g_statusLabel, lv_color_hex(0x00FF88), 0);
    lv_obj_align(g_statusLabel, LV_ALIGN_TOP_MID, 0, 4);

    // Info details (shown when not connected)
    g_infoLabel = lv_label_create(scr);
    lv_label_set_text(g_infoLabel, "Press A to scan Wi-Fi");
    lv_obj_set_style_text_color(g_infoLabel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(g_infoLabel, LV_ALIGN_CENTER, 0, -4);

    // Parameter Dashboard Container (2x2 grid)
    g_paramContainer = lv_obj_create(scr);
    lv_obj_set_size(g_paramContainer, 230, 95);
    lv_obj_align(g_paramContainer, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(g_paramContainer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_paramContainer, 0, 0);
    lv_obj_set_style_pad_all(g_paramContainer, 0, 0);
    lv_obj_set_flex_flow(g_paramContainer, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(g_paramContainer, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    createParamCard(g_paramContainer, "ISO", g_isoCard, g_isoValLabel);
    createParamCard(g_paramContainer, "Aperture", g_apertureCard, g_apertureValLabel);
    createParamCard(g_paramContainer, "Shutter", g_shutterCard, g_shutterValLabel);
    createParamCard(g_paramContainer, "EV", g_evCard, g_evValLabel);

    showDashboard(false);

    // Action Hint
    g_btnLabel = lv_label_create(scr);
    lv_label_set_text(g_btnLabel, "[A]: Scan  [B]: Reset");
    lv_obj_set_style_text_color(g_btnLabel, lv_color_hex(0x888888), 0);
    lv_obj_align(g_btnLabel, LV_ALIGN_BOTTOM_MID, 0, -4);

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
    g_camera.update();

    if (g_appState == AppState::CAMERA_READY) {
        updateParameterCards();
    }

    // Button A interaction
    if (M5.BtnA.wasPressed()) {
        if (g_appState == AppState::IDLE || g_appState == AppState::SCANNING_WIFI) {
            Serial.println("[UI] Starting WiFi scan...");
            g_appState = AppState::SCANNING_WIFI;
            showDashboard(false);
            updateUI("SCANNING...", "Searching for Fuji AP", "[Scanning]");
            g_wifiManager.startScan();
        } else if (g_appState == AppState::WIFI_FOUND) {
            Serial.printf("[UI] Connecting to %s...\n", g_targetSSID.c_str());
            g_appState = AppState::CONNECTING_WIFI;
            showDashboard(false);
            updateUI("CONNECTING...", g_targetSSID, "Please wait...");
            g_wifiManager.connectTo(g_targetSSID);
        } else if (g_appState == AppState::CAMERA_READY) {
            // Short press A: Trigger Shutter
            Serial.println("[UI] Triggering Shutter!");
            updateUI("SHUTTER!", "Capturing photo...", "[A]: Shoot [B]: Next");
            bool ok = g_camera.triggerShutter();
            Serial.printf("[Camera] Shutter result: %s\n", ok ? "SUCCESS" : "FAILED");
            delay(150);
            updateUI("READY", g_targetSSID, "[A]: Shoot [B]: Param+");
        }
    }

    // Button B interaction
    if (M5.BtnB.wasPressed()) {
        if (g_appState == AppState::CAMERA_READY) {
            // Cycle parameter selection
            if (g_selectedParam == SelectedParam::ISO) g_selectedParam = SelectedParam::APERTURE;
            else if (g_selectedParam == SelectedParam::APERTURE) g_selectedParam = SelectedParam::SHUTTER;
            else if (g_selectedParam == SelectedParam::SHUTTER) g_selectedParam = SelectedParam::EV;
            else g_selectedParam = SelectedParam::ISO;

            updateParameterCards();
            Serial.printf("[UI] Selected Param: %d\n", (int)g_selectedParam);
        } else {
            Serial.println("[UI] Resetting connection...");
            g_camera.disconnect();
            g_wifiManager.disconnect();
            g_appState = AppState::IDLE;
            showDashboard(false);
            updateUI("FUJI REMOTE", "Press A to scan Wi-Fi", "[A]: Scan  [B]: Reset");
        }
    }

    // Long press Button B: Adjust current selected parameter +1 step
    if (M5.BtnB.pressedFor(600) && g_appState == AppState::CAMERA_READY) {
        ExposurePropertyId targetId = ExposurePropertyId::ISO;
        if (g_selectedParam == SelectedParam::APERTURE) targetId = ExposurePropertyId::APERTURE;
        else if (g_selectedParam == SelectedParam::SHUTTER) targetId = ExposurePropertyId::SHUTTER_SPEED;
        else if (g_selectedParam == SelectedParam::EV) targetId = ExposurePropertyId::EXPOSURE_COMPENSATION;

        Serial.printf("[UI] Adjusting parameter %d step +1\n", (int)targetId);
        g_camera.adjustPropertyStep(targetId, 1);
        updateParameterCards();
        delay(200); // Debounce step
    }

    // State machine transitions
    if (g_appState == AppState::SCANNING_WIFI) {
        if (g_wifiManager.isScanDone()) {
            const auto& aps = g_wifiManager.getScannedAPs();
            bool foundFuji = false;
            for (const auto& ap : aps) {
                Serial.printf("  AP: %s (RSSI: %d, Fuji: %d)\n", ap.ssid.c_str(), ap.rssi, ap.isFujiCamera);
                if (ap.isFujiCamera && !foundFuji) {
                    g_targetSSID = ap.ssid;
                    foundFuji = true;
                }
            }

            if (foundFuji) {
                g_appState = AppState::WIFI_FOUND;
                updateUI("CAMERA FOUND", g_targetSSID, "[A]: Connect");
            } else if (!aps.empty()) {
                g_targetSSID = aps[0].ssid;
                g_appState = AppState::WIFI_FOUND;
                updateUI("AP FOUND", g_targetSSID, "[A]: Connect");
            } else {
                g_appState = AppState::IDLE;
                updateUI("NOT FOUND", "No camera detected", "[A]: Re-scan");
            }
        }
    } else if (g_appState == AppState::CONNECTING_WIFI) {
        if (g_wifiManager.isConnected()) {
            g_appState = AppState::CONNECTING_CAMERA;
            updateUI("PTP/IP CONNECT", "Handshaking with camera...", "Connecting...");
            
            IPAddress cameraIP = g_wifiManager.getGatewayIP();
            Serial.printf("[App] WiFi Connected! Gateway IP (Camera): %s\n", cameraIP.toString().c_str());

            if (g_camera.connect(cameraIP, 15740)) {
                Serial.println("[App] PTP/IP Connected and Session Opened successfully!");
                g_appState = AppState::CAMERA_READY;
                showDashboard(true);
                updateUI("READY", g_targetSSID, "[A]: Shoot [B]: Select");
            } else {
                Serial.println("[App] PTP/IP Handshake Failed!");
                updateUI("PTP FAILED", "Check camera pairing mode", "[B]: Retry");
            }
        } else if (g_wifiManager.getState() == WifiState::CONNECT_FAILED) {
            g_appState = AppState::IDLE;
            updateUI("WIFI FAILED", "Could not join AP", "[A]: Re-scan");
        }
    }

    lv_timer_handler();
    delay(5);
}
