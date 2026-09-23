#include "app_console.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "bsp.h"
#include "bus.h"
#include "esp_chip_info.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "power_service.h"
#include "time_service.h"

static const char *TAG = "app_console";

static const char *reset_reason_to_str(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_EXT: return "ext";
    case ESP_RST_SW: return "sw";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    default: return "other";
    }
}

static bool parse_log_level(const char *s, esp_log_level_t *out)
{
    if (strcmp(s, "e") == 0 || strcmp(s, "error") == 0) { *out = ESP_LOG_ERROR; return true; }
    if (strcmp(s, "w") == 0 || strcmp(s, "warn") == 0) { *out = ESP_LOG_WARN; return true; }
    if (strcmp(s, "i") == 0 || strcmp(s, "info") == 0) { *out = ESP_LOG_INFO; return true; }
    if (strcmp(s, "d") == 0 || strcmp(s, "debug") == 0) { *out = ESP_LOG_DEBUG; return true; }
    if (strcmp(s, "v") == 0 || strcmp(s, "verbose") == 0) { *out = ESP_LOG_VERBOSE; return true; }
    return false;
}

static int cmd_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    esp_chip_info_t chip = {0};
    esp_chip_info(&chip);

    printf("uptime_ms=%lld\r\n", (long long)(esp_timer_get_time() / 1000));
    printf("reset_reason=%s\r\n", reset_reason_to_str(esp_reset_reason()));
    printf("cores=%d revision=%d\r\n", chip.cores, chip.revision);
    printf("wifi_connected=%s ip=%u.%u.%u.%u\r\n", net_is_connected() ? "yes" : "no",
           (unsigned)(net_ipv4() & 0xFF), (unsigned)((net_ipv4() >> 8) & 0xFF),
           (unsigned)((net_ipv4() >> 16) & 0xFF), (unsigned)((net_ipv4() >> 24) & 0xFF));
    return 0;
}

static int cmd_time(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "set") == 0) {
        if (argc != 4) {
            printf("usage: time set YYYY-MM-DD HH:MM:SS\r\n");
            return 1;
        }

        int y, mo, d, h, mi, s;
        if (sscanf(argv[2], "%d-%d-%d", &y, &mo, &d) != 3 ||
            sscanf(argv[3], "%d:%d:%d", &h, &mi, &s) != 3) {
            printf("bad date/time format, expected YYYY-MM-DD HH:MM:SS\r\n");
            return 1;
        }
        if (y < 2020 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 ||
            mi < 0 || mi > 59 || s < 0 || s > 59) {
            printf("date/time out of range\r\n");
            return 1;
        }

        const bsp_datetime_t dt = {
            .year = (int16_t)y, .month = (int8_t)mo, .day = (int8_t)d,
            .hour = (int8_t)h, .minute = (int8_t)mi, .second = (int8_t)s,
        };
        esp_err_t err = bsp_rtc_set_datetime(&dt);
        if (err != ESP_OK) {
            printf("rtc_write_failed=%s\r\n", esp_err_to_name(err));
            return 1;
        }

        /* Keep the system clock in step so SNTP/log timestamps agree. */
        struct tm tm_local = {
            .tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d,
            .tm_hour = h, .tm_min = mi, .tm_sec = s,
        };
        const time_t epoch = mktime(&tm_local);
        struct timeval tv = {.tv_sec = epoch, .tv_usec = 0};
        (void)settimeofday(&tv, NULL);

        printf("rtc_set=%04d-%02d-%02d %02d:%02d:%02d\r\n", y, mo, d, h, mi, s);
        return 0;
    }

    if (argc != 1) {
        printf("usage: time | time set YYYY-MM-DD HH:MM:SS\r\n");
        return 1;
    }

    bus_time_t cached = {0};
    (void)time_service_get_time(&cached);
    printf("bus_time=%s %02u:%02u\r\n", cached.valid ? "valid" : "invalid", (unsigned)cached.hour,
           (unsigned)cached.minute);

    bsp_datetime_t dt = {0};
    esp_err_t err = bsp_rtc_get_datetime(&dt);
    if (err == ESP_OK) {
        printf("rtc=%04d-%02d-%02d %02d:%02d:%02d\r\n", dt.year, dt.month, dt.day, dt.hour,
               dt.minute, dt.second);
    } else {
        printf("rtc_error=%s\r\n", esp_err_to_name(err));
    }
    return 0;
}

static int cmd_power(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    bus_power_state_t cached = {0};
    (void)power_service_get(&cached);
    printf("bus_power: valid=%d level=%d charging=%d\r\n", cached.valid, (int)cached.level,
           cached.charging);

    bsp_power_t raw = {0};
    esp_err_t err = bsp_power_get(&raw);
    printf("pmu: level=%d%% voltage=%d mV charging=%d valid=%d (%s)\r\n", (int)raw.level,
           (int)raw.millivolt, raw.charging, raw.valid, esp_err_to_name(err));
    return 0;
}

static int cmd_touch(int argc, char **argv)
{
    int seconds = 10;
    if (argc >= 2) {
        seconds = atoi(argv[1]);
    }
    if (seconds < 1 || seconds > 60) {
        printf("usage: touch [seconds 1-60]\r\n");
        return 1;
    }

    printf("watching touch for %d s - press the screen now\r\n", seconds);

    const int64_t deadline = esp_timer_get_time() + ((int64_t)seconds * 1000000);
    bool was_pressed = false;
    int last_x = -1;
    int last_y = -1;
    int samples = 0;

    while (esp_timer_get_time() < deadline) {
        int x = 0;
        int y = 0;
        bool pressed = false;
        if (bsp_touch_read(&x, &y, &pressed) == ESP_OK) {
            if (pressed) {
                if (!was_pressed || x != last_x || y != last_y) {
                    printf("touch x=%3d y=%3d\r\n", x, y);
                    last_x = x;
                    last_y = y;
                    samples++;
                }
            } else if (was_pressed) {
                printf("touch released\r\n");
            }
            was_pressed = pressed;
        }
        vTaskDelay(pdMS_TO_TICKS(40));
    }

    printf("touch_done samples=%d\r\n", samples);
    return 0;
}

static int cmd_heap(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("internal free=%u min=%u largest=%u\r\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    printf("psram    free=%u min=%u largest=%u\r\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    return 0;
}

static int cmd_log(int argc, char **argv)
{
    if (argc != 4 || strcmp(argv[1], "level") != 0) {
        printf("usage: log level <tag|*> <error|warn|info|debug|verbose>\r\n");
        return 1;
    }

    esp_log_level_t level;
    if (!parse_log_level(argv[3], &level)) {
        printf("invalid level '%s'\r\n", argv[3]);
        return 1;
    }

    esp_log_level_set(argv[2], level);
    printf("log_level_set tag=%s level=%s\r\n", argv[2], argv[3]);
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("restarting...\r\n");
    fflush(stdout);
    esp_restart();
    return 0;
}

static esp_err_t register_cmd(const char *name, const char *help, esp_console_cmd_func_t func)
{
    const esp_console_cmd_t cmd = {
        .command = name,
        .help = help,
        .hint = NULL,
        .func = func,
        .argtable = NULL,
    };
    return esp_console_cmd_register(&cmd);
}

esp_err_t app_console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "core2>";
    repl_config.max_cmdline_length = 128;

    esp_console_register_help_command();
    ESP_ERROR_CHECK(register_cmd("status", "Show uptime, reset reason, Wi-Fi state", cmd_status));
    ESP_ERROR_CHECK(register_cmd("time", "Show clock, or set it: time set YYYY-MM-DD HH:MM:SS",
                                 cmd_time));
    ESP_ERROR_CHECK(register_cmd("heap", "Show internal/PSRAM heap summary", cmd_heap));
    ESP_ERROR_CHECK(register_cmd("power", "Show battery level / voltage / charging state",
                                 cmd_power));
    ESP_ERROR_CHECK(register_cmd("touch", "Watch touch points: touch [seconds]", cmd_touch));
    ESP_ERROR_CHECK(register_cmd("log", "Runtime log level: log level <tag|*> <level>", cmd_log));
    ESP_ERROR_CHECK(register_cmd("reboot", "Restart the device", cmd_reboot));

    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    esp_err_t err = esp_console_new_repl_uart(&hw_config, &repl_config, &repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_console_new_repl_uart: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_console_start_repl(repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_console_start_repl: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "service terminal ready (type 'help')");
    return ESP_OK;
}
