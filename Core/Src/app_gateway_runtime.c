#include "app_gateway_runtime.h"

#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "debug_uart.h"

#include "mqtt_handler.h"
#include "comm_manager.h"
#include "app_modbus_payload.h"
#include "app_log.h"
#include "app_flash_log.h"
#include "app_health.h"
#include "app_debug_led.h"
#include "app_watchdog.h"
#include "app_sd_card.h"

#include <string.h>

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

static uint8_t process_downlink_once(const char *tag)
{
    char *rx = MQTT_Subscriber_Run();

    if (rx && rx[0])
    {
        print("[%s] %s\r\n", tag, rx);
        handle_downlink_cjson(rx);
        app_debug_led_event(DEBUG_LED_EVENT_DOWNLINK);
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

        osDelay(DOWNLINK_POLL_STEP_MS);
    }

    return 0U;
}

static telemetry_result_t publish_telemetry_once(void)
{
    if (!device_can_publish_cloud())
    {
        print("[PROVISION] telemetry skipped: waiting for valid config/apiKey\r\n");
        return TELEMETRY_RESULT_SKIPPED;
    }

    const char *payload = app_build_payload_json_once();

    if (payload && payload[0])
    {
        uint8_t ok = mqttPublish(payload);

        if (ok || MQTT_LastPublishAttempted())
        {
            comm_manager_report_publish_result(ok);
        }
        else
        {
            print("[RUNTIME] telemetry publish skipped stage=%s\r\n",
                  MQTT_LastPublishStage());

            if (strcmp(MQTT_LastPublishStage(), "COOLDOWN") == 0)
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

static uint8_t handle_disconnect_urc_once(void)
{
    if (!MQTT_CheckDisconnectURC())
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

    comm_manager_report_mqtt_disconnected();
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

        if (!comm_manager_is_online())
        {
            return;
        }

        if (MQTT_OtaIsBusy())
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

        (void)app_health_signal_poll_due();
        app_memory_monitor_once();
        app_sd_telemetry_service();
        app_sd_terminal_log_service();

        uint8_t keepalive_attempted = 0U;
        uint8_t keepalive_ok = MQTT_Maintain(ONLINE_IDLE_MQTT_KEEPALIVE_MS,
                                             topic_d2c_message,
                                             "{\"type\":\"keepalive\"}",
                                             &keepalive_attempted);

        if (keepalive_attempted)
        {
            comm_manager_report_publish_result(keepalive_ok);

            if (!keepalive_ok)
            {
                print("[RUNTIME] keepalive failed stage=%s\r\n",
                      MQTT_LastPublishStage());
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

    app_watchdog_mark_task(APP_WATCHDOG_TASK_RUNTIME);
    osDelay(5000);
    app_watchdog_mark_task(APP_WATCHDOG_TASK_RUNTIME);
    app_debug_led_run_once();
    app_watchdog_refresh();

    app_log_init();
    app_health_init();
    comm_manager_init();

    for (;;)
    {
        app_watchdog_mark_task(APP_WATCHDOG_TASK_RUNTIME);
        app_debug_led_run_once();
        app_watchdog_refresh();

        comm_manager_run_once();

        if (MQTT_OtaIsBusy())
        {
            osDelay(100U);
            continue;
        }

        app_memory_monitor_once();
        app_sd_telemetry_service();
        app_sd_terminal_log_service();

        if (!comm_manager_is_online())
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

            print("[OFFLINE] flash pending=%u total=%u dropped=%lu deduped=%lu\r\n",
                  app_flash_log_pending_count(),
                  app_flash_log_total_count(),
                  (unsigned long)app_flash_log_dropped_count(),
                  (unsigned long)app_flash_log_deduped_count());

            osDelay(OFFLINE_LOOP_DELAY_MS);
            continue;
        }

        app_debug_led_event(DEBUG_LED_EVENT_ERROR_OFF);

        /*
         * If the bootloader has just installed an OTA image, report that only
         * after MQTT is online. The manifest is cleared only after publish ACK.
         */
        (void)MQTT_ReportOtaBootResultOnce();

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
         * Priority 3: telemetry is main path.
         */
        (void)app_health_signal_poll_due();
        app_memory_monitor_once();
        telemetry_result_t telemetry_result = publish_telemetry_once();

        /*
         * Priority 4: after telemetry, wait briefly for downlink/config.
         */
        if (wait_for_downlink_window(DOWNLINK_AFTER_PUB_WINDOW_MS))
        {
            osDelay(DOWNLINK_HANDLED_DELAY_MS);
            continue;
        }

        /*
         * Priority 5: persist one deferred critical log into W25Q.
         */
        if (device_can_publish_cloud())
        {
            app_log_critical_persist_deferred_once();
        }

        /*
         * Priority 6: upload one flash log if due.
         */
#if ENABLE_FLASH_LOG_UPLOAD
        if (device_can_publish_cloud())
        {
            app_log_critical_flush_cloud_once();
        }
#endif

        /*
         * Priority 7: health is best-effort.
         * Keep disabled if it competes with telemetry.
         */
        // app_health_run_once();

        print("[FLASH_LOG] pending=%u total=%u dropped=%lu deduped=%lu\r\n",
              app_flash_log_pending_count(),
              app_flash_log_total_count(),
              (unsigned long)app_flash_log_dropped_count(),
              (unsigned long)app_flash_log_deduped_count());

        uint32_t next_uplink_ms = app_modbus_payload_get_telemetry_interval_ms();

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
