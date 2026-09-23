#include "theme.h"

void gui_theme_init(lv_display_t *disp, const lv_font_t *font)
{
    /* Official LVGL dark variant (MODE_DARK) as the base theme. Screens still pin
     * their own colours from theme.h so rendering does not depend on theme internals. */
    lv_theme_t *theme = lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_BLUE),
                                              lv_palette_main(LV_PALETTE_RED), true, font);
    lv_display_set_theme(disp, theme);
    lv_obj_set_style_bg_color(lv_screen_active(), GUI_COLOR_BG, 0);
}
