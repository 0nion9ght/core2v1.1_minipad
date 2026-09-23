#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * bus - typed publish/subscribe message bus between components.
 *
 * Design rules:
 *  - Components never call each other's business logic directly; they exchange
 *    typed messages through this bus.
 *  - Topics are a compile-time enum (no string matching, no silent typos).
 *  - Payloads are a tagged union; the valid member is implied by the topic.
 *  - Callbacks run in the bus dispatcher task, never in the publisher's context.
 *    A callback must be short and must NOT block (no vTaskDelay/while loops);
 *    publishing from inside a callback is allowed (it only enqueues).
 *  - bus_publish() is task-context only (not ISR-safe). ISR producers must
 *    defer to a task or a queue of their own.
 *  - The bus retains the last payload per topic: a subscriber that joins late
 *    (e.g. the UI starting after a service) immediately receives the current
 *    state instead of waiting for the next change.
 */

typedef enum {
    /* time_service -> gui: wall clock changed to a trustworthy value. */
    BUS_TOPIC_TIME_UPDATED = 0,
    /* time_service -> gui: no trustworthy wall clock (RTC unset / never synced). */
    BUS_TOPIC_TIME_INVALID,
    /* net -> any: Wi-Fi/network connectivity state changed. */
    BUS_TOPIC_NET_STATE,
    /* power_service -> gui: battery level / charging state changed. */
    BUS_TOPIC_POWER_STATE,
    /* button_service -> any: bottom-bezel virtual key event. */
    BUS_TOPIC_BUTTON,
    /* gui -> net: request Wi-Fi on/off (command, not state). */
    BUS_TOPIC_WIFI_SET,

    /* Reserved for upcoming features (touch, API content, images): append new
     * topics here, before BUS_TOPIC_MAX, and never renumber existing ones. */
    BUS_TOPIC_MAX,
} bus_topic_t;

typedef struct {
    uint8_t hour;   /* 0-23 */
    uint8_t minute; /* 0-59 */
    bool valid;
} bus_time_t;

typedef enum {
    BUS_NET_DISCONNECTED = 0, /* not connected (or Wi-Fi not configured) */
    BUS_NET_CONNECTING,       /* station started, waiting for association/IP */
    BUS_NET_CONNECTED,        /* link up and got an IPv4 address */
} bus_net_conn_t;

typedef struct {
    bus_net_conn_t state;
    bool enabled;  /* user intent: false while Wi-Fi is switched off */
    uint32_t ipv4; /* host byte order, 0 when not connected */
} bus_net_state_t;

/* Command topic: ask net to switch the station on/off. */
typedef struct {
    bool enabled;
} bus_wifi_set_t;

typedef struct {
    int8_t level;     /* battery percentage 0-100; <0 when unknown */
    bool charging;    /* true while charging */
    bool valid;       /* false when the PMU reading is unusable */
    int16_t millivolt;/* battery voltage in mV; <0 when unknown */
} bus_power_state_t;

/* The three capacitive keys printed on the bezel below the LCD (M5Unified maps
 * them to BtnA/BtnB/BtnC). RIGHT doubles as the Android-like "back". */
typedef enum {
    BUS_BUTTON_LEFT = 0,
    BUS_BUTTON_CENTER,
    BUS_BUTTON_RIGHT,
    BUS_BUTTON_COUNT,
} bus_button_id_t;

typedef enum {
    BUS_BUTTON_PRESS = 0,
    BUS_BUTTON_RELEASE,
    BUS_BUTTON_LONG,
} bus_button_action_t;

typedef struct {
    bus_button_id_t id;
    bus_button_action_t action;
} bus_button_t;

typedef union {
    bus_time_t time;
    bus_net_state_t net;
    bus_power_state_t power;
    bus_button_t button;
    bus_wifi_set_t wifi_set;
} bus_payload_t;

typedef void (*bus_event_cb_t)(bus_topic_t topic, const bus_payload_t *payload, void *arg);

/* Start the dispatcher. Safe to call once; repeated calls return ESP_ERR_INVALID_STATE. */
esp_err_t bus_init(void);

/* Enqueue a message for asynchronous delivery to all topic subscribers.
 * payload may be NULL (delivered as a zeroed payload). */
esp_err_t bus_publish(bus_topic_t topic, const bus_payload_t *payload);

/* Register/unregister a subscriber. Both are task-context and idempotent-safe. */
esp_err_t bus_subscribe(bus_topic_t topic, bus_event_cb_t cb, void *arg);
esp_err_t bus_unsubscribe(bus_event_cb_t cb, void *arg);

#ifdef __cplusplus
}
#endif
