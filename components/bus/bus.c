#include "bus.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "bus";

#define BUS_MAX_SUBS       16
#define BUS_QUEUE_LEN      16
#define BUS_DISPATCH_STACK 3072
#define BUS_DISPATCH_PRIO  5

typedef struct {
    bus_topic_t topic;
    bus_event_cb_t cb;
    void *arg;
    bool used;
} bus_sub_t;

typedef struct {
    bus_topic_t topic;
    bus_payload_t payload;
} bus_evt_t;

static bus_sub_t s_subs[BUS_MAX_SUBS];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_queue;
static bool s_started;
static uint32_t s_dropped;

/* Last payload per topic, so late subscribers get the current state at once. */
static bus_payload_t s_last[BUS_TOPIC_MAX];
static bool s_has_last[BUS_TOPIC_MAX];

static void bus_dispatch_task(void *arg)
{
    (void)arg;
    bus_evt_t evt;

    while (true) {
        if (xQueueReceive(s_queue, &evt, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* Snapshot matching subscribers under the lock, then invoke callbacks
         * outside of it so a subscriber can (un)subscribe or publish. */
        bus_sub_t local[BUS_MAX_SUBS];
        size_t n = 0;
        portENTER_CRITICAL(&s_lock);
        for (size_t i = 0; i < BUS_MAX_SUBS; i++) {
            if (s_subs[i].used && s_subs[i].topic == evt.topic) {
                local[n++] = s_subs[i];
            }
        }
        portEXIT_CRITICAL(&s_lock);

        for (size_t i = 0; i < n; i++) {
            local[i].cb(evt.topic, &evt.payload, local[i].arg);
        }
    }
}

esp_err_t bus_init(void)
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    s_queue = xQueueCreate(BUS_QUEUE_LEN, sizeof(bus_evt_t));
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "failed to create event queue");
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(bus_dispatch_task, "bus_dispatch", BUS_DISPATCH_STACK, NULL,
                    BUS_DISPATCH_PRIO, NULL) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        ESP_LOGE(TAG, "failed to create dispatcher task");
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    ESP_LOGI(TAG, "bus ready (topics=%d, queue=%d)", (int)BUS_TOPIC_MAX, BUS_QUEUE_LEN);
    return ESP_OK;
}

esp_err_t bus_publish(bus_topic_t topic, const bus_payload_t *payload)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (topic < 0 || topic >= BUS_TOPIC_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    bus_evt_t evt;
    evt.topic = topic;
    memset(&evt.payload, 0, sizeof(evt.payload));
    if (payload != NULL) {
        evt.payload = *payload;
    }

    /* Retain the newest value even if the queue is momentarily full. */
    portENTER_CRITICAL(&s_lock);
    s_last[topic] = evt.payload;
    s_has_last[topic] = true;
    portEXIT_CRITICAL(&s_lock);

    if (xQueueSend(s_queue, &evt, 0) != pdTRUE) {
        /* Queue full: keep the newest state (latest wins) by dropping the oldest. */
        bus_evt_t discarded;
        (void)xQueueReceive(s_queue, &discarded, 0);
        if (xQueueSend(s_queue, &evt, 0) != pdTRUE) {
            s_dropped++;
            ESP_LOGW(TAG, "event dropped (topic=%d, total=%u)", (int)topic, (unsigned)s_dropped);
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

esp_err_t bus_subscribe(bus_topic_t topic, bus_event_cb_t cb, void *arg)
{
    if (cb == NULL || topic < 0 || topic >= BUS_TOPIC_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    bool added = false;
    portENTER_CRITICAL(&s_lock);
    for (size_t i = 0; i < BUS_MAX_SUBS; i++) {
        if (!s_subs[i].used) {
            s_subs[i] = (bus_sub_t){.topic = topic, .cb = cb, .arg = arg, .used = true};
            added = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_lock);

    if (!added) {
        ESP_LOGE(TAG, "subscriber table full (%d)", BUS_MAX_SUBS);
        return ESP_ERR_NO_MEM;
    }

    /* Hand the late subscriber the retained state for this topic, so the UI does
     * not have to wait for the next value change (which may never come, e.g. a
     * battery level that stays at 100%). Existing subscribers may see the same
     * payload again, which is harmless for state topics. */
    portENTER_CRITICAL(&s_lock);
    const bool retained = s_has_last[topic];
    const bus_payload_t last = s_last[topic];
    portEXIT_CRITICAL(&s_lock);

    if (retained) {
        (void)bus_publish(topic, &last);
    }
    return ESP_OK;
}

esp_err_t bus_unsubscribe(bus_event_cb_t cb, void *arg)
{
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_lock);
    for (size_t i = 0; i < BUS_MAX_SUBS; i++) {
        if (s_subs[i].used && s_subs[i].cb == cb && s_subs[i].arg == arg) {
            s_subs[i].used = false;
            portEXIT_CRITICAL(&s_lock);
            return ESP_OK;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    return ESP_ERR_NOT_FOUND;
}
