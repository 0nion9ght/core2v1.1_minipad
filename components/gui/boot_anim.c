#include "boot_anim.h"

#include <math.h>
#include <string.h>

#include "boot_path.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "lvgl.h"
#include "theme.h"

static const char *TAG = "boot_anim";

#define BOOT_ANIM_FRAME_MS 40 /* 25 fps while the word is written */
#define BOOT_ANIM_WRITE_MS 1800
#define BOOT_ANIM_HOLD_MS  600
#define BOOT_ANIM_FADE_MS  400

#define BOOT_ANIM_PEN_R 1.5f /* stroke half-width in px */
#define BOOT_ANIM_Q     16.0f

static lv_obj_t *s_splash;
static lv_obj_t *s_image;
static lv_timer_t *s_timer;
static lv_image_dsc_t s_dsc;
static uint8_t *s_buf;

/* Pen cursor: how much of the path has been drawn, and where inside a segment. */
static float s_drawn;
static float s_step;
static size_t s_poly;
static size_t s_pt;
static float s_frac;

/* Changed-pixel bounding box of the current frame (for a partial invalidate). */
static int s_dirty_x0, s_dirty_y0, s_dirty_x1, s_dirty_y1;

static inline float px_at(size_t i) { return (float)boot_path_points[i].x / BOOT_ANIM_Q; }
static inline float py_at(size_t i) { return (float)boot_path_points[i].y / BOOT_ANIM_Q; }

static void dirty_reset(void)
{
    s_dirty_x0 = 10000;
    s_dirty_y0 = 10000;
    s_dirty_x1 = -1;
    s_dirty_y1 = -1;
}

/* Anti-aliased thick line segment, written straight into the A8 mask. */
static void draw_segment(float ax, float ay, float bx, float by)
{
    const float r = BOOT_ANIM_PEN_R;
    int x0 = (int)floorf(fminf(ax, bx) - r - 1.0f);
    int x1 = (int)ceilf(fmaxf(ax, bx) + r + 1.0f);
    int y0 = (int)floorf(fminf(ay, by) - r - 1.0f);
    int y1 = (int)ceilf(fmaxf(ay, by) + r + 1.0f);

    if (x0 < 0) { x0 = 0; }
    if (y0 < 0) { y0 = 0; }
    if (x1 > (int)s_dsc.header.w - 1) { x1 = (int)s_dsc.header.w - 1; }
    if (y1 > (int)s_dsc.header.h - 1) { y1 = (int)s_dsc.header.h - 1; }
    if (x0 > x1 || y0 > y1) {
        return;
    }

    const float vx = bx - ax, vy = by - ay;
    const float len2 = vx * vx + vy * vy;

    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            const float cx = (float)x + 0.5f, cy = (float)y + 0.5f;
            float t = (len2 > 0.0f) ? ((cx - ax) * vx + (cy - ay) * vy) / len2 : 0.0f;
            if (t < 0.0f) { t = 0.0f; } else if (t > 1.0f) { t = 1.0f; }
            const float dx = cx - (ax + t * vx), dy = cy - (ay + t * vy);
            const float cov = r + 0.5f - sqrtf(dx * dx + dy * dy);
            if (cov <= 0.0f) {
                continue;
            }
            const uint8_t v = (cov >= 1.0f) ? 255 : (uint8_t)(cov * 255.0f);
            const size_t idx = (size_t)y * s_dsc.header.w + (size_t)x;
            if (v > s_buf[idx]) {
                s_buf[idx] = v;
            }
        }
    }

    if (x0 < s_dirty_x0) { s_dirty_x0 = x0; }
    if (y0 < s_dirty_y0) { s_dirty_y0 = y0; }
    if (x1 > s_dirty_x1) { s_dirty_x1 = x1; }
    if (y1 > s_dirty_y1) { s_dirty_y1 = y1; }
}

/* Draw up to `budget` px of path. Returns false once the whole path is drawn. */
static bool pen_advance(float budget)
{
    float left = budget;

    while (left > 0.0f) {
        if (s_poly >= boot_path_poly_count) {
            return false;
        }

        const uint16_t start = boot_path_poly_start[s_poly];
        const uint16_t end = boot_path_poly_start[s_poly + 1];
        const size_t npts = (size_t)(end - start);

        if (npts < 2 || s_pt + 1 >= npts) {
            /* polyline done: hop to the start of the next one (no connecting line) */
            s_poly++;
            s_pt = 0;
            s_frac = 0.0f;
            continue;
        }

        const size_t ia = start + s_pt;
        const float ax = px_at(ia), ay = py_at(ia);
        const float bx = px_at(ia + 1), by = py_at(ia + 1);
        const float seg = hypotf(bx - ax, by - ay);
        const float seg_left = seg * (1.0f - s_frac);
        const float take = (left < seg_left) ? left : seg_left;
        const float f1 = (seg > 0.0f) ? s_frac + take / seg : 1.0f;

        draw_segment(ax + (bx - ax) * s_frac, ay + (by - ay) * s_frac, ax + (bx - ax) * f1,
                     ay + (by - ay) * f1);

        left -= take;
        s_drawn += take;

        if (f1 >= 1.0f) {
            s_frac = 0.0f;
            s_pt++;
        } else {
            s_frac = f1;
        }
    }
    return true;
}

static void splash_set_opa(void *var, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)value, 0);
}

static void fade_completed_cb(lv_anim_t *anim)
{
    (void)anim;

    if (s_splash != NULL) {
        lv_obj_delete(s_splash);
        s_splash = NULL;
        s_image = NULL;
    }
    if (s_buf != NULL) {
        heap_caps_free(s_buf);
        s_buf = NULL;
    }
    ESP_LOGI(TAG, "boot anim: done (path %.0f px)", (double)s_drawn);
}

static void start_fade_out(void)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_splash);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&a, BOOT_ANIM_FADE_MS);
    lv_anim_set_exec_cb(&a, splash_set_opa);
    lv_anim_set_completed_cb(&a, fade_completed_cb);
    lv_anim_start(&a);
}

static void boot_anim_timer_cb(lv_timer_t *timer)
{
    dirty_reset();
    const bool more = pen_advance(s_step);

    if (s_dirty_x1 >= s_dirty_x0 && s_dirty_y1 >= s_dirty_y0) {
        lv_area_t area = {s_dirty_x0, s_dirty_y0, s_dirty_x1, s_dirty_y1};
        lv_obj_invalidate_area(s_image, &area);
    }

    if (more) {
        return;
    }

    /* Whole word written: hold, then fade out. */
    lv_timer_delete(timer);
    s_timer = NULL;
    start_fade_out();
}

esp_err_t gui_boot_anim_start(void)
{
    if (boot_path_poly_count == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    const int width = lv_display_get_horizontal_resolution(lv_display_get_default());
    const int height = lv_display_get_vertical_resolution(lv_display_get_default());

    /* A8 mask for the pen strokes. 76.8 KB: PSRAM here, so internal RAM stays free. */
    s_buf = heap_caps_calloc(1, (size_t)width * height, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_buf == NULL) {
        ESP_LOGE(TAG, "no PSRAM for the splash mask - skipping the boot animation");
        return ESP_ERR_NO_MEM;
    }

    s_splash = lv_obj_create(lv_layer_top());
    if (s_splash == NULL) {
        heap_caps_free(s_buf);
        s_buf = NULL;
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_size(s_splash, width, height);
    lv_obj_set_pos(s_splash, 0, 0);
    lv_obj_set_style_bg_color(s_splash, GUI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_splash, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_splash, 0, 0);
    lv_obj_set_style_radius(s_splash, 0, 0);
    lv_obj_set_style_pad_all(s_splash, 0, 0);
    lv_obj_set_scrollable(s_splash, false);
    lv_obj_set_clickable(s_splash, true); /* swallow input: the splash is not skippable */

    s_dsc = (lv_image_dsc_t){
        .header =
            {
                .magic = LV_IMAGE_HEADER_MAGIC,
                .cf = LV_COLOR_FORMAT_A8,
                .flags = 0,
                .w = (uint32_t)width,
                .h = (uint32_t)height,
                .stride = (uint32_t)width,
                .reserved_2 = 0,
            },
        .data_size = (uint32_t)((size_t)width * height),
        .data = s_buf,
        .reserved = NULL,
    };

    s_image = lv_image_create(s_splash);
    lv_image_set_src(s_image, &s_dsc);
    lv_obj_set_pos(s_image, 0, 0);
    lv_obj_set_style_image_recolor(s_image, GUI_COLOR_TEXT, 0);
    lv_obj_set_style_image_recolor_opa(s_image, LV_OPA_COVER, 0);

    s_drawn = 0.0f;
    s_poly = 0;
    s_pt = 0;
    s_frac = 0.0f;
    s_step = (float)boot_path_total_q / BOOT_ANIM_Q / ((float)BOOT_ANIM_WRITE_MS / BOOT_ANIM_FRAME_MS);

    s_timer = lv_timer_create(boot_anim_timer_cb, BOOT_ANIM_FRAME_MS, NULL);
    if (s_timer == NULL) {
        lv_obj_delete(s_splash);
        s_splash = NULL;
        heap_caps_free(s_buf);
        s_buf = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "boot anim: path %.0f px, step %.1f px/frame, %d ms frames",
             (double)((float)boot_path_total_q / BOOT_ANIM_Q), (double)s_step, BOOT_ANIM_FRAME_MS);
    return ESP_OK;
}
