#include "app_gateway_runtime.h"
#include "mqtt_handler.h"

#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "debug_uart.h"

#include "connectivity.h"
#include "app_modbus_payload.h"
#include "app_log.h"
#include "app_board_io.h"
#include "app_powerlytics_engine.h"
#include "app_flash_log.h"
#include "app_health.h"
#include "app_debug_led.h"
#include "app_watchdog.h"
#include "app_sd_card.h"
#include "app_firmware.h"
#include "modem_handler.h"
#include "app_gateway_platform.h"
#include "app_identity.h"

extern char imei[20]; /* the modem IMEI (modem_handler.c): a hardware attribute reported in hello, not the unit's identity */

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

static uint32_t s_link_lost_since;
static uint8_t  s_link_lost;
static uint8_t  s_link_loss_safe_applied;

/* A relay output that changed without a cloud command (local logic, manual
 * debug, safe state) is reported at once instead of at the next interval,
 * which is up to an hour at long intervals. Changes made by commands are
 * left to their acks. At most one early report every 2 s. */
static uint32_t s_last_relay_change_report;

/* Events dropped because the queue was full are logged, not silent
 * (at most one log per minute, with the running total). */
static void event_service(void)
{
    static uint32_t reported_drops;
    static uint32_t last_log_tick;
    app_powerlytics_engine_seq_service();
    uint32_t drops = app_powerlytics_engine_events_dropped();
    if (drops != reported_drops && (last_log_tick == 0U || (HAL_GetTick() - last_log_tick) >= 60000U)) {
        app_log_critical(APP_LOG_WARN, "EVENT", "EVENT_QUEUE_FULL", "events dropped: event queue full",
                     (int32_t)(drops - reported_drops), (int32_t)drops, "NONE");
        print("[EVENT][WARN] %lu events dropped (queue full), total %lu\r\n",
              (unsigned long)(drops - reported_drops), (unsigned long)drops);
        reported_drops = drops;
        last_log_tick = HAL_GetTick() | 1U;
    }
}

static uint8_t relay_change_needs_report(void)
{
    uint8_t reported = 0U;
    if (!app_powerlytics_engine_reported_relays(&reported)) return 0U;
    uint8_t now_mask = 0U;
    char key[8];
    for (uint8_t i = 0U; i < app_relay_count() && i < 8U; i++) {
        snprintf(key, sizeof(key), "RO_%u", (unsigned)(i + 1U));
        if (app_relay_get_by_key(key)) now_mask |= (uint8_t)(1U << i);
    }
    if (now_mask == reported) return 0U;
    uint32_t now = HAL_GetTick();
    uint32_t last_cmd = app_relay_last_command_tick();
    if (last_cmd && (now - last_cmd) < 3000U) return 0U;
    if (s_last_relay_change_report && (now - s_last_relay_change_report) < 2000U) return 0U;
    s_last_relay_change_report = now | 1U;
    print("[RELAY] output changed without a command (0x%02X -> 0x%02X), reporting now\r\n",
          (unsigned)reported, (unsigned)now_mask);
    return 1U;
}

static uint8_t publish_hello_once(void)
{
    static uint8_t sent = 0U;
    static char payload[1200];
    if (sent || !connectivity_is_online()) return sent;
    char hardware_ids[80];
    app_identity_hardware_ids_json(hardware_ids, sizeof(hardware_ids), imei);

    int idx = snprintf(payload, sizeof(payload),
        "{\"type\":\"hello\",\"v\":1,\"model\":\"%s\",\"hw\":\"%s\","
        "\"fw\":{\"version\":\"%s\",\"build\":\"%s\"},\"connectivity\":\"CELLULAR\","
        /* What the hardware says about itself: attributes. The unit's identity is its deviceId, in the topic. */
        "\"hardwareIds\":%s,\"channels\":{\"AI\":[",
        APP_DEVICE_MODEL, BOARD_HW_REVISION, APP_FIRMWARE_VERSION, APP_FIRMWARE_BUILD, hardware_ids);
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
    /* Security posture (2026-11-18): broker certificate checked or not,
     * flash readout protection level (0 = SWD can read the image and its
     * broker credentials), brown-out reset level (3 = off). */
    uint32_t optcr = FLASH->OPTCR;
    uint8_t rdp_byte = (uint8_t)((optcr >> 8) & 0xFFU);
    unsigned rdp = (rdp_byte == 0xAAU) ? 0U : ((rdp_byte == 0xCCU) ? 2U : 1U);
    unsigned bor = (unsigned)((optcr >> 2) & 0x3U); /* 0..2 = level 3..1, 3 = off */
    const char *bor_name = (bor == 3U) ? "off" : ((bor == 0U) ? "3" : ((bor == 1U) ? "2" : "1"));
    idx += snprintf(payload + idx, sizeof(payload) - (size_t)idx, "},\"bootId\":\"%s\",\"bootCount\":%lu,\"resetReason\":\"%s\",\"uptimeMs\":%lu,\"presenceMs\":%lu,"
        "\"security\":{\"tls\":\"%s\",\"rdp\":%u,\"bor\":\"%s\"}}",
        app_boot_id, (unsigned long)app_boot_count, app_reset_reason, (unsigned long)HAL_GetTick(),
        (unsigned long)ONLINE_IDLE_MQTT_KEEPALIVE_MS,
        MQTT_TlsVerified() ? "verified" : "unverified", rdp, bor_name);
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

/* Network time -> RTC: on the first online pass, then every 6 h; retry
 * every 60 s until the network has provided time. */
#define CLOCK_SYNC_INTERVAL_MS  (6UL * 60UL * 60UL * 1000UL)
#define CLOCK_SYNC_RETRY_MS     60000UL

static void service_clock_sync_once(void)
{
    static uint8_t s_synced;
    static uint32_t s_next_tick;
    uint32_t now = HAL_GetTick();

    if (s_next_tick != 0U && (int32_t)(now - s_next_tick) < 0) return;
    s_synced = connectivity_sync_clock();
    s_next_tick = now + (s_synced ? CLOCK_SYNC_INTERVAL_MS : CLOCK_SYNC_RETRY_MS);
    if (s_next_tick == 0U) s_next_tick = 1U;
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
        /* Acks of relay commands already run on arrival go out before a
         * slower downlink (a config takes ~2 s to apply) is handled. */
        (void)connectivity_flush_pending_acks();
        connectivity_handle_downlink(rx);
        /* Command acks go out now, ahead of anything else queued. */
        (void)connectivity_flush_pending_acks();
        return 1U;
    }

    return 0U;
}

/* The idle wait between cycles handles downlinks, acks and events too, so
 * the window after a publish only needs to be short. A fixed 3 s window
 * made every cycle at least ~5 s whatever the interval (1.0.15: at most a
 * quarter of the interval, never more than before). */
static uint32_t after_pub_window_ms(void)
{
    uint32_t interval = app_modbus_payload_get_telemetry_interval_ms();
    uint32_t w = interval / 4U;
    if (w > DOWNLINK_AFTER_PUB_WINDOW_MS) w = DOWNLINK_AFTER_PUB_WINDOW_MS;
    if (w < DOWNLINK_POLL_STEP_MS) w = DOWNLINK_POLL_STEP_MS;
    return w;
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

        /* Acks of commands run on arrival during the publish go out now. */
        (void)connectivity_flush_pending_acks();
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

    /* A normal publish keeps a 3 s gap after the previous one and used to
     * sleep it out inside MQTT_Publish, during which nothing read the modem
     * and commands waited. Wait it out here, still serving downlinks. */
    /* Never wait more than one gap: commands still run in the loop below,
     * but a steady stream of their acks must not hold telemetry back. */
    uint32_t gap_wait_start = HAL_GetTick();
    for (;;)
    {
        uint32_t gap_ms = connectivity_publish_gap_remaining_ms();
        if (gap_ms == 0U) break;
        if ((HAL_GetTick() - gap_wait_start) >= 3000U) break;
        app_watchdog_mark_task(APP_WATCHDOG_TASK_RUNTIME);
        app_watchdog_refresh();
        (void)process_downlink_once("DOWNLINK BEFORE TELEMETRY");
        (void)connectivity_flush_pending_acks();
        osDelay((gap_ms < DOWNLINK_POLL_STEP_MS) ? gap_ms : DOWNLINK_POLL_STEP_MS);
    }

    const char *payload = app_build_payload_json_once();

    if (payload && payload[0])
    {
        /* Echoing ~2 KB to USB took 0.26 s a cycle: skip it when fast. */
        if (app_modbus_payload_get_telemetry_interval_ms() >= 5000U) print_telemetry_payload(payload);
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

#if BOARD_TELEMETRY_TEST_INTERVAL_MS > 0U
        current_interval_ms = BOARD_TELEMETRY_TEST_INTERVAL_MS;
#endif

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

        event_service();

        if (relay_change_needs_report())
        {
            return;
        }

        (void)connectivity_flush_pending_acks();
        service_clock_sync_once();

        /*
         * The post-install trial confirmation fires 5 min after boot. Check it
         * here too, not only once per telemetry cycle, or OTA_CONFIRMED waits
         * for the next uplink (up to the cloud interval, e.g. 30 min).
         */
        (void)connectivity_report_ota_boot_result_once();

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
    osDelay(500);
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
        app_identity_service(HAL_GetTick()); /* USB "ID" commands, and the one restart after the unit learns its deviceId */

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

            /* Relays go to their safe state once the connection has been
             * lost for the configured time (0 = hold the current state). */
            {
                uint16_t flags = app_powerlytics_engine_relay_flags();
                uint32_t minutes = (flags >> 8) & 0xFFU;
                if (!s_link_lost) { s_link_lost_since = HAL_GetTick(); s_link_lost = 1U; }
                if (minutes && !s_link_loss_safe_applied &&
                    (HAL_GetTick() - s_link_lost_since) >= minutes * 60000UL) {
                    print("[RELAY] connection lost for %lu ms (limit %lu min), applying safe states\r\n",
                          (unsigned long)(HAL_GetTick() - s_link_lost_since), (unsigned long)minutes);
                    app_relay_apply_safe_states(flags & 0xFFU, "connection lost");
                    app_log_critical(APP_LOG_WARN, "RELAY", "LINK_LOSS_SAFE_STATE", "relays set to safe state after connection loss", (int32_t)minutes, 0, "RELAY_SAFE_STATE");
                    s_link_loss_safe_applied = 1U;
                }
            }

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
        s_link_lost = 0U;
        s_link_loss_safe_applied = 0U;

        /*
         * If the bootloader has just installed an OTA image, report that only
         * after MQTT is online. The manifest is cleared only after publish ACK.
         */
        (void)connectivity_report_ota_boot_result_once();
        (void)publish_hello_once();
        (void)connectivity_flush_pending_acks();
        service_clock_sync_once();
        event_service();

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
            if (wait_for_downlink_window(after_pub_window_ms()))
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
        /* The interval is measured from the start of one telemetry cycle to
         * the next. Before 2026-11-17 the full interval was waited after the
         * publish, logs and health had finished, so a 5 s setting gave a
         * message every 10-14 s and 10 s gave 16-19 s. */
        uint32_t telemetry_cycle_start = HAL_GetTick();
        telemetry_result_t telemetry_result = publish_telemetry_once();

        /*
         * Priority 5: after telemetry, wait briefly for downlink/config.
         */
        if (wait_for_downlink_window(after_pub_window_ms()))
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
            uint32_t spent_ms = HAL_GetTick() - telemetry_cycle_start;
            next_uplink_ms = (spent_ms < next_uplink_ms) ? (next_uplink_ms - spent_ms) : 0U;
            print("[RUNTIME] next telemetry in %lu ms (cycle work %lu ms)\r\n",
                  (unsigned long)next_uplink_ms, (unsigned long)spent_ms);
        }

        app_gateway_online_idle_for(next_uplink_ms);
    }
}
