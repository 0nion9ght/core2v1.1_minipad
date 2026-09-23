#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * boot_anim - animated splash ("Shallwe" being written) shown over everything
 * while the clock page stays hidden behind it.
 *
 * The frames come from tools/gen_boot_anim.js (LVGL A8 masks, tinted white via
 * image recolor). Playback: 45 frames at 40 ms (the write) -> hold -> fade out ->
 * the splash deletes itself. It is opaque and on the top layer, so it also
 * swallows touch/keys while it plays (the splash is intentionally not skippable).
 */
esp_err_t gui_boot_anim_start(void);

#ifdef __cplusplus
}
#endif
