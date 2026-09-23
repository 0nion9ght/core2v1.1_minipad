#include "time_service.h"

#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "bsp.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "time_service";

#define TIME_TASK_STACK     3072
#define TIME_TASK_PRIO      4
#define TIME_POLL_PERIOD_MS 1000

/* Anything before 2020 means the RTC battery/clock was never set. */
#define TIME_MIN_VALID_YEAR 2020

#define TIME_TZ "CST-8" /* fixed UTC+8 per project decision */

static bus_time_t s_time;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_sntp_started;

static bool datetime_is_valid(const bsp_datetime_t *dt)
{
    return dt->year >= TIME_MIN_VALID_YEAR && dt->year <= 2099 && dt->month >= 1 &&
           dt->month <= 12 && dt->day >= 1 && dt->day <= 31 && dt->hour >= 0 &&
           dt->hour <= 23 && dt->minute >= 0 && dt->minute <= 59;
}

static void publish_time(const bus_time_t *t)
{
    bus_payload_t payload = {.time = *t};
    (void)bus_publish(t->valid ? BUS_TOPIC_TIME_UPDATED : BUS_TOPIC_TIME_INVALID, &payload);
}

/* net -> time_service: bring SNTP up on first connectivity. */
static void on_net_state(bus_topic_t topic, const bus_payload_t *payload, void *arg)
{
    (void)topic;
    (void)arg;

    if (payload->net.state != BUS_NET_CONNECTED || s_sntp_started) {
        return;
    }

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    s_sntp_started = true;
    ESP_LOGI(TAG, "SNTP started after network connect");
}

/* Apply a completed SNTP sync: convert to local time, store it in the RTC. */
static bool apply_sntp_sync(void)
{
    if (!s_sntp_started || esp_sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED) {
        return false;
    }

    time_t now = 0;
    time(&now);
    struct tm tm_local = {0};
    localtime_r(&now, &tm_local);

    bsp_datetime_t dt = {
        .year = (int16_t)(tm_local.tm_year + 1900),
        .month = (int8_t)(tm_local.tm_mon + 1),
        .day = (int8_t)tm_local.tm_mday,
        .hour = (int8_t)tm_local.tm_hour,
        .minute = (int8_t)tm_local.tm_min,
        .second = (int8_t)tm_local.tm_sec,
    };

    if (bsp_rtc_set_datetime(&dt) == ESP_OK) {
        ESP_LOGI(TAG, "SNTP synced %04d-%02d-%02d %02d:%02d:%02d (local %s)", dt.year, dt.month,
                 dt.day, dt.hour, dt.minute, dt.second, TIME_TZ);
    } else {
        ESP_LOGW(TAG, "SNTP synced but RTC write failed; showing system time only");
    }

    esp_sntp_stop();
    s_sntp_started = false;
    return true;
}

static void time_task(void *arg)
{
    (void)arg;

    bus_time_t last_published = {.valid = false, .hour = 0, .minute = 0, };
    bool have_published = false;

    while (true) {
        (void)apply_sntp_sync();

        bsp_datetime_t dt = {0};
        bus_time_t current = {.valid = false, .hour = 0, .minute = 0};

        if (bsp_rtc_get_datetime(&dt) == ESP_OK && datetime_is_valid(&dt)) {
            current.valid = true;
            current.hour = (uint8_t)dt.hour;
            current.minute = (uint8_t)dt.minute;
        } else if (bsp_rtc_get_datetime(&dt) != ESP_OK) {
            ESP_LOGW(TAG, "RTC read failed");
        }

        portENTER_CRITICAL(&s_lock);
        s_time = current;
        portEXIT_CRITICAL(&s_lock);

        /* Publish on minute change, or on any validity transition. */
        const bool changed = !have_published || last_published.valid != current.valid ||
                             (current.valid && (last_published.hour != current.hour ||
                                                last_published.minute != current.minute));
        if (changed) {
            publish_time(&current);
            last_published = current;
            have_published = true;
            if (current.valid) {
                ESP_LOGI(TAG, "time page data: %02u:%02u", current.hour, current.minute);
            } else {
                ESP_LOGW(TAG, "RTC time not set (year < %d) - showing --:--", TIME_MIN_VALID_YEAR);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(TIME_POLL_PERIOD_MS));
    }
}

esp_err_t time_service_start(void)
{
    setenv("TZ", TIME_TZ, 1);
    tzset();

    esp_err_t err = bus_subscribe(BUS_TOPIC_NET_STATE, on_net_state, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bus_subscribe: %s", esp_err_to_name(err));
        return err;
    }

    if (xTaskCreate(time_task, "time_svc", TIME_TASK_STACK, NULL, TIME_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create time task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "time service started (tz=%s)", TIME_TZ);
    return ESP_OK;
}

esp_err_t time_service_get_time(bus_time_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    *out = s_time;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}
