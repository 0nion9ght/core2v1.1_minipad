#include "net.h"

#include <string.h>

#include "bus.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "net";

#define NET_RETRY_DELAY_MS  5000
#define NET_RETRY_LOG_EVERY 6 /* log every Nth silent retry (~30 s) */

static bool s_connected;
static uint32_t s_ipv4;
static esp_timer_handle_t s_retry_timer;
static uint32_t s_retry_count;
static bool s_started;
static bool s_user_disabled; /* user asked for Wi-Fi off: no retries, no state churn */

static void net_publish_state(bus_net_conn_t state)
{
    bus_payload_t payload = {.net = {.state = state, .enabled = !s_user_disabled, .ipv4 = s_ipv4}};
    (void)bus_publish(BUS_TOPIC_NET_STATE, &payload);
}

/* gui -> net: switch the station on/off (runs on the bus dispatcher task). */
static void on_wifi_set(bus_topic_t topic, const bus_payload_t *payload, void *arg)
{
    (void)topic;
    (void)arg;

    if (!s_started) {
        ESP_LOGW(TAG, "Wi-Fi switch ignored: no SSID configured");
        return;
    }

    if (payload->wifi_set.enabled) {
        if (s_user_disabled) {
            s_user_disabled = false;
            ESP_LOGI(TAG, "Wi-Fi switched ON by user");
            net_publish_state(BUS_NET_CONNECTING);
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_start());
        }
    } else if (!s_user_disabled) {
        s_user_disabled = true;
        s_connected = false;
        s_ipv4 = 0;
        ESP_LOGI(TAG, "Wi-Fi switched OFF by user");
        (void)esp_timer_stop(s_retry_timer);
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_stop());
        net_publish_state(BUS_NET_DISCONNECTED);
    }
}

static void net_retry_cb(void *arg)
{
    (void)arg;
    if (s_user_disabled) {
        return;
    }
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
    }
}

static void net_schedule_retry(void)
{
    if (s_user_disabled) {
        return;
    }
    s_retry_count++;
    if ((s_retry_count % NET_RETRY_LOG_EVERY) == 0) {
        ESP_LOGW(TAG, "Wi-Fi still disconnected (retries=%u)", (unsigned)s_retry_count);
    }
    if (s_retry_timer != NULL) {
        (void)esp_timer_start_once(s_retry_timer, NET_RETRY_DELAY_MS * 1000);
    }
}

static void net_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        net_publish_state(BUS_NET_CONNECTING);
        (void)esp_wifi_connect();
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        s_ipv4 = 0;
        net_publish_state(BUS_NET_DISCONNECTED);
        net_schedule_retry();
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *evt = (const ip_event_got_ip_t *)data;
        s_ipv4 = evt->ip_info.ip.addr; /* already host byte order */
        s_connected = true;
        s_retry_count = 0;
        net_publish_state(BUS_NET_CONNECTED);
        ESP_LOGI(TAG, "connected, ip=" IPSTR, IP2STR(&evt->ip_info.ip));
        return;
    }
}

esp_err_t net_start(void)
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Subscribe before anything else so the GUI's on/off button is always heard. */
    esp_err_t err = bus_subscribe(BUS_TOPIC_WIFI_SET, on_wifi_set, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bus_subscribe(WIFI_SET): %s", esp_err_to_name(err));
        return err;
    }

    if (strlen(CONFIG_APP_WIFI_SSID) == 0) {
        ESP_LOGW(TAG, "no Wi-Fi SSID configured (CONFIG_APP_WIFI_SSID empty) - "
                      "running offline, clock will rely on the RTC only");
        return ESP_ERR_INVALID_STATE;
    }

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(err));
        return err;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        net_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        net_event_handler, NULL, NULL));

    wifi_config_t wifi_cfg = {0};
    strlcpy((char *)wifi_cfg.sta.ssid, CONFIG_APP_WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, CONFIG_APP_WIFI_PASSWORD,
            sizeof(wifi_cfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));

    const esp_timer_create_args_t retry_args = {
        .callback = net_retry_cb,
        .name = "wifi_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&retry_args, &s_retry_timer));

    ESP_ERROR_CHECK(esp_wifi_start());
    s_started = true;
    ESP_LOGI(TAG, "Wi-Fi station started (ssid=%s)", CONFIG_APP_WIFI_SSID);
    return ESP_OK;
}

bool net_is_connected(void)
{
    return s_connected;
}

uint32_t net_ipv4(void)
{
    return s_ipv4;
}
