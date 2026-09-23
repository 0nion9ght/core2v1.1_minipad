#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * app_console - on-device service terminal over the USB serial port.
 *
 * Provides line editing, history and autocomplete via esp_console REPL, plus a
 * small set of high-value commands (status / heap / log / time / reboot) so
 * diagnostics and log levels can be changed without reflashing.
 *
 * Named app_console to avoid clashing with ESP-IDF's own `console` component.
 */
esp_err_t app_console_start(void);

#ifdef __cplusplus
}
#endif
