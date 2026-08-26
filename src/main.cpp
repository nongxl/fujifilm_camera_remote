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

void setup()
{
    auto cfg = M5.config();
    M5.begin(cfg);
    Serial.begin(115200);
    delay(500);

    Serial.println("\n=================================");
    Serial.println("  Fujifilm Camera Remote - Phase 2");
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
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x181818), 0);

    // Title / Status
    g_statusLabel = lv_label_create(scr);
    lv_label_set_text(g_statusLabel, "FUJI REMOTE");
    lv_obj_set_style_text_color(g_statusLabel, lv_color_hex(0x00FF88), 0);
    lv_obj_align(g_statusLabel, LV_ALIGN_TOP_MID, 0, 8);

    // Info details
    g_infoLabel = lv_label_create(scr);
    lv_label_set_text(g_infoLabel, "Press A to scan Wi-Fi");
    lv_obj_set_style_text_color(g_infoLabel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(g_infoLabel, LV_ALIGN_CENTER, 0, -4);

    // Action Hint
    g_btnLabel = lv_label_create(scr);
    lv_label_set_text(g_btnLabel, "[A]: Scan  [B]: Reset");
    lv_obj_set_style_text_color(g_btnLabel, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(g_btnLabel, LV_ALIGN_BOTTOM_MID, 0, -8);

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

    // Button handling
    if (M5.BtnA.wasPressed()) {
        if (g_appState == AppState::IDLE || g_appState == AppState::SCANNING_WIFI) {
            Serial.println("[UI] Starting WiFi scan...");
            g_appState = AppState::SCANNING_WIFI;
            updateUI("SCANNING...", "Searching for Fuji AP", "[Scanning]");
            g_wifiManager.startScan();
        } else if (g_appState == AppState::WIFI_FOUND) {
            Serial.printf("[UI] Connecting to %s...\n", g_targetSSID.c_str());
            g_appState = AppState::CONNECTING_WIFI;
            updateUI("CONNECTING...", g_targetSSID, "Please wait...");
            g_wifiManager.connectTo(g_targetSSID);
        } else if (g_appState == AppState::CAMERA_READY) {
            Serial.println("[UI] Triggering Shutter!");
            updateUI("SHUTTER!", "Capturing photo...", "[A]: Shoot  [B]: Discon");
            bool ok = g_camera.triggerShutter();
            Serial.printf("[Camera] Shutter result: %s\n", ok ? "SUCCESS" : "FAILED");
            delay(200);
            updateUI("CONNECTED", g_targetSSID, "[A]: Shoot  [B]: Discon");
        }
    }

    if (M5.BtnB.wasPressed()) {
        Serial.println("[UI] Resetting connection...");
        g_camera.disconnect();
        g_wifiManager.disconnect();
        g_appState = AppState::IDLE;
        updateUI("FUJI REMOTE", "Press A to scan Wi-Fi", "[A]: Scan  [B]: Reset");
    }

    // State machine updates
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
                // If no FUJIFILM- prefix found, show first AP as option
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
                updateUI("CONNECTED", g_targetSSID, "[A]: Shoot  [B]: Discon");
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
