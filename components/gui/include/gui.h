#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * gui - LVGL display stack and screens.
 *
 * Owns the LVGL port (task, tick, flush to the panel via bsp) and every screen.
 * Screens never call services directly: they subscribe to bus topics and render
 * whatever state arrives. The LVGL task is the only task that may touch LVGL
 * objects, so bus callbacks only stash data; a periodic lv_timer applies it.
 */
esp_err_t gui_start(void);

#ifdef __cplusplus
}
#endif
