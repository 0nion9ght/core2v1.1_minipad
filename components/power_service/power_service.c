#include "power_service.h"

#include <string.h>

#include "bsp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "power_service";

#define POWER_POLL_PERIOD_MS 10000
#define POWER_TASK_STACK     3072
#define POWER_TASK_PRIO      3

static bus_power_state_t s_state;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static bool state_differs(const bus_power_state_t *a, const bus_power_state_t *b)
{
    return a->level != b->level || a->charging != b->charging || a->valid != b->valid;
}

static void power_task(void *arg)
{
    (void)arg;

    bus_power_state_t last = {.level = -2, .charging = false, .valid = false, .millivolt = -1};

    while (true) {
        bsp_power_t raw = {0};
        bus_power_state_t current = {.level = -1, .charging = false, .valid = false, .millivolt = -1};

        if (bsp_power_get(&raw) == ESP_OK) {
            current.level = raw.level;
            current.charging = raw.charging;
            current.valid = raw.valid;
            current.millivolt = raw.millivolt;
        } else {
            ESP_LOGW(TAG, "PMU reading unavailable");
        }

        portENTER_CRITICAL(&s_lock);
        s_state = current;
        portEXIT_CRITICAL(&s_lock);

        if (state_differs(&last, &current)) {
            const bus_payload_t payload = {.power = current};
            (void)bus_publish(BUS_TOPIC_POWER_STATE, &payload);
            last = current;
            if (current.valid) {
                ESP_LOGI(TAG, "battery %d%% (%d mV)%s", (int)current.level, (int)current.millivolt,
                         current.charging ? " charging" : "");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(POWER_POLL_PERIOD_MS));
    }
}

esp_err_t power_service_start(void)
{
    if (xTaskCreate(power_task, "power_svc", POWER_TASK_STACK, NULL, POWER_TASK_PRIO, NULL) !=
        pdPASS) {
        ESP_LOGE(TAG, "failed to create power task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "power service started (period %d ms)", POWER_POLL_PERIOD_MS);
    return ESP_OK;
}

esp_err_t power_service_get(bus_power_state_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    *out = s_state;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}
