#pragma once

#include "lvgl.h"

/*
 * Single source of truth for the dark theme colours.
 *
 * The values are LVGL's official dark palette (lv_theme_default MODE_DARK), pinned
 * here so screens/trays do not depend on theme internals.
 */
#define GUI_COLOR_BG    lv_color_hex(0x15171A) /* screen background */
#define GUI_COLOR_CARD  lv_color_hex(0x282B30) /* status bar / tray */
#define GUI_COLOR_TEXT  lv_color_hex(0xF5F5F5) /* primary text and icons */
#define GUI_COLOR_MUTED lv_color_hex(0x9AA0A6) /* secondary text, inactive icons */
#define GUI_COLOR_BUTTON lv_color_hex(0x32363C) /* circular buttons in the panel */

/* Install the official LVGL dark theme on the display. */
void gui_theme_init(lv_display_t *disp, const lv_font_t *font);
