#pragma once

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * shell - the always-on UI chrome, sitting above whatever page is displayed:
 *
 *   status bar (24px, top layer):  right-aligned Wi-Fi icon + battery icon + percentage
 *   pull-down tray (168px):        hidden above the screen; drag from the status bar
 *                                  to reveal it, swipe up or tap outside to close
 *   page area:                     everything below the status bar; pages parent to it
 *
 * The shell is the only place that renders NET_STATE / POWER_STATE, so pages stay
 * independent of where those values come from.
 */
esp_err_t shell_init(lv_display_t *disp);

/* Parent object for page content (the area below the status bar). */
lv_obj_t *shell_page_area(void);

#ifdef __cplusplus
}
#endif
