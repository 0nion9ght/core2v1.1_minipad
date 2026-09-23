#pragma once

#include "bus.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * power_service - battery / charging state, published on the bus.
 *
 * Reads the PMU through bsp every POWER_POLL_PERIOD_MS and publishes
 * BUS_TOPIC_POWER_STATE only when the reading changes (plus once at start).
 * No other component talks to the PMU directly.
 */
esp_err_t power_service_start(void);

/* Last known state, for diagnostics (e.g. the console). */
esp_err_t power_service_get(bus_power_state_t *out);

#ifdef __cplusplus
}
#endif
