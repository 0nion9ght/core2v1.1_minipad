#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * button_service - bottom-bezel virtual keys ("left / center / right").
 *
 * The keys themselves are read by bsp (M5Unified maps the three printed dots below
 * the LCD to BtnA/BtnB/BtnC). This service turns them into bus events so any
 * component can react without owning UI policy:
 *
 *   BUS_TOPIC_BUTTON { id = LEFT|CENTER|RIGHT, action = PRESS|RELEASE|LONG }
 *
 * A short vibration confirms every press.
 */
esp_err_t button_service_start(void);

#ifdef __cplusplus
}
#endif
