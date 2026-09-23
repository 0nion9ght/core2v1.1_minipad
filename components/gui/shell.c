#include "shell.h"

#include "bsp.h"
#include "bus.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "theme.h"

static const char *TAG = "shell";

LV_FONT_DECLARE(icons_20);
LV_FONT_DECLARE(icons_32);
LV_FONT_DECLARE(bebas_neue_20);
LV_FONT_DECLARE(cjk_20);

/* Material Symbols Outlined codepoints (see gui/fonts/icons_20.c). The font's
 * unicode_list maps these escape sequences to glyphs. */
#define ICON_WIFI           "\xEE\x98\xBE" /* U+E63E wifi */
#define ICON_WIFI_OFF       "\xEE\x99\x88" /* U+E648 wifi_off */
#define ICON_BATTERY_0      "\xEE\xAF\x9C" /* U+EBDC battery_0_bar */
#define ICON_BATTERY_1      "\xEE\xAF\x99" /* U+EBD9 battery_1_bar */
#define ICON_BATTERY_2      "\xEE\xAF\xA0" /* U+EBE0 battery_2_bar */
#define ICON_BATTERY_3      "\xEE\xAF\x9D" /* U+EBDD battery_3_bar */
#define ICON_BATTERY_4      "\xEE\xAF\xA2" /* U+EBE2 battery_4_bar */
#define ICON_BATTERY_5      "\xEE\xAF\x94" /* U+EBD4 battery_5_bar */
#define ICON_BATTERY_6      "\xEE\xAF\x92" /* U+EBD2 battery_6_bar */
#define ICON_BATTERY_CHARGE "\xEE\x86\xA3" /* U+E1A3 battery_charging_full */
#define ICON_BATTERY_ALERT  "\xEE\x86\x9C" /* U+E19C battery_alert */
#define ICON_BATTERY_UNK    "\xEE\x86\xA6" /* U+E1A6 battery_unknown */

/* Panel buttons (icons_32) */
#define ICON_POWER      "\xEE\xA2\xAC" /* U+E8AC power_settings_new */
#define ICON_BRIGHTNESS "\xEE\x8E\xAB" /* U+E3AB brightness_6 (placeholder) */
#define ICON_VOLUME     "\xEE\x81\x90" /* U+E050 volume_up (placeholder) */
#define ICON_THEME      "\xEE\x94\x98" /* U+E518 light_mode (placeholder) */
#define ICON_RESTART    "\xEF\x81\x93" /* U+F053 restart_alt (placeholder) */

/* Msgbox strings, escaped so the source stays ASCII (UTF-8 of the CJK glyphs in
 * cjk_20): 确定要关机吗？ / 关机 / 取消 */
#define TXT_POWER_ASK       "\xE7\xA1\xAE\xE5\xAE\x9A\xE8\xA6\x81\xE5\x85\xB3\xE6\x9C\xBA\xE5\x90\x97\xEF\xBC\x9F"
#define TXT_POWER_OFF       "\xE5\x85\xB3\xE6\x9C\xBA"
#define TXT_CANCEL          "\xE5\x8F\x96\xE6\xB6\x88"

#define SHELL_STATUSBAR_H 24
#define SHELL_TRAY_RADIUS 8
#define SHELL_TRAY_PAD    16
#define SHELL_ANIM_MS     200

/* Panel grid: 3 columns x 2 rows of 96x92 cells, 64px round buttons, 32px icons */
#define SHELL_CELL_W    96
#define SHELL_CELL_H    92
#define SHELL_BTN_SIZE  64
#define SHELL_GRID_COLS 3
#define SHELL_GRID_ROWS 2

#define SHELL_VIB_BTN_MS   60
#define SHELL_VIB_POWER_MS 120

/* Latch thresholds: a slow drag needs this much travel, a quick flick only needs a
 * fast last movement (phones use velocity; without it short flicks spring back). */
#define SHELL_DRAG_OPEN_RATIO  25 /* % of tray height */
#define SHELL_DRAG_CLOSE_RATIO 20 /* % of tray height */
#define SHELL_FLING_PX         10 /* px of finger movement in the last frame */
#define SHELL_TAP_PX           8  /* below this travel a gesture counts as a tap */
#define SHELL_DRAG_CLICK_PX    10 /* beyond this, a press on a button is a panel drag */

#define SHELL_LOW_BATTERY_PCT 15

static lv_obj_t *s_page_area;
static lv_obj_t *s_status_bar;
static lv_obj_t *s_wifi_label;
static lv_obj_t *s_batt_icon;
static lv_obj_t *s_batt_text;
static lv_obj_t *s_tray;
static lv_obj_t *s_handle;
static lv_obj_t *s_wifi_btn_icon;
static bool s_wifi_enabled = true;
static bool s_panel_dragged;
static int s_drag_travel;

static int s_screen_w;
static int s_screen_h;
static int s_tray_h;
static int s_tray_closed_y;
static int s_tray_open_y;
static int s_anim_target;
static bool s_dragging;
static int s_vect_last; /* last non-zero finger delta, for fling detection */

/* Latest bus state, applied on the LVGL task via shell_state_timer_cb. */
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bus_net_state_t s_net;
static bus_power_state_t s_power;
static uint32_t s_pending_seq;
static uint32_t s_applied_seq;

/* Button events must not be coalesced (a press + release can arrive inside one
 * timer period), so they go through a small ring drained by the shell timer. */
#define SHELL_BTN_QUEUE 8
static bus_button_t s_btn_q[SHELL_BTN_QUEUE];
static uint8_t s_btn_head;
static uint8_t s_btn_tail;

/* Defined in the tray section, used by the state timer above it. */
static void tray_animate_to(int target_y);

/* ---------------------------------------------------------------- bus -> UI */

static void on_net_event(bus_topic_t topic, const bus_payload_t *payload, void *arg)
{
    (void)topic;
    (void)arg;
    portENTER_CRITICAL(&s_state_lock);
    s_net = payload->net;
    s_pending_seq++;
    portEXIT_CRITICAL(&s_state_lock);
}

static void on_power_event(bus_topic_t topic, const bus_payload_t *payload, void *arg)
{
    (void)topic;
    (void)arg;
    portENTER_CRITICAL(&s_state_lock);
    s_power = payload->power;
    s_pending_seq++;
    portEXIT_CRITICAL(&s_state_lock);
}

static void on_button_event(bus_topic_t topic, const bus_payload_t *payload, void *arg)
{
    (void)topic;
    (void)arg;
    portENTER_CRITICAL(&s_state_lock);
    const uint8_t next = (uint8_t)((s_btn_tail + 1) % SHELL_BTN_QUEUE);
    if (next != s_btn_head) { /* drop when full rather than block the bus task */
        s_btn_q[s_btn_tail] = payload->button;
        s_btn_tail = next;
    }
    portEXIT_CRITICAL(&s_state_lock);
}

/* Called from the LVGL task only. */
static bool button_pop(bus_button_t *out)
{
    bool got = false;
    portENTER_CRITICAL(&s_state_lock);
    if (s_btn_head != s_btn_tail) {
        *out = s_btn_q[s_btn_head];
        s_btn_head = (uint8_t)((s_btn_head + 1) % SHELL_BTN_QUEUE);
        got = true;
    }
    portEXIT_CRITICAL(&s_state_lock);
    return got;
}

static void apply_wifi(const bus_net_state_t *net)
{
    const bool connected = (net->state == BUS_NET_CONNECTED);
    s_wifi_enabled = net->enabled;

    /* enabled but not yet connected = still trying; disabled = switched off */
    const char *icon = net->enabled ? ICON_WIFI : ICON_WIFI_OFF;
    const lv_color_t color = connected ? GUI_COLOR_TEXT : GUI_COLOR_MUTED;

    lv_label_set_text(s_wifi_label, icon);
    lv_obj_set_style_text_color(s_wifi_label, color, 0);

    if (s_wifi_btn_icon != NULL) {
        lv_label_set_text(s_wifi_btn_icon, icon);
        lv_obj_set_style_text_color(s_wifi_btn_icon, color, 0);
    }
}

static void apply_power(const bus_power_state_t *power)
{
    static const char *const bars[7] = {
        ICON_BATTERY_0, ICON_BATTERY_1, ICON_BATTERY_2, ICON_BATTERY_3,
        ICON_BATTERY_4, ICON_BATTERY_5, ICON_BATTERY_6,
    };

    if (!power->valid || power->level < 0) {
        lv_label_set_text(s_batt_icon, ICON_BATTERY_UNK);
        lv_obj_set_style_text_color(s_batt_icon, GUI_COLOR_MUTED, 0);
        lv_label_set_text(s_batt_text, "-");
        lv_obj_set_style_text_color(s_batt_text, GUI_COLOR_MUTED, 0);
        return;
    }

    const char *icon = ICON_BATTERY_UNK;
    if (power->charging) {
        icon = ICON_BATTERY_CHARGE;
    } else if (power->level <= SHELL_LOW_BATTERY_PCT) {
        icon = ICON_BATTERY_ALERT;
    } else {
        int idx = ((int)power->level * 6 + 50) / 100; /* nearest of the 7 bar glyphs */
        if (idx < 0) {
            idx = 0;
        } else if (idx > 6) {
            idx = 6;
        }
        icon = bars[idx];
    }

    lv_label_set_text(s_batt_icon, icon);
    lv_obj_set_style_text_color(s_batt_icon, GUI_COLOR_TEXT, 0);

    char text[8];
    lv_snprintf(text, sizeof(text), power->charging ? "%d+" : "%d", (int)power->level);
    lv_label_set_text(s_batt_text, text);
    lv_obj_set_style_text_color(s_batt_text, GUI_COLOR_TEXT, 0);
}

static void shell_state_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    portENTER_CRITICAL(&s_state_lock);
    const uint32_t seq = s_pending_seq;
    const bus_net_state_t net = s_net;
    const bus_power_state_t power = s_power;
    portEXIT_CRITICAL(&s_state_lock);

    if (seq != s_applied_seq) {
        s_applied_seq = seq;
        apply_wifi(&net);
        apply_power(&power);
    }

    /* Back key (bottom-bezel right key): close the panel if it is out. */
    bus_button_t btn;
    while (button_pop(&btn)) {
        if (btn.id == BUS_BUTTON_RIGHT && btn.action == BUS_BUTTON_PRESS &&
            s_anim_target > s_tray_closed_y) {
            tray_animate_to(s_tray_closed_y);
            ESP_LOGI(TAG, "back key: closing tray");
        }
    }
}

/* ------------------------------------------------------------------- tray */

static void tray_apply_y(int32_t y)
{
    lv_obj_set_y(s_tray, y);
}

static void tray_anim_cb(void *var, int32_t value)
{
    (void)var;
    lv_obj_set_y(s_tray, value);
}

static void tray_anim_completed_cb(lv_anim_t *anim)
{
    (void)anim;

    const int y = lv_obj_get_y(s_tray);
    const bool at_open = (y >= s_tray_open_y - 2);
    const bool at_closed = (y <= s_tray_closed_y + 2);

    if (!at_open && !at_closed) {
        /* Interrupted mid-flight (a gesture cancelled the animation): never leave
         * the panel resting half-open - finish the intended travel. */
        tray_animate_to(s_anim_target);
        return;
    }

    lv_area_t hc = {0};
    if (s_handle != NULL) {
        lv_obj_get_coords(s_handle, &hc);
    }

    if (at_closed) {
        ESP_LOGI(TAG, "tray closed (tray_y=%d handle_y=%d)", y, (int)hc.y1);
    } else {
        ESP_LOGI(TAG, "tray opened (tray_y=%d handle_y=%d)", y, (int)hc.y1);
    }
}

static void tray_animate_to(int target_y)
{
    /* Explicitly stop any in-flight animation: one animation owns the tray, so a
     * completed callback always corresponds to a real settle. */
    lv_anim_delete(s_tray, tray_anim_cb);
    s_anim_target = target_y;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_tray);
    lv_anim_set_values(&a, lv_obj_get_y(s_tray), target_y);
    lv_anim_set_time(&a, SHELL_ANIM_MS);
    lv_anim_set_exec_cb(&a, tray_anim_cb);
    lv_anim_set_completed_cb(&a, tray_anim_completed_cb);
    lv_anim_start(&a);
}

/* One gesture, one owner: the object that was pressed keeps the gesture even when
 * the finger slides off it (LV_OBJ_FLAG_PRESS_LOCK). Without that the press would
 * be lost mid-drag and re-captured by the other object, and the two settle
 * animations would fight over the tray position. */
static void tray_begin_drag(void)
{
    lv_anim_delete(s_tray, tray_anim_cb);
    s_dragging = true;
    s_vect_last = 0;
    s_drag_travel = 0;
    s_panel_dragged = false;
}

static void tray_end_drag(int travelled, int threshold, bool fling_latches,
                          int target_when_latched, int target_otherwise)
{
    s_dragging = false;
    tray_animate_to((travelled >= threshold || fling_latches) ? target_when_latched
                                                              : target_otherwise);
}

static void drag_apply(void)
{
    lv_point_t vect;
    lv_indev_get_vect(lv_indev_active(), &vect);
    const int dy = vect.y;
    if (dy != 0) {
        s_vect_last = dy;
        s_drag_travel += (dy < 0) ? -dy : dy;
        if (s_drag_travel > SHELL_DRAG_CLICK_PX) {
            s_panel_dragged = true; /* this gesture is a drag, drop the button click */
        }
    }

    int y = lv_obj_get_y(s_tray) + dy;
    if (y < s_tray_closed_y) {
        y = s_tray_closed_y;
    } else if (y > s_tray_open_y) {
        y = s_tray_open_y;
    }
    tray_apply_y(y);
}

static bool fling_is_fast(int sign)
{
    return (sign > 0) ? (s_vect_last >= SHELL_FLING_PX) : (s_vect_last <= -SHELL_FLING_PX);
}

/* Pull down from the status bar to open the tray; tapping it while open closes. */
static void statusbar_event_cb(lv_event_t *e)
{
    const int range = s_tray_open_y - s_tray_closed_y;

    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        tray_begin_drag();
        break;
    case LV_EVENT_PRESSING:
        drag_apply();
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST: {
        const int travelled = lv_obj_get_y(s_tray) - s_tray_closed_y;
        const bool tray_was_open = (s_anim_target >= s_tray_open_y - 2);
        if (travelled <= SHELL_TAP_PX && tray_was_open) {
            /* Tap on the status bar: collapse the panel. */
            tray_end_drag(0, 1, false, s_tray_closed_y, s_tray_closed_y);
            break;
        }
        tray_end_drag(travelled, range * SHELL_DRAG_OPEN_RATIO / 100, fling_is_fast(+1),
                      s_tray_open_y, s_tray_closed_y);
        break;
    }
    default:
        break;
    }
}

/* Swipe up on the tray to close it. */
static void tray_event_cb(lv_event_t *e)
{
    const int range = s_tray_open_y - s_tray_closed_y;

    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        tray_begin_drag();
        break;
    case LV_EVENT_PRESSING:
        drag_apply();
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        tray_end_drag(s_tray_open_y - lv_obj_get_y(s_tray), range * SHELL_DRAG_CLOSE_RATIO / 100,
                      fling_is_fast(-1), s_tray_closed_y, s_tray_open_y);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ build */

/* ------------------------------------------------------- panel buttons */

static void wifi_button_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || s_panel_dragged) {
        return; /* the gesture was a panel drag, not a click */
    }
    const bus_payload_t payload = {.wifi_set = {.enabled = !s_wifi_enabled}};
    (void)bus_publish(BUS_TOPIC_WIFI_SET, &payload);
    ESP_LOGI(TAG, "wifi button -> %s", s_wifi_enabled ? "OFF" : "ON");
    bsp_vibrate_pulse(SHELL_VIB_BTN_MS);
}

static void msgbox_cancel_cb(lv_event_t *e)
{
    lv_msgbox_close((lv_obj_t *)lv_event_get_user_data(e));
    ESP_LOGI(TAG, "power cancelled");
}

static void msgbox_power_off_cb(lv_event_t *e)
{
    lv_msgbox_close((lv_obj_t *)lv_event_get_user_data(e));
    ESP_LOGW(TAG, "power off confirmed");
    bsp_vibrate_pulse(SHELL_VIB_POWER_MS);
    vTaskDelay(pdMS_TO_TICKS(SHELL_VIB_POWER_MS + 20)); /* let the buzz be felt */
    bsp_power_off();
}

static void open_power_msgbox(void)
{
    lv_obj_t *mbox = lv_msgbox_create(NULL);
    if (mbox == NULL) {
        ESP_LOGE(TAG, "lv_msgbox_create failed");
        return;
    }

    lv_obj_t *title = lv_msgbox_add_title(mbox, TXT_POWER_ASK);
    if (title != NULL) {
        lv_obj_set_style_text_font(title, &cjk_20, 0);
    }

    lv_obj_t *btn_off = lv_msgbox_add_footer_button(mbox, TXT_POWER_OFF);
    if (btn_off != NULL) {
        lv_obj_set_style_text_font(btn_off, &cjk_20, 0);
        lv_obj_add_event_cb(btn_off, msgbox_power_off_cb, LV_EVENT_CLICKED, mbox);
    }

    lv_obj_t *btn_cancel = lv_msgbox_add_footer_button(mbox, TXT_CANCEL);
    if (btn_cancel != NULL) {
        lv_obj_set_style_text_font(btn_cancel, &cjk_20, 0);
        lv_obj_add_event_cb(btn_cancel, msgbox_cancel_cb, LV_EVENT_CLICKED, mbox);
    }

    ESP_LOGI(TAG, "power confirmation shown");
}

static void power_button_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || s_panel_dragged) {
        return;
    }
    bsp_vibrate_pulse(SHELL_VIB_BTN_MS);
    open_power_msgbox();
}

/*
 * Round button in the panel grid. Buttons carry LV_OBJ_FLAG_EVENT_BUBBLE so the
 * tray still sees the press and can drag the panel; a click that turned into a
 * drag is dropped by the callbacks above (s_panel_dragged).
 */
static void make_round_button(int col, int row, const char *icon, bool clickable,
                              lv_event_cb_t cb, lv_obj_t **out_icon)
{
    lv_obj_t *btn = lv_button_create(s_tray);
    lv_obj_set_size(btn, SHELL_BTN_SIZE, SHELL_BTN_SIZE);
    lv_obj_set_pos(btn,
                   SHELL_TRAY_PAD + (col * SHELL_CELL_W) + ((SHELL_CELL_W - SHELL_BTN_SIZE) / 2),
                   SHELL_TRAY_PAD + (row * SHELL_CELL_H) + ((SHELL_CELL_H - SHELL_BTN_SIZE) / 2));
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, GUI_COLOR_BUTTON, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_clickable(btn, clickable);

    lv_obj_t *icon_label = lv_label_create(btn);
    lv_obj_set_style_text_font(icon_label, &icons_32, 0);
    lv_obj_set_style_text_color(icon_label, GUI_COLOR_TEXT, 0);
    lv_label_set_text(icon_label, icon);
    lv_obj_center(icon_label);

    if (!clickable) {
        /* Placeholder: dimmed and inert so it cannot look broken. */
        lv_obj_set_style_opa(btn, LV_OPA_40, 0);
    }
    if (cb != NULL) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    }
    if (out_icon != NULL) {
        *out_icon = icon_label;
    }
}

static void build_panel_buttons(void)
{
    /* Row 0: Wi-Fi, power, brightness. Row 1: volume, theme, restart. */
    make_round_button(0, 0, ICON_WIFI, true, wifi_button_cb, &s_wifi_btn_icon);
    make_round_button(1, 0, ICON_POWER, true, power_button_cb, NULL);
    make_round_button(2, 0, ICON_BRIGHTNESS, false, NULL, NULL);
    make_round_button(0, 1, ICON_VOLUME, false, NULL, NULL);
    make_round_button(1, 1, ICON_THEME, false, NULL, NULL);
    make_round_button(2, 1, ICON_RESTART, false, NULL, NULL);
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, const char *text,
                            lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_label_set_text(label, text);
    return label;
}

/* LVGL has a single radius for all corners. To get square top corners against the
 * status bar (no hairline gap) while keeping the bottom rounded, the panel is a
 * rounded rectangle with two square patches in its top corners. */
static void add_corner_patch(lv_obj_t *parent, int x)
{
    lv_obj_t *patch = lv_obj_create(parent);
    lv_obj_set_size(patch, SHELL_TRAY_RADIUS, SHELL_TRAY_RADIUS);
    lv_obj_set_pos(patch, x, 0);
    lv_obj_set_style_bg_color(patch, GUI_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(patch, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(patch, 0, 0);
    lv_obj_set_style_border_width(patch, 0, 0);
    lv_obj_set_style_pad_all(patch, 0, 0);
    lv_obj_set_clickable(patch, false);
}

static void build_status_bar(int width)
{
    s_status_bar = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_status_bar, width, SHELL_STATUSBAR_H);
    lv_obj_set_pos(s_status_bar, 0, 0);
    lv_obj_set_style_bg_color(s_status_bar, GUI_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(s_status_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_status_bar, 0, 0);
    lv_obj_set_style_radius(s_status_bar, 0, 0);
    lv_obj_set_style_pad_all(s_status_bar, 0, 0);
    lv_obj_set_style_pad_right(s_status_bar, 8, 0);
    lv_obj_set_style_pad_column(s_status_bar, 6, 0);
    lv_obj_set_scrollable(s_status_bar, false);
    lv_obj_set_clickable(s_status_bar, true);
    lv_obj_add_flag(s_status_bar, LV_OBJ_FLAG_PRESS_LOCK);

    lv_obj_set_flex_flow(s_status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_status_bar, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    s_wifi_label = make_label(s_status_bar, &icons_20, ICON_WIFI_OFF, GUI_COLOR_MUTED);
    s_batt_icon = make_label(s_status_bar, &icons_20, ICON_BATTERY_UNK, GUI_COLOR_MUTED);
    s_batt_text = make_label(s_status_bar, &bebas_neue_20, "-", GUI_COLOR_MUTED);

    lv_obj_add_event_cb(s_status_bar, statusbar_event_cb, LV_EVENT_ALL, NULL);
}

static void build_tray(int width)
{
    s_tray = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_tray, width, s_tray_h);
    lv_obj_set_pos(s_tray, 0, s_tray_closed_y);
    lv_obj_set_style_bg_color(s_tray, GUI_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(s_tray, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_tray, SHELL_TRAY_RADIUS, 0);
    lv_obj_set_style_border_width(s_tray, 0, 0);
    lv_obj_set_style_pad_all(s_tray, 0, 0);
    lv_obj_set_scrollable(s_tray, false);
    lv_obj_set_clickable(s_tray, true);
    lv_obj_add_flag(s_tray, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(s_tray, tray_event_cb, LV_EVENT_ALL, NULL);

    add_corner_patch(s_tray, 0);
    add_corner_patch(s_tray, width - SHELL_TRAY_RADIUS);

    /* Drag handle along the panel's bottom edge: it doubles as the affordance for
     * "swipe up to close" (dragging the panel body works too). Not clickable, so
     * drags fall through to the tray. */
    s_handle = lv_obj_create(s_tray);
    lv_obj_set_size(s_handle, 32, 4);
    lv_obj_align(s_handle, LV_ALIGN_BOTTOM_MID, 0, -SHELL_TRAY_PAD);
    lv_obj_set_style_bg_color(s_handle, GUI_COLOR_MUTED, 0);
    lv_obj_set_style_bg_opa(s_handle, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_handle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_handle, 0, 0);
    lv_obj_set_clickable(s_handle, false);

    build_panel_buttons();
}

esp_err_t shell_init(lv_display_t *disp)
{
    if (disp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_screen_w = lv_display_get_horizontal_resolution(disp);
    s_screen_h = lv_display_get_vertical_resolution(disp);

    /* The tray covers everything below the status bar when open. */
    s_tray_h = s_screen_h - SHELL_STATUSBAR_H;
    s_tray_closed_y = SHELL_STATUSBAR_H - s_tray_h;
    s_tray_open_y = SHELL_STATUSBAR_H;
    s_anim_target = s_tray_closed_y;

    s_page_area = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_page_area, s_screen_w, s_screen_h - SHELL_STATUSBAR_H);
    lv_obj_set_pos(s_page_area, 0, SHELL_STATUSBAR_H);
    lv_obj_set_style_bg_opa(s_page_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_page_area, 0, 0);
    lv_obj_set_style_pad_all(s_page_area, 0, 0);
    lv_obj_set_scrollable(s_page_area, false);

    build_tray(s_screen_w);
    build_status_bar(s_screen_w);

    if (bus_subscribe(BUS_TOPIC_NET_STATE, on_net_event, NULL) != ESP_OK ||
        bus_subscribe(BUS_TOPIC_POWER_STATE, on_power_event, NULL) != ESP_OK ||
        bus_subscribe(BUS_TOPIC_BUTTON, on_button_event, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "failed to subscribe to status topics");
        return ESP_FAIL;
    }

    /* 50 ms keeps the back-key reaction snappy; the timer itself is trivial. */
    lv_timer_create(shell_state_timer_cb, 50, NULL);

    ESP_LOGI(TAG, "status bar %dpx + tray %dpx ready", SHELL_STATUSBAR_H, s_tray_h);
    return ESP_OK;
}

lv_obj_t *shell_page_area(void)
{
    return s_page_area;
}
