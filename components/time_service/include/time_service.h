#pragma once

#include "bus.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * time_service - owns the wall clock.
 *
 * Sources:
 *   1. BM8563 RTC on the board (authoritative between syncs),
 *   2. SNTP, started when the net service reports connectivity; a successful
 *      sync is written back into the RTC.
 *
 * Publishes on the bus:
 *   BUS_TOPIC_TIME_UPDATED  - a trustworthy HH:MM whenever the displayed minute changes
 *   BUS_TOPIC_TIME_INVALID  - when the RTC holds no plausible date (unset battery)
 *
 * No other component reads the RTC directly.
 */
esp_err_t time_service_start(void);

/* Last known clock state, for diagnostics (e.g. the console). */
esp_err_t time_service_get_time(bus_time_t *out);

#ifdef __cplusplus
}
#endif
