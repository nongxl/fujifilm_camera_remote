#include <Arduino.h>
#include "../.pio/libdeps/m5stack-sticks3/lvgl/lvgl.h"
#define M5GFX_USING_REAL_LVGL 1
#include <M5Unified.h>

/* LVGL display buffer */
static uint8_t *buf;

/* Display flushing */
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

/* Touch/Button input */
void my_touchpad_read(lv_indev_t * indev, lv_indev_data_t * data)
{
    M5.update();
    
    // Using M5StickS3 button A as an example click
    if (M5.BtnA.isPressed()) {
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

void setup()
{
    auto cfg = M5.config();
    M5.begin(cfg);
    
    // Initialize LVGL
    lv_init();
    lv_tick_set_cb((lv_tick_get_cb_t)millis);
    
    // Allocate buffer
    uint32_t w = M5.Display.width();
    uint32_t h = M5.Display.height();
    buf = (uint8_t *)heap_caps_malloc(w * h / 10 * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    
    // Initialize display driver
    lv_display_t *disp = lv_display_create(w, h);
    lv_display_set_flush_cb(disp, my_disp_flush);
    lv_display_set_buffers(disp, buf, NULL, w * h / 10 * sizeof(lv_color_t), LV_DISPLAY_RENDER_MODE_PARTIAL);
    
    // Initialize input device
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touchpad_read);
    
    // Create simple UI
    lv_obj_t *label = lv_label_create(lv_screen_active());
    lv_label_set_text(label, "Fuji Remote\nPhase 1");
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
}

void loop()
{
    lv_timer_handler(); // let the GUI do its work
    delay(5);
}
