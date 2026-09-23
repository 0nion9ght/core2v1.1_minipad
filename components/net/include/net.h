#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * net - Wi-Fi connectivity service.
 *
 * Owns Wi-Fi + netif/NVS setup and reports state on the bus as BUS_TOPIC_NET_STATE.
 * Other components must not touch Wi-Fi APIs directly; they subscribe to the bus.
 *
 * net_start() is a no-op (with a warning) when no SSID is configured, so the
 * firmware still boots and shows the clock page without networking.
 */
esp_err_t net_start(void);

bool net_is_connected(void);

/* Current IPv4 address in host byte order, 0 when not connected. */
uint32_t net_ipv4(void);

#ifdef __cplusplus
}
#endif
