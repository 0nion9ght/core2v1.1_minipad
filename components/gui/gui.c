#include "gui.h"

#include <stdint.h>
#include <string.h>

#include "bsp.h"
#include "boot_anim.h"
#include "bus.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "shell.h"
#include "theme.h"

static const char *TAG = "gui";

/* Bebas Neue, generated with lv_font_conv (see components/gui/fonts/).
 * Size 176 is the largest size where "HH:MM" still fits the 320px width:
 * 4 x 70.38 + 33.06 = 314.6px. Glyph height is 127px on a 240px screen. */
LV_FONT_DECLARE(bebas_neue_176);

#define GUI_TASK_STACK       8192
#define GUI_TASK_PRIO        5
#define GUI_TASK_CORE        1 /* keep core 0 for Wi-Fi */
#define GUI_LOOP_MIN_MS     5
#define GUI_LOOP_MAX_MS     20
#define GUI_STATE_POLL_MS    200
#define GUI_BUF_LINES        80
/* Bebas Neue puts ':' on the baseline (ink 86px at size 176) while digits are
 * 125px tall, so the colon reads as bottom-aligned. Raise it to the optical
 * centre of the digits: (125 - 86) / 2 ~= 20px. */
#define GUI_CLOCK_COLON_LIFT (-20)

static lv_display_t *s_disp;
static lv_obj_t *s_clock_box;
static lv_obj_t *s_clock_hh;
static lv_obj_t *s_clock_colon;
static lv_obj_t *s_clock_mm;

/* Latest state from the bus, applied by the LVGL timer. */
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bus_time_t s_pending_time;
static uint32_t s_pending_seq;
static uint32_t s_applied_seq;

static void gui_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    const int32_t w = lv_area_get_width(area);
    const int32_t h = lv_area_get_height(area);

    if (bsp_display_push_rgb565(area->x1, area->y1, (int)w, (int)h, px_map) != ESP_OK) {
        ESP_LOGE(TAG, "panel push failed at (%d,%d) %dx%d", (int)area->x1, (int)area->y1, (int)w,
                 (int)h);
    }
    lv_display_flush_ready(disp);
}

/* bus -> gui: never touch LVGL here (wrong task, LVGL is not thread-safe). */
static void on_time_event(bus_topic_t topic, const bus_payload_t *payload, void *arg)
{
    (void)arg;
    portENTER_CRITICAL(&s_state_lock);
    if (topic == BUS_TOPIC_TIME_INVALID) {
        s_pending_time.valid = false;
    } else {
        s_pending_time = payload->time;
    }
    s_pending_seq++;
    portEXIT_CRITICAL(&s_state_lock);
}

/* Runs inside the LVGL task: apply the newest bus state to the label. */
static void gui_state_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    portENTER_CRITICAL(&s_state_lock);
    const uint32_t seq = s_pending_seq;
    const bus_time_t t = s_pending_time;
    portEXIT_CRITICAL(&s_state_lock);

    if (seq == s_applied_seq) {
        return;
    }
    s_applied_seq = seq;

    if (t.valid) {
        lv_label_set_text_fmt(s_clock_hh, "%02u", (unsigned)t.hour);
        lv_label_set_text_fmt(s_clock_mm, "%02u", (unsigned)t.minute);
    } else {
        lv_label_set_text(s_clock_hh, "--");
        lv_label_set_text(s_clock_mm, "--");
    }
}

static void *gui_alloc_draw_buf(size_t size)
{
    void *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGW(TAG, "PSRAM draw buffer unavailable, falling back to internal RAM");
        buf = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return buf;
}

/* LVGL pointer input: bsp owns the FT6336U, LVGL just consumes coordinates. */
static void gui_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    int x = 0;
    int y = 0;
    bool pressed = false;

    if (bsp_touch_read(&x, &y, &pressed) == ESP_OK && pressed) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void gui_indev_init(void)
{
    bsp_touch_init();

    lv_indev_t *indev = lv_indev_create();
    if (indev == NULL) {
        ESP_LOGE(TAG, "lv_indev_create failed - touch gestures will not work");
        return;
    }
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, gui_touch_read_cb);
    ESP_LOGI(TAG, "touch indev registered");
}

static void gui_task(void *arg)
{
    (void)arg;

    const int width = bsp_display_width();
    const int height = bsp_display_height();
    const size_t buf_size = (size_t)width * GUI_BUF_LINES * sizeof(lv_color_t);

    lv_init();

    s_disp = lv_display_create(width, height);
    if (s_disp == NULL) {
        ESP_LOGE(TAG, "lv_display_create failed");
        vTaskDelete(NULL);
        return;
    }

    void *buf1 = gui_alloc_draw_buf(buf_size);
    void *buf2 = gui_alloc_draw_buf(buf_size);
    if (buf1 == NULL) {
        ESP_LOGE(TAG, "no draw buffer memory (%u bytes)", (unsigned)buf_size);
        vTaskDelete(NULL);
        return;
    }
    lv_display_set_buffers(s_disp, buf1, buf2, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_disp, gui_flush_cb);

    gui_theme_init(s_disp, LV_FONT_DEFAULT);

    if (shell_init(s_disp) != ESP_OK) {
        ESP_LOGE(TAG, "shell_init failed");
    }

    gui_indev_init();

    /* Clock page: "HH" / ":" / "MM" are separate labels so the colon can be lifted
     * to the optical centre of the digits (see GUI_CLOCK_COLON_LIFT). Widths come
     * from the font advances, so the layout matches a plain "HH:MM" string. */
    lv_obj_t *page = shell_page_area();

    s_clock_box = lv_obj_create(page);
    lv_obj_set_size(s_clock_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_clock_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_clock_box, 0, 0);
    lv_obj_set_style_pad_all(s_clock_box, 0, 0);
    lv_obj_set_style_pad_column(s_clock_box, 0, 0);
    lv_obj_set_scrollable(s_clock_box, false);
    lv_obj_set_flex_flow(s_clock_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_clock_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_center(s_clock_box);

    s_clock_hh = lv_label_create(s_clock_box);
    s_clock_colon = lv_label_create(s_clock_box);
    s_clock_mm = lv_label_create(s_clock_box);

    lv_obj_t *const parts[] = {s_clock_hh, s_clock_colon, s_clock_mm};
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        lv_obj_set_style_text_font(parts[i], &bebas_neue_176, 0);
        lv_obj_set_style_text_color(parts[i], GUI_COLOR_TEXT, 0);
    }
    lv_label_set_text(s_clock_hh, "--");
    lv_label_set_text(s_clock_colon, ":");
    lv_label_set_text(s_clock_mm, "--");
    lv_obj_set_style_translate_y(s_clock_colon, GUI_CLOCK_COLON_LIFT, 0);

    /* Splash last: it must sit above the status bar (both live on the top layer). */
    (void)gui_boot_anim_start();

    lv_timer_create(gui_state_timer_cb, GUI_STATE_POLL_MS, NULL);

    ESP_LOGI(TAG, "clock page ready (%dx%d, %d-line buffers, %u bytes total)", width, height,
             GUI_BUF_LINES, (unsigned)(buf_size * 2));

    int64_t last_tick_us = esp_timer_get_time();
    while (true) {
        const int64_t now_us = esp_timer_get_time();
        lv_tick_inc((uint32_t)((now_us - last_tick_us) / 1000));
        last_tick_us = now_us;

        const uint32_t next_ms = lv_timer_handler();

        /* CONFIG_FREERTOS_HZ is 100 here, so pdMS_TO_TICKS(<10ms) rounds to 0 and
         * vTaskDelay(0) does NOT yield - that turns this loop into a busy loop and
         * starves the idle task (task watchdog). Always block for >= 1 tick, and
         * never sleep past the next LVGL timer. */
        uint32_t wait_ms = next_ms;
        if (wait_ms < GUI_LOOP_MIN_MS) {
            wait_ms = GUI_LOOP_MIN_MS;
        } else if (wait_ms > GUI_LOOP_MAX_MS) {
            wait_ms = GUI_LOOP_MAX_MS;
        }
        TickType_t delay = pdMS_TO_TICKS(wait_ms);
        if (delay == 0) {
            delay = 1;
        }
        vTaskDelay(delay);
    }
}

esp_err_t gui_start(void)
{
    if (bus_subscribe(BUS_TOPIC_TIME_UPDATED, on_time_event, NULL) != ESP_OK ||
        bus_subscribe(BUS_TOPIC_TIME_INVALID, on_time_event, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "failed to subscribe to time topics");
        return ESP_FAIL;
    }

    if (xTaskCreatePinnedToCore(gui_task, "gui", GUI_TASK_STACK, NULL, GUI_TASK_PRIO, NULL,
                                GUI_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "failed to create gui task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
