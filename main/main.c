/*
 * Core2v1.1_tokenpad - application entry point.
 *
 * main only orchestrates startup; every functional concern lives in a component
 * and talks to the others through the bus:
 *
 *   bsp           board hardware (display, backlight, RTC)
 *   bus           typed publish/subscribe between components
 *   time_service  wall clock (RTC + SNTP), publishes time on the bus
 *   net           Wi-Fi connectivity, publishes network state on the bus
 *   gui           LVGL stack + screens, subscribes to the bus
 *   app_console   service terminal over USB serial
 */
#include "app_console.h"
#include "bsp.h"
#include "bus.h"
#include "button_service.h"
#include "esp_err.h"
#include "esp_log.h"
#include "gui.h"
#include "net.h"
#include "power_service.h"
#include "time_service.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_ERROR_CHECK(bsp_init());
    ESP_ERROR_CHECK(bus_init());

    /* Subscribe before producers start, so no early event is missed. */
    ESP_ERROR_CHECK(time_service_start());

    /* No-op when CONFIG_APP_WIFI_SSID is empty: the clock then runs on RTC only. */
    (void)net_start();

    ESP_ERROR_CHECK(power_service_start());
    ESP_ERROR_CHECK(button_service_start());
    ESP_ERROR_CHECK(gui_start());
    ESP_ERROR_CHECK(app_console_start());

    ESP_LOGI(TAG, "boot done");
}
