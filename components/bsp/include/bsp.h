#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * bsp - board support for the M5Stack Core2 v1.1.
 *
 * This is the ONLY component allowed to know about board hardware: pins, LCD
 * controller, backlight, I2C devices (RTC). Everything it exposes to the rest of
 * the firmware is a board-agnostic C API, so the display/UI stack can be replaced
 * without touching any other component.
 *
 * Implementation note: the implementation is C++ (M5Unified/M5GFX), the API is C.
 */

typedef struct {
    int16_t year;  /* 1900-2099 */
    int8_t month;  /* 1-12 */
    int8_t day;    /* 1-31 */
    int8_t hour;   /* 0-23 */
    int8_t minute; /* 0-59 */
    int8_t second; /* 0-59 */
} bsp_datetime_t;

/* Bring up the board: I2C, display controller, backlight, RTC. Idempotent. */
esp_err_t bsp_init(void);

int bsp_display_width(void);
int bsp_display_height(void);

/* Push an RGB565 (LVGL little-endian) bitmap region to the panel.
 * pixels must contain w*h uint16_t values. Bounds-checked. */
esp_err_t bsp_display_push_rgb565(int x, int y, int w, int h, const void *pixels);

/* BM8563 RTC access (I2C). Returns ESP_ERR_INVALID_STATE if no RTC is present. */
esp_err_t bsp_rtc_get_datetime(bsp_datetime_t *out);
esp_err_t bsp_rtc_set_datetime(const bsp_datetime_t *in);

/* Battery / charging state from the PMU (AXP2101 + INA3221 on Core2 v1.1). */
typedef struct {
    int8_t level;      /* 0-100; <0 when unknown */
    bool charging;     /* true while charging */
    bool valid;        /* false when the PMU reading is unusable */
    int16_t millivolt; /* battery voltage in mV; <0 when unknown */
} bsp_power_t;

esp_err_t bsp_power_get(bsp_power_t *out);

/* Capacitive touch (FT6336U). Coordinates are in panel pixels (320x240). */
esp_err_t bsp_touch_init(void);
esp_err_t bsp_touch_read(int *x, int *y, bool *pressed);

/* Bottom-bezel virtual keys (the three printed dots below the LCD). M5Unified maps
 * them to BtnA/BtnB/BtnC; they are only updated while bsp_poll() runs. */
typedef enum {
    BSP_BUTTON_LEFT = 0,
    BSP_BUTTON_CENTER,
    BSP_BUTTON_RIGHT,
    BSP_BUTTON_COUNT,
} bsp_button_t;

/* Poll M5Unified (touch, buttons, PMU). Call from exactly ONE task, periodically
 * (~20 ms). bsp_touch_read() and the button getters serve this polled state. */
void bsp_poll(void);

bool bsp_button_is_pressed(bsp_button_t id);
bool bsp_button_was_pressed(bsp_button_t id);
bool bsp_button_was_released(bsp_button_t id);
bool bsp_button_pressed_for(bsp_button_t id, uint32_t ms);

/* Vibration motor: 0 = stop, 255 = max. */
void bsp_vibrate(uint8_t level);

/* Buzz for the given duration and stop automatically (non-blocking). */
void bsp_vibrate_pulse(uint16_t ms);

/* Cut the power through the PMU (AXP2101 on Core2 v1.1). Does not return. */
void bsp_power_off(void);

#ifdef __cplusplus
}
#endif
