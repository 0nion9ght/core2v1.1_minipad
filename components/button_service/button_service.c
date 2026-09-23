#include "button_service.h"

#include "bsp.h"
#include "bus.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "button_service";

#define BUTTON_POLL_MS 20
#define BUTTON_LONG_MS 700
#define BUTTON_VIB_MS  60

#define BUTTON_TASK_STACK 3072
#define BUTTON_TASK_PRIO  4

static const char *const s_names[BSP_BUTTON_COUNT] = {"left", "center", "right"};
static const bus_button_id_t s_ids[BSP_BUTTON_COUNT] = {
    BUS_BUTTON_LEFT,
    BUS_BUTTON_CENTER,
    BUS_BUTTON_RIGHT,
};

static bool s_long_sent[BSP_BUTTON_COUNT];

static void publish_event(bus_button_id_t id, bus_button_action_t action)
{
    const bus_payload_t payload = {.button = {.id = id, .action = action}};
    (void)bus_publish(BUS_TOPIC_BUTTON, &payload);
}

static void button_task(void *arg)
{
    (void)arg;

    while (true) {
        /* Also services the touch controller; bsp serialises it internally. */
        bsp_poll();

        for (int i = 0; i < BSP_BUTTON_COUNT; i++) {
            const bsp_button_t key = (bsp_button_t)i;

            if (bsp_button_was_pressed(key)) {
                publish_event(s_ids[i], BUS_BUTTON_PRESS);
                ESP_LOGI(TAG, "%s press", s_names[i]);
                bsp_vibrate_pulse(BUTTON_VIB_MS);
                s_long_sent[i] = false;
            } else if (bsp_button_is_pressed(key) && !s_long_sent[i] &&
                       bsp_button_pressed_for(key, BUTTON_LONG_MS)) {
                s_long_sent[i] = true;
                publish_event(s_ids[i], BUS_BUTTON_LONG);
                ESP_LOGI(TAG, "%s long", s_names[i]);
            }

            if (bsp_button_was_released(key)) {
                publish_event(s_ids[i], BUS_BUTTON_RELEASE);
                ESP_LOGI(TAG, "%s release", s_names[i]);
                s_long_sent[i] = false;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

esp_err_t button_service_start(void)
{
    if (xTaskCreate(button_task, "buttons", BUTTON_TASK_STACK, NULL, BUTTON_TASK_PRIO, NULL) !=
        pdPASS) {
        ESP_LOGE(TAG, "failed to create button task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "started (poll %d ms, long %d ms, buzz %d ms)", BUTTON_POLL_MS, BUTTON_LONG_MS,
             BUTTON_VIB_MS);
    return ESP_OK;
}
