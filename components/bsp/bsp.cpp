#include "bsp.h"

#include <M5Unified.hpp>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "bsp";

#define BSP_FALLBACK_WIDTH  320
#define BSP_FALLBACK_HEIGHT 240

static bool s_ready;
/* Serialises access to the touch controller: the LVGL input device reads it
 * directly while bsp_poll() runs M5.update() (buttons) from another task. */
static SemaphoreHandle_t s_bus_lock;
static esp_timer_handle_t s_vib_timer;

static void bsp_vib_stop_cb(void *arg)
{
    (void)arg;
    M5.Power.setVibration(0);
}

extern "C" esp_err_t bsp_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    auto cfg = M5.config();
    M5.begin(cfg);

    s_bus_lock = xSemaphoreCreateMutex();
    if (s_bus_lock == NULL) {
        ESP_LOGE(TAG, "failed to create bus lock");
        return ESP_ERR_NO_MEM;
    }

    const esp_timer_create_args_t vib_args = {
        .callback = bsp_vib_stop_cb,
        .name = "bsp_vib_stop",
    };
    if (esp_timer_create(&vib_args, &s_vib_timer) != ESP_OK) {
        ESP_LOGE(TAG, "failed to create vibration timer");
        return ESP_ERR_NO_MEM;
    }

    if (M5.Display.width() <= 0 || M5.Display.height() <= 0) {
        ESP_LOGE(TAG, "no display detected (w=%d h=%d)", (int)M5.Display.width(),
                 (int)M5.Display.height());
        return ESP_ERR_NOT_FOUND;
    }

    /* LVGL renders RGB565 little-endian; the ILI9341-class panel expects
     * big-endian bytes on the wire. White/black are byte-order invariant, but
     * set it so future colour UI is correct too. */
    M5.Display.setSwapBytes(true);
    M5.Display.fillScreen(TFT_WHITE);

    s_ready = true;
    ESP_LOGI(TAG, "board ready: display %dx%d, rtc=%s", (int)M5.Display.width(),
             (int)M5.Display.height(), M5.Rtc.isEnabled() ? "yes" : "no");
    return ESP_OK;
}

extern "C" int bsp_display_width(void)
{
    int w = s_ready ? (int)M5.Display.width() : 0;
    return w > 0 ? w : BSP_FALLBACK_WIDTH;
}

extern "C" int bsp_display_height(void)
{
    int h = s_ready ? (int)M5.Display.height() : 0;
    return h > 0 ? h : BSP_FALLBACK_HEIGHT;
}

extern "C" esp_err_t bsp_display_push_rgb565(int x, int y, int w, int h, const void *pixels)
{
    if (!s_ready || pixels == NULL || w <= 0 || h <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (x < 0 || y < 0 || (x + w) > bsp_display_width() || (y + h) > bsp_display_height()) {
        ESP_LOGE(TAG, "flush out of bounds: x=%d y=%d w=%d h=%d", x, y, w, h);
        return ESP_ERR_INVALID_ARG;
    }

    M5.Display.pushImage(x, y, w, h, reinterpret_cast<const lgfx::rgb565_t *>(pixels));
    return ESP_OK;
}

extern "C" esp_err_t bsp_rtc_get_datetime(bsp_datetime_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!M5.Rtc.isEnabled()) {
        return ESP_ERR_INVALID_STATE;
    }

    m5::rtc_datetime_t dt{};
    if (!M5.Rtc.getDateTime(&dt)) {
        return ESP_FAIL;
    }

    out->year = dt.date.year;
    out->month = dt.date.month;
    out->day = dt.date.date;
    out->hour = dt.time.hours;
    out->minute = dt.time.minutes;
    out->second = dt.time.seconds;
    return ESP_OK;
}

extern "C" esp_err_t bsp_rtc_set_datetime(const bsp_datetime_t *in)
{
    if (in == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!M5.Rtc.isEnabled()) {
        return ESP_ERR_INVALID_STATE;
    }

    m5::rtc_datetime_t dt{};
    dt.date.year = in->year;
    dt.date.month = in->month;
    dt.date.date = in->day;
    dt.time.hours = in->hour;
    dt.time.minutes = in->minute;
    dt.time.seconds = in->second;
    M5.Rtc.setDateTime(dt);
    return ESP_OK;
}

extern "C" esp_err_t bsp_power_get(bsp_power_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const int32_t level = M5.Power.getBatteryLevel();
    const int16_t mv = M5.Power.getBatteryVoltage();
    const auto charging = M5.Power.isCharging();

    out->level = (level >= 0 && level <= 100) ? (int8_t)level : -1;
    out->millivolt = (mv > 0) ? mv : -1;
    out->charging = (charging == m5::Power_Class::is_charging);
    out->valid = (out->level >= 0) || (out->millivolt > 0);
    return out->valid ? ESP_OK : ESP_FAIL;
}

extern "C" esp_err_t bsp_touch_init(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    /* M5.begin() already binds the FT6336U to the display as a touch device; the
     * first poll runs the controller init sequence inside M5GFX. */
    int32_t x = 0;
    int32_t y = 0;
    const uint_fast8_t count = M5.Display.getTouch(&x, &y);
    ESP_LOGI(TAG, "touch polled (%s)", count > 0 ? "point already down" : "no point at boot");
    return ESP_OK;
}

extern "C" esp_err_t bsp_touch_read(int *x, int *y, bool *pressed)
{
    if (x == NULL || y == NULL || pressed == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    int32_t tx = 0;
    int32_t ty = 0;
    uint_fast8_t count = 0;

    xSemaphoreTake(s_bus_lock, portMAX_DELAY);
    count = M5.Display.getTouch(&tx, &ty);
    xSemaphoreGive(s_bus_lock);

    *pressed = (count > 0);
    if (*pressed) {
        *x = (int)tx;
        *y = (int)ty;
    }
    return ESP_OK;
}

extern "C" void bsp_poll(void)
{
    if (!s_ready) {
        return;
    }

    /* M5.update() services the bottom-bezel virtual keys (BtnA/BtnC) - they only
     * change state while this runs. */
    xSemaphoreTake(s_bus_lock, portMAX_DELAY);
    M5.update();
    xSemaphoreGive(s_bus_lock);
}

static m5::Button_Class *button_for(bsp_button_t id)
{
    switch (id) {
    case BSP_BUTTON_LEFT:
        return &M5.BtnA;
    case BSP_BUTTON_CENTER:
        return &M5.BtnB;
    case BSP_BUTTON_RIGHT:
        return &M5.BtnC;
    default:
        return nullptr;
    }
}

extern "C" bool bsp_button_is_pressed(bsp_button_t id)
{
    m5::Button_Class *btn = button_for(id);
    return (btn != nullptr) && btn->isPressed();
}

extern "C" bool bsp_button_was_pressed(bsp_button_t id)
{
    m5::Button_Class *btn = button_for(id);
    return (btn != nullptr) && btn->wasPressed();
}

extern "C" bool bsp_button_was_released(bsp_button_t id)
{
    m5::Button_Class *btn = button_for(id);
    return (btn != nullptr) && btn->wasReleased();
}

extern "C" bool bsp_button_pressed_for(bsp_button_t id, uint32_t ms)
{
    m5::Button_Class *btn = button_for(id);
    return (btn != nullptr) && btn->pressedFor(ms);
}

extern "C" void bsp_vibrate(uint8_t level)
{
    if (!s_ready) {
        return;
    }
    M5.Power.setVibration(level);
}

extern "C" void bsp_vibrate_pulse(uint16_t ms)
{
    if (!s_ready) {
        return;
    }
    M5.Power.setVibration(255);
    (void)esp_timer_start_once(s_vib_timer, (uint64_t)ms * 1000);
}

extern "C" void bsp_power_off(void)
{
    ESP_LOGW(TAG, "powering off via PMU");
    M5.Power.powerOff();
    /* If the PMU refused, never return to a half-dead state. */
    esp_restart();
}
