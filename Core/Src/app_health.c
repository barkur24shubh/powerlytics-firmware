#include "app_health.h"

#include "cmsis_os.h"
#include "stm32f4xx_hal.h"

#include "connectivity.h"
#include "app_log.h"
#include "debug_uart.h"
#include "app_firmware.h"
#include "app_board_io.h"
#include "app_gateway_platform.h"
#include "app_powerlytics_engine.h"
#include "rtc_manager.h"
#include "board_profile.h"

#include <stdio.h>
#include <string.h>

#define HEALTH_PUBLISH_INTERVAL_MS   20000UL
#define SIGNAL_SAMPLE_INTERVAL_MS    60000UL
#define HEALTH_JSON_LEN              1280U

extern char imei[20];
extern char topic_d2c_health[64];

static uint32_t s_last_health_publish_tick = 0;
static uint32_t s_boot_count = 0U;
static char s_boot_id[48] = "unknown";
static char s_reset_reason[40] = "UNKNOWN";
static uint32_t s_last_signal_sample_tick = 0;
static app_signal_snapshot_t s_signal = {
    .valid = 0U,
    .rssi = 99,
    .ber = 99,
    .dbm = -999,
    .ageMs = 0U,
    .quality = "unknown"
};

static int32_t signal_dbm_from_rssi(int32_t rssi)
{
    if (rssi == 99) {
        return -999;
    }

    if (rssi < 0 || rssi > 31) {
        return -999;
    }

    return -113 + (2 * rssi);
}

static const char *signal_quality_from_rssi(int32_t rssi)
{
    if (rssi == 99 || rssi < 0) {
        return "unknown";
    }

    if (rssi <= 9) {
        return "poor";
    }

    if (rssi <= 14) {
        return "weak";
    }

    if (rssi <= 19) {
        return "fair";
    }

    if (rssi <= 25) {
        return "good";
    }

    return "excellent";
}

void app_health_set_boot_info(uint32_t boot_count, const char *boot_id, const char *reset_reason)
{
    s_boot_count = boot_count;
    snprintf(s_boot_id, sizeof(s_boot_id), "%s", (boot_id && boot_id[0]) ? boot_id : "unknown");
    snprintf(s_reset_reason, sizeof(s_reset_reason), "%s", (reset_reason && reset_reason[0]) ? reset_reason : "UNKNOWN");
}

void app_health_init(void)
{
    s_last_health_publish_tick = 0;
    s_last_signal_sample_tick = 0;
    s_signal.valid = 0U;
    s_signal.rssi = 99;
    s_signal.ber = 99;
    s_signal.dbm = -999;
    s_signal.ageMs = 0U;
    s_signal.quality = "unknown";

    print("[HEALTH] init ok\r\n");

//    app_log_push(APP_LOG_INFO,
//                 "HEALTH",
//                 "INIT",
//                 "health telemetry initialized",
//                 0,
//                 0);
}

uint8_t app_health_signal_poll_due(void)
{
    uint32_t now = HAL_GetTick();

    if (s_last_signal_sample_tick != 0U &&
        (now - s_last_signal_sample_tick) < SIGNAL_SAMPLE_INTERVAL_MS)
    {
        return 0U;
    }

    s_last_signal_sample_tick = now;

    int32_t rssi = 99;
    int32_t ber = 99;

    if (!connectivity_poll_signal_quality(&rssi, &ber)) {
        return 0U;
    }

    s_signal.valid = (rssi != 99) ? 1U : 0U;
    s_signal.rssi = rssi;
    s_signal.ber = ber;
    s_signal.dbm = signal_dbm_from_rssi(rssi);
    s_signal.ageMs = 0U;
    s_signal.quality = signal_quality_from_rssi(rssi);

    print("[HEALTH] signal quality=%s rssi=%ld dbm=%ld\r\n",
          s_signal.quality,
          (long)s_signal.rssi,
          (long)s_signal.dbm);

    return s_signal.valid;
}

uint8_t app_health_get_signal_snapshot(app_signal_snapshot_t *out)
{
    if (!out) {
        return 0U;
    }

    *out = s_signal;

    if (s_last_signal_sample_tick == 0U || !s_signal.valid) {
        out->valid = 0U;
        out->ageMs = 0U;
        out->quality = "unknown";
        return 0U;
    }

    out->ageMs = HAL_GetTick() - s_last_signal_sample_tick;
    out->quality = signal_quality_from_rssi(out->rssi);

    return 1U;
}

uint8_t app_health_publish_now(void)
{
    char json[HEALTH_JSON_LEN];

    const connectivity_health_t *h = connectivity_get_health();

    uint32_t now = HAL_GetTick();

    uint32_t last_pub_ago_ms = 0;
    if (h->last_successful_publish_tick > 0U) {
        last_pub_ago_ms = now - h->last_successful_publish_tick;
    }

    (void)app_health_signal_poll_due();

    app_signal_snapshot_t sig;
    (void)app_health_get_signal_snapshot(&sig);

    uint16_t power_raw = 0U;
    uint32_t power_sense_mv = 0U;
    uint32_t supply_mv = 0U;
    const uint8_t power_valid = app_power_monitor_get(&power_raw,
                                                       &power_sense_mv,
                                                       &supply_mv);

    snprintf(json, sizeof(json),
             "{"
             "\"type\":\"health\","
             "\"uptimeMs\":%lu,"
             "\"imei\":\"%s\","
             "\"bootId\":\"%s\",\"bootCount\":%lu,\"resetReason\":\"%s\","
             "\"fw\":{\"version\":\"%s\",\"build\":\"%s\",\"model\":\"%s\"},"
             "\"config\":{\"configId\":\"%s\",\"hash\":\"%s\"},"
             "\"commState\":\"%s\","
             "\"lastError\":\"%s\","
             "\"mqttOnline\":%s,"
             "\"lastPublishAgoMs\":%lu,"
             "\"signal\":{"
                "\"valid\":%s,"
                "\"rssi\":%ld,"
                "\"ber\":%ld,"
                "\"dbm\":%ld,"
                "\"quality\":\"%s\","
                "\"ageMs\":%lu"
             "},"
             "\"power\":{"
                "\"valid\":%s,"
                "\"raw\":%u,"
                "\"senseMv\":%lu,"
                "\"supplyMv\":%lu"
             "},"
             "\"counters\":{"
                "\"atFail\":%lu,"
                "\"simNotReady\":%lu,"
                "\"networkRegFail\":%lu,"
                "\"gprsFail\":%lu,"
                "\"mqttConnectFail\":%lu,"
                "\"mqttSubscribeFail\":%lu,"
                "\"mqttPublishFail\":%lu,"
                "\"mqttDisconnect\":%lu,"
                "\"onlineEnter\":%lu"
             "},"
             "\"logQueue\":{"
                "\"queued\":%u,"
                "\"dropped\":%lu"
             "}"
             "}",
             (unsigned long)now,
             imei,
             s_boot_id,
             (unsigned long)s_boot_count,
             s_reset_reason,
             APP_FIRMWARE_VERSION,
             APP_FIRMWARE_BUILD,
             APP_DEVICE_MODEL,
             app_powerlytics_engine_get_config_id(),
             app_powerlytics_engine_get_config_hash(),
             connectivity_state_name(connectivity_get_state()),
             connectivity_error_name(connectivity_get_last_error()),
             connectivity_is_online() ? "true" : "false",
             (unsigned long)last_pub_ago_ms,
             sig.valid ? "true" : "false",
             (long)sig.rssi,
             (long)sig.ber,
             (long)sig.dbm,
             sig.quality ? sig.quality : "unknown",
             (unsigned long)sig.ageMs,
             power_valid ? "true" : "false",
             (unsigned int)power_raw,
             (unsigned long)power_sense_mv,
             (unsigned long)supply_mv,
             (unsigned long)h->at_fail_count,
             (unsigned long)h->sim_not_ready_count,
             (unsigned long)h->network_reg_fail_count,
             (unsigned long)h->gprs_fail_count,
             (unsigned long)h->mqtt_connect_fail_count,
             (unsigned long)h->mqtt_subscribe_fail_count,
             (unsigned long)h->mqtt_publish_fail_count,
             (unsigned long)h->mqtt_disconnect_count,
             (unsigned long)h->online_enter_count,
             (unsigned int)app_log_count(),
             (unsigned long)app_log_dropped_count());

    if (connectivity_publish(topic_d2c_health, json)) {
        s_last_health_publish_tick = now;

        print("[HEALTH] published powerValid=%u raw=%u sense=%lu mV supply=%lu mV\r\n",
              power_valid,
              (unsigned int)power_raw,
              (unsigned long)power_sense_mv,
              (unsigned long)supply_mv);
        return 1U;
    }

//    app_log_push(APP_LOG_WARN,
//                 "HEALTH",
//                 "PUBLISH_FAIL",
//                 "health telemetry publish failed",
//                 0,
//                 0);

    print("[HEALTH] publish failed\r\n");
    return 0U;
}

void app_health_run_once(void)
{
    uint32_t now = HAL_GetTick();

    /*
     * Do not publish health immediately after boot/online.
     * Let telemetry stabilize first.
     */
    if (s_last_health_publish_tick == 0U) {
        s_last_health_publish_tick = now;
        return;
    }

    if ((now - s_last_health_publish_tick) >= HEALTH_PUBLISH_INTERVAL_MS) {
        /*
         * Move the timestamp before publish.
         * Even if publish fails, we do not retry every loop.
         */
        s_last_health_publish_tick = now;
        (void)app_health_publish_now();
    }
}
