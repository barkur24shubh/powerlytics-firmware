#include "app_gateway_runtime.h"

#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "debug_uart.h"

#include "connectivity.h"
#include "app_modbus_payload.h"
#include "app_log.h"
#include "app_flash_log.h"
#include "app_health.h"
#include "app_debug_led.h"
#include "app_watchdog.h"
#include "app_sd_card.h"
#include "app_firmware.h"
#include "modem_handler.h"
#include "app_gateway_platform.h"

#include <stdio.h>
#include <string.h>

#ifndef BOARD_TELEMETRY_TEST_INTERVAL_MS
#define BOARD_TELEMETRY_TEST_INTERVAL_MS 0U
#endif

extern char topic_d2c_event[64];
extern char topic_d2c_message[64];
extern osThreadId_t defaultTaskHandle;
extern osThreadId_t ModbusTaskHandle;

typedef enum
{
    TELEMETRY_RESULT_SKIPPED = 0,
    TELEMETRY_RESULT_OK,
    TELEMETRY_RESULT_FAILED
} telemetry_result_t;

static uint8_t device_can_publish_cloud(void)
{
    return app_modbus_payload_has_valid_config();
}

#define OFFLINE_DIAG_INTERVAL_MS 5000UL

static uint8_t publish_event_once(void);

extern uint32_t app_boot_count;
extern char app_boot_id[48];
extern char app_reset_reason[40];

static uint8_t publish_hello_once(void)
{
    static uint8_t sent = 0U;
    static char payload[1200];
    if (sent || !connectivity_is_online()) return sent;

    int idx = snprintf(payload, sizeof(payload),
        "{\"type\":\"hello\",\"v\":1,\"model\":\"%s\",\"hw\":\"%s\","
        "\"fw\":{\"version\":\"%s\",\"build\":\"%s\"},\"connectivity\":\"CELLULAR\","
        "\"channels\":{\"AI\":[",
        APP_DEVICE_MODEL, BOARD_HW_REVISION, APP_FIRMWARE_VERSION, APP_FIRMWARE_BUILD);
    if (idx < 0 || idx >= (int)sizeof(payload)) return 0U;
#define APPEND_CHANNELS(prefix, count) do { \
    for (uint8_t ci = 0U; ci < (count); ci++) { \
        idx += snprintf(payload + idx, sizeof(payload) - (size_t)idx, "%s\"%s%u\"", ci ? "," : "", prefix, (unsigned)(ci + 1U)); \
        if (idx < 0 || idx >= (int)sizeof(payload)) return 0U; \
    } \
} while (0)
    APPEND_CHANNELS("AI_", BOARD_AI_COUNT);
    idx += snprintf(payload + idx, sizeof(payload) - (size_t)idx, "],\"DI\":[");
#ifndef BOARD_COUNTER_COUNT
#define BOARD_COUNTER_COUNT 0U
#endif
    APPEND_CHANNELS("DI_", BOARD_DI_COUNT + BOARD_COUNTER_COUNT);
    idx += snprintf(payload + idx, sizeof(payload) - (size_t)idx, "],\"RO\":[");
    APPEND_CHANNELS("RO_", BOARD_RELAY_COUNT);
    idx += snprintf(payload + idx, sizeof(payload) - (size_t)idx, "]");
#ifdef BOARD_MODBUS_UART_INSTANCE
    idx += snprintf(payload + idx, sizeof(payload) - (size_t)idx, ",\"MI\":[\"MI_1\"]");
#endif
    idx += snprintf(payload + idx, sizeof(payload) - (size_t)idx, "},\"bootId\":\"%s\",\"bootCount\":%lu,\"resetReason\":\"%s\",\"uptimeMs\":%lu}",
        app_boot_id, (unsigned long)app_boot_count, app_reset_reason, (unsigned long)HAL_GetTick());
#undef APPEND_CHANNELS
    if (idx < 0 || idx >= (int)sizeof(payload)) return 0U;
    if (connectivity_publish_priority(topic_d2c_message, payload)) {
        sent = 1U;
        print("[HELLO] identity published bootId=%s\r\n", app_boot_id);
    }
    return sent;
}

static void print_telemetry_payload(const char *payload)
{
    const size_t chunk_size = 480U;
    size_t length;
    size_t offset = 0U;

    if (!payload) return;

    length = strlen(payload);

    /* Most payloads fit in debug_uart's 1 KB formatting buffer. Emit those in
     * one USB transfer so the JSON cannot be split or dropped between writes. */
    if (length <= 900U)
    {
        print("[TELEMETRY_JSON] %s\r\n", payload);
        return;
    }

    print("[TELEMETRY_JSON_BEGIN] len=%lu\r\n", (unsigned long)length);
    osDelay(50U);
    while (offset < length)
    {
        size_t remaining = length - offset;
        size_t count = (remaining > chunk_size) ? chunk_size : remaining;
        print("%.*s", (int)count, &payload[offset]);
        offset += count;
        osDelay(50U);
    }
    print("\r\n[TELEMETRY_JSON_END]\r\n");
}

static uint8_t process_downlink_once(const char *tag)
{
    char *rx = connectivity_read_downlink();

    if (rx && rx[0])
    {
        print("[%s] downlink received len=%lu\r\n",
              tag,
              (unsigned long)strlen(rx));
        app_debug_led_event(DEBUG_LED_EVENT_DOWNLINK);
        connectivity_handle_downlink(rx);
        return 1U;
    }

    return 0U;
}

static uint8_t wait_for_downlink_window(uint32_t window_ms)
{
    uint32_t start = HAL_GetTick();

    while ((HAL_GetTick() - start) < window_ms)
    {
        app_watchdog_mark_task(APP_WATCHDOG_TASK_RUNTIME);
        app_debug_led_run_once();
        app_watchdog_refresh();

        if (process_downlink_once("DOWNLINK AFTER PUB"))
        {
            return 1U;
        }

        (void)publish_event_once();

        osDelay(DOWNLINK_POLL_STEP_MS);
    }

    return 0U;
}

static telemetry_result_t publish_telemetry_once(void)
{
    if (connectivity_ota_is_busy())
    {
        print("[RUNTIME] telemetry skipped: OTA active\r\n");
        return TELEMETRY_RESULT_SKIPPED;
    }

    if (!device_can_publish_cloud())
    {
        print("[PROVISION] telemetry skipped: waiting for valid config/apiKey\r\n");
        return TELEMETRY_RESULT_SKIPPED;
    }

    const char *payload = app_build_payload_json_once();

    if (payload && payload[0])
    {
        print_telemetry_payload(payload);
        uint8_t ok = connectivity_publish_telemetry(payload);

        if (ok || connectivity_last_publish_attempted())
        {
            connectivity_report_publish_result(ok);
        }
        else
        {
            print("[RUNTIME] telemetry publish skipped stage=%s\r\n",
                  connectivity_last_publish_stage());

            if (strcmp(connectivity_last_publish_stage(), "COOLDOWN") == 0)
            {
                return TELEMETRY_RESULT_FAILED;
            }

            return TELEMETRY_RESULT_SKIPPED;
        }

        if (ok)
        {
            app_debug_led_event(DEBUG_LED_EVENT_UPLINK);
            return TELEMETRY_RESULT_OK;
        }
        else
        {
            app_debug_led_event(DEBUG_LED_EVENT_ERROR_ON);
            return TELEMETRY_RESULT_FAILED;
        }
    }

    return TELEMETRY_RESULT_SKIPPED;
}

static uint8_t publish_event_once(void)
{
    static char event_payload[1024];

    if (!device_can_publish_cloud())
    {
        return 0U;
    }

    if (!app_modbus_payload_pop_event(event_payload, sizeof(event_payload)))
    {
        return 0U;
    }

    uint8_t ok = connectivity_publish_priority(topic_d2c_event, event_payload);
    connectivity_report_publish_result(ok);

    if (ok)
    {
        print("[EVENT] published len=%lu\r\n", (unsigned long)strlen(event_payload));
        app_debug_led_event(DEBUG_LED_EVENT_UPLINK);
    }
    else
    {
        print("[EVENT][ERR] publish failed stage=%s\r\n", connectivity_last_publish_stage());
        app_debug_led_event(DEBUG_LED_EVENT_ERROR_ON);
    }

    return ok;
}

static uint8_t handle_disconnect_urc_once(void)
{
    if (!connectivity_check_disconnect_urc())
    {
        return 0U;
    }

    app_log_critical(APP_LOG_WARN,
                     "MQTT",
                     "DISCONNECT_URC",
                     "disconnect URC detected while online",
                     0,
                     0,
                     "MQTT_RECONNECT");

    connectivity_report_disconnected();
    return 1U;
}

static UBaseType_t task_stack_words(osThreadId_t handle)
{
    if (!handle)
    {
        return 0U;
    }

    return uxTaskGetStackHighWaterMark((TaskHandle_t)handle);
}

static void app_memory_monitor_once(void)
{
    static uint32_t s_last_check_tick = 0U;
    static uint32_t s_last_low_log_tick = 0U;
    uint32_t now = HAL_GetTick();

    if (s_last_check_tick != 0U &&
        (now - s_last_check_tick) < MEMORY_MONITOR_INTERVAL_MS)
    {
        return;
    }

    s_last_check_tick = now;

    size_t heap_free = xPortGetFreeHeapSize();
    size_t heap_min = xPortGetMinimumEverFreeHeapSize();
    UBaseType_t runtime_stack = task_stack_words(defaultTaskHandle);
    UBaseType_t modbus_stack = task_stack_words(ModbusTaskHandle);

    print("[MEM] heapFree=%lu heapMin=%lu runtimeStack=%lu modbusStack=%lu\r\n",
          (unsigned long)heap_free,
          (unsigned long)heap_min,
          (unsigned long)runtime_stack,
          (unsigned long)modbus_stack);

    if ((heap_min >= MEMORY_LOW_HEAP_WARN_BYTES) &&
        (runtime_stack == 0U || runtime_stack >= MEMORY_LOW_STACK_WARN_WORDS) &&
        (modbus_stack == 0U || modbus_stack >= MEMORY_LOW_STACK_WARN_WORDS))
    {
        return;
    }

    if (s_last_low_log_tick != 0U &&
        (now - s_last_low_log_tick) < MEMORY_LOW_LOG_INTERVAL_MS)
    {
        return;
    }

    s_last_low_log_tick = now;

    if (heap_min < MEMORY_LOW_HEAP_WARN_BYTES)
    {
        app_log_critical(APP_LOG_ERROR,
                         "SYSTEM",
                         "LOW_HEAP",
                         "FreeRTOS heap low",
                         (int32_t)heap_free,
                         (int32_t)heap_min,
                         "MONITOR");
    }

    if (runtime_stack != 0U && runtime_stack < MEMORY_LOW_STACK_WARN_WORDS)
    {
        app_log_critical(APP_LOG_ERROR,
                         "SYSTEM",
                         "LOW_STACK_RUNTIME",
                         "runtime task stack low",
                         (int32_t)runtime_stack,
                         (int32_t)MEMORY_LOW_STACK_WARN_WORDS,
                         "MONITOR");
    }

    if (modbus_stack != 0U && modbus_stack < MEMORY_LOW_STACK_WARN_WORDS)
    {
        app_log_critical(APP_LOG_ERROR,
                         "SYSTEM",
                         "LOW_STACK_MODBUS",
                         "modbus task stack low",
                         (int32_t)modbus_stack,
                         (int32_t)MEMORY_LOW_STACK_WARN_WORDS,
                         "MONITOR");
    }
}

static void service_idle_log_work_once(void)
{
    static uint32_t s_last_persist_tick = 0U;
    static uint32_t s_last_upload_tick = 0U;
    uint32_t now = HAL_GetTick();

    if ((now - s_last_persist_tick) >= ONLINE_IDLE_LOG_PERSIST_MS)
    {
        s_last_persist_tick = now;
        if (device_can_publish_cloud())
        {
            app_log_critical_persist_deferred_once();
        }
    }

#if ENABLE_FLASH_LOG_UPLOAD
    if ((now - s_last_upload_tick) >= ONLINE_IDLE_LOG_UPLOAD_MS)
    {
        s_last_upload_tick = now;
        if (device_can_publish_cloud())
        {
            (void)app_log_critical_flush_cloud_once();
        }
    }
#endif
}

static void app_gateway_online_idle_for(uint32_t wait_ms)
{
    uint32_t start = HAL_GetTick();

    for (;;)
    {
        uint32_t elapsed = HAL_GetTick() - start;
        uint32_t effective_wait_ms = wait_ms;
        uint32_t current_interval_ms = app_modbus_payload_get_telemetry_interval_ms();

        if (current_interval_ms > 0U && current_interval_ms < effective_wait_ms)
        {
            effective_wait_ms = current_interval_ms;
        }

        app_watchdog_mark_task(APP_WATCHDOG_TASK_RUNTIME);
        app_debug_led_run_once();
        app_watchdog_refresh();

        if (elapsed >= effective_wait_ms)
        {
            return;
        }

        if (!connectivity_is_online())
        {
            return;
        }

        if (connectivity_ota_is_busy())
        {
            osDelay(100U);
            continue;
        }

        if (process_downlink_once("DOWNLINK IDLE"))
        {
            continue;
        }

        if (handle_disconnect_urc_once())
        {
            osDelay(DISCONNECT_RETRY_DELAY_MS);
            return;
        }

        if (publish_event_once())
        {
            continue;
        }

        (void)app_health_signal_poll_due();
        app_memory_monitor_once();
        app_sd_telemetry_service();
        app_sd_terminal_log_service();

        uint8_t keepalive_attempted = 0U;
        uint8_t keepalive_ok = connectivity_maintain(ONLINE_IDLE_MQTT_KEEPALIVE_MS,
                                                     topic_d2c_message,
                                                     "{\"type\":\"keepalive\"}",
                                                     &keepalive_attempted);

        if (keepalive_attempted)
        {
            connectivity_report_publish_result(keepalive_ok);

            if (!keepalive_ok)
            {
                print("[RUNTIME] keepalive failed stage=%s\r\n",
                      connectivity_last_publish_stage());
                app_debug_led_event(DEBUG_LED_EVENT_ERROR_ON);
                osDelay(DISCONNECT_RETRY_DELAY_MS);
                return;
            }
        }

        service_idle_log_work_once();

        osDelay(ONLINE_IDLE_SERVICE_STEP_MS);
    }
}

void app_gateway_runtime_task(void *argument)
{
    (void)argument;
    uint32_t last_offline_diag_tick = 0U;

    app_watchdog_mark_task(APP_WATCHDOG_TASK_RUNTIME);
    osDelay(5000);
    app_watchdog_mark_task(APP_WATCHDOG_TASK_RUNTIME);
    app_debug_led_run_once();
    app_watchdog_refresh();

    app_log_init();
    app_health_init();
    connectivity_init();
    print("[CONNECTIVITY] backend=%s\r\n", connectivity_backend_name());

    for (;;)
    {
        app_watchdog_mark_task(APP_WATCHDOG_TASK_RUNTIME);
        app_debug_led_run_once();
        app_watchdog_refresh();

        connectivity_run_once();

        if (connectivity_ota_is_busy())
        {
            osDelay(100U);
            continue;
        }

        app_memory_monitor_once();
        app_sd_telemetry_service();
        app_sd_terminal_log_service();

        if (!connectivity_is_online())
        {
            app_debug_led_event(DEBUG_LED_EVENT_ERROR_ON);

            /*
             * Important:
             * Persist deferred logs even when offline.
             * This is what makes SIM/network/MQTT failures survive reboot.
             */
            if (device_can_publish_cloud())
            {
                app_log_critical_persist_deferred_once();
            }

            uint32_t now = HAL_GetTick();
            if ((last_offline_diag_tick == 0U) ||
                ((now - last_offline_diag_tick) >= OFFLINE_DIAG_INTERVAL_MS))
            {
                last_offline_diag_tick = now;
                print("[OFFLINE] state=%s error=%s flash pending=%u total=%u dropped=%lu deduped=%lu\r\n",
                      connectivity_state_name(connectivity_get_state()),
                      connectivity_error_name(connectivity_get_last_error()),
                      app_flash_log_pending_count(),
                      app_flash_log_total_count(),
                      (unsigned long)app_flash_log_dropped_count(),
                      (unsigned long)app_flash_log_deduped_count());
            }

            osDelay(OFFLINE_LOOP_DELAY_MS);
            continue;
        }

        app_debug_led_event(DEBUG_LED_EVENT_ERROR_OFF);
        last_offline_diag_tick = 0U;

        /*
         * If the bootloader has just installed an OTA image, report that only
         * after MQTT is online. The manifest is cleared only after publish ACK.
         */
        (void)connectivity_report_ota_boot_result_once();
        (void)publish_hello_once();

        /*
         * Priority 1: downlink/config/command always wins.
         */
        if (process_downlink_once("DOWNLINK PRIORITY"))
        {
            osDelay(DOWNLINK_HANDLED_DELAY_MS);
            continue;
        }

        /*
         * Priority 2: detect disconnect before publishing.
         */
        if (handle_disconnect_urc_once())
        {
            osDelay(DISCONNECT_RETRY_DELAY_MS);
            continue;
        }

        /*
         * Priority 3: critical events beat normal telemetry.
         */
        if (publish_event_once())
        {
            if (wait_for_downlink_window(DOWNLINK_AFTER_PUB_WINDOW_MS))
            {
                osDelay(DOWNLINK_HANDLED_DELAY_MS);
                continue;
            }
        }

        /*
         * Priority 4: telemetry is main path.
         */
        (void)app_health_signal_poll_due();
        app_memory_monitor_once();
        telemetry_result_t telemetry_result = publish_telemetry_once();

        /*
         * Priority 5: after telemetry, wait briefly for downlink/config.
         */
        if (wait_for_downlink_window(DOWNLINK_AFTER_PUB_WINDOW_MS))
        {
            osDelay(DOWNLINK_HANDLED_DELAY_MS);
            continue;
        }

        /*
         * Priority 6: persist one deferred critical log into W25Q.
         */
        if (device_can_publish_cloud())
        {
            app_log_critical_persist_deferred_once();
        }

        /*
         * Priority 7: upload one flash log if due.
         */
#if ENABLE_FLASH_LOG_UPLOAD
        if (device_can_publish_cloud())
        {
            app_log_critical_flush_cloud_once();
        }
#endif

        /*
         * Priority 8: health is best-effort.
         * Keep disabled if it competes with telemetry.
         */
        app_health_run_once();

        print("[FLASH_LOG] pending=%u total=%u dropped=%lu deduped=%lu\r\n",
              app_flash_log_pending_count(),
              app_flash_log_total_count(),
              (unsigned long)app_flash_log_dropped_count(),
              (unsigned long)app_flash_log_deduped_count());

        uint32_t next_uplink_ms = app_modbus_payload_get_telemetry_interval_ms();

#if BOARD_TELEMETRY_TEST_INTERVAL_MS > 0U
        next_uplink_ms = BOARD_TELEMETRY_TEST_INTERVAL_MS;
#endif

        if (telemetry_result == TELEMETRY_RESULT_FAILED)
        {
            next_uplink_ms = TELEMETRY_FAIL_RETRY_MS;
            print("[RUNTIME] telemetry failed, retry window=%lu ms\r\n",
                  (unsigned long)next_uplink_ms);
        }
        else
        {
            print("[RUNTIME] next telemetry in %lu ms\r\n",
                  (unsigned long)next_uplink_ms);
        }

        app_gateway_online_idle_for(next_uplink_ms);
    }
}
