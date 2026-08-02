/* mqtt_handler.c
 * MQTT transport + downlink/config handling.
 *
 * Notes:
 * - Modem RX is handled by uart2_rx_engine using DMA-to-IDLE.
 * - MQTT connect/publish uses SIMCom CMQTT AT commands.
 * - Critical logs are only queued through app_log_critical().
 *   W25Q persistence/upload is handled from the runtime loop.
 */

#include "mqtt_handler.h"
#include "main.h"
#include "app_board_io.h"
#include "debug_uart.h"
#include "modem_handler.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h> // Required for bool

#include "cJSON.h"
#include "sha256.h"
#include "modbus_config_json.h"
#include "uart2_rx_engine.h"

#include "config.h"
#include "mqtt_config.h"
#include "app_log.h"
#include "app_debug_led.h"
#include "app_modbus_payload.h"

/* ===== Externals ===== */
extern uart2_rx_t g_u2rx;
extern char imei[20];
extern char topic_d2c_telemetry[64];
extern char topic_c2d_commands[64];
extern char topic_d2c_message[64];
extern char topic_d2c_logs[64];
extern char topic_d2c_health[64];
extern uint8_t pump_state;

uint32_t lastMQTTSuccessTime = 0;

/*
 * Critical logging policy in this file:
 * - Do NOT write flash directly here. app_log_critical() must only defer to RAM queue.
 * - Actual W25Q persistence must be done by app_log_critical_persist_deferred_once()
 *   from main loop, including offline loop.
 * - Cloud upload must be done later by app_log_critical_flush_cloud_once().
 */
static uint8_t s_consecutive_mqtt_connect_fail = 0;
static uint8_t s_consecutive_mqtt_subscribe_fail = 0;

/* Updated Prototypes */
uint8_t SIM800_SendATCommand2(const char *command, const char *expected, uint32_t timeout, bool flush_rx);
uint8_t SendCommandWithRetry(const char *cmd, const char *expected, uint16_t timeout, uint8_t retries);

#define MQTT_MAX_PAYLOAD (1024U * 4U)

#define MQTT_CONNECT_CMD_OK_TIMEOUT_MS      30000UL
#define MQTT_CONNECT_URC_TIMEOUT_MS         45000UL

#define MQTT_PUBLISH_TOPIC_PROMPT_MS        8000U
#define MQTT_PUBLISH_TOPIC_SEND_MS          8000U
#define MQTT_PUBLISH_PAYLOAD_PROMPT_MS      8000U
#define MQTT_PUBLISH_PAYLOAD_SEND_MS        12000U
#define MQTT_PUBLISH_CMD_OK_TIMEOUT_MS      15000U
#define MQTT_PUBLISH_URC_TIMEOUT_MS         30000UL

static const char *s_last_publish_stage = "NONE";
static uint8_t s_last_publish_attempted = 1U;

static void publish_config_status(const char *config_id,
                                  const char *status,
                                  const char *type)
{
    char message[256];
    const char *safe_id = (config_id && config_id[0]) ? config_id : "unknown";

    if (type && type[0]) {
        snprintf(message, sizeof(message),
                 "{\"status\":\"%s\",\"configId\":\"%s\",\"type\":\"%s\"}",
                 status,
                 safe_id,
                 type);
    } else {
        snprintf(message, sizeof(message),
                 "{\"type\":\"deployment_ack\",\"configId\":\"%s\",\"status\":\"%s\"}",
                 safe_id,
                 status);
    }

    MQTT_Publish(topic_d2c_message, message);
}

static int32_t mqtt_dbm_from_rssi(int32_t rssi)
{
    if (rssi < 0 || rssi > 31) {
        return -999;
    }

    return -113 + (2 * rssi);
}

static void mqtt_signal_diag(char *dst, uint32_t dst_len)
{
    int32_t rssi = 99;
    int32_t ber = 99;

    if (!dst || dst_len == 0U) {
        return;
    }

    if (modem_get_signal_quality(&rssi, &ber)) {
        snprintf(dst,
                 dst_len,
                 "rssi=%ld dbm=%ld ber=%ld",
                 (long)rssi,
                 (long)mqtt_dbm_from_rssi(rssi),
                 (long)ber);
    } else {
        snprintf(dst, dst_len, "rssi=unknown");
    }
}

static void mqtt_log_diag(const char *op, const char *stage)
{
    char sig[40];

    mqtt_signal_diag(sig, sizeof(sig));

    print("[MQTT][DIAG] op=%s stage=%s %s\r\n",
          op ? op : "",
          stage ? stage : "",
          sig);
}

static void mqtt_build_failure_message(char *dst,
                                       uint32_t dst_len,
                                       const char *op,
                                       const char *stage)
{
    char sig[40];

    if (!dst || dst_len == 0U) {
        return;
    }

    mqtt_signal_diag(sig, sizeof(sig));

    snprintf(dst,
             dst_len,
             "%s failed stage=%s %s",
             op ? op : "MQTT",
             stage ? stage : "UNKNOWN",
             sig);
}

/* =========================================================
   Topics
   ========================================================= */
void Prepare_MQTT_Topics(void)
{
    /*
     * Use actual modem IMEI as device ID.
     * Fallback is only for safety if IMEI is not available yet.
     */
    const char *device_id = (imei[0] != '\0') ? imei : "unknown_device";

    snprintf(topic_d2c_telemetry, sizeof(topic_d2c_telemetry),
             "d2c/%s/telemetry", device_id);

    snprintf(topic_c2d_commands, sizeof(topic_c2d_commands),
             "c2d/%s/commands", device_id);

    snprintf(topic_d2c_message, sizeof(topic_d2c_message),
             "d2c/%s/message", device_id);

    snprintf(topic_d2c_logs, sizeof(topic_d2c_logs),
             "d2c/%s/logs", device_id);

    snprintf(topic_d2c_health, sizeof(topic_d2c_health),
             "d2c/%s/health", device_id);

    print("[MQTT] Topics prepared for deviceId=%s\r\n", device_id);
}

/* =========================================================
   FIXED MQTT CONNECT (Old Logic + New Engine)
   ========================================================= */
uint8_t MQTT_Connect(const char *host, int port, const char *user, const char *pass)
{
    char cmd[180];
    const char *fail_stage = "START";

    /* Fallback to config.h if arguments are NULL */
    const char *host_u = (host && host[0]) ? host : MQTT_HOST_URI;
    int port_u         = (port > 0) ? port : MQTT_PORT;
    const char *user_u = (user && user[0]) ? user : MQTT_USERNAME;
    const char *pass_u = (pass && pass[0]) ? pass : MQTT_PASSWORD;

    print("MQTT: Starting Connection (TLS)...\r\n");

    /* 1. Reset Session (flush_rx = true to clear previous noise) */
    /* 1. Reset/Cleanup previous MQTT session */
    print("MQTT: Cleaning previous session...\r\n");

    fail_stage = "CLEANUP_DISC";
    (void)SIM800_SendATCommand2("AT+CMQTTDISC=0,120", "OK", 3000, true);
    osDelay(300);

    fail_stage = "CLEANUP_REL";
    (void)SIM800_SendATCommand2("AT+CMQTTREL=0", "OK", 3000, true);
    osDelay(300);

    fail_stage = "CLEANUP_STOP";
    (void)SIM800_SendATCommand2("AT+CMQTTSTOP", "OK", 5000, true);
    osDelay(1000);

    /* 2. TLS Setup (Using your working "No-Verify" logic) */
    fail_stage = "SSL_VERSION";
    if (!SendCommandWithRetry("AT+CSSLCFG=\"sslversion\",0,4", "OK", 5000, 3)) goto fail;
    fail_stage = "SSL_AUTHMODE";
    if (!SendCommandWithRetry("AT+CSSLCFG=\"authmode\",0,0", "OK", 5000, 3)) goto fail;

    /* SNI Setup - Crucial for cloud brokers like HiveMQ */
    if (!SendCommandWithRetry("AT+CSSLCFG=\"enableSNI\",0,1", "OK", 5000, 3)) {
        print("WARN: enableSNI not accepted; continuing...\r\n");
    }

    /* 3. Start MQTT Service */
    fail_stage = "CMQTTSTART";
    if (!SendCommandWithRetry("AT+CMQTTSTART", "OK", 10000, 2)) goto fail;
    osDelay(1000);

    /* 4. Acquire Client (trailing ,1 for secure) */
    snprintf(cmd, sizeof(cmd), "AT+CMQTTACCQ=0,\"STM32_%s\",1", imei);
    fail_stage = "CMQTTACCQ";
    if (!SendCommandWithRetry(cmd, "OK", 5000, 3)) goto fail;

    /* 5. Bind MQTT client 0 -> TLS context 0 */
    fail_stage = "CMQTTSSLCFG";
    if (!SendCommandWithRetry("AT+CMQTTSSLCFG=0,0", "OK", 6000, 3)) goto fail;

    /* 6. Connect over TLS */
    snprintf(cmd, sizeof(cmd),
             "AT+CMQTTCONNECT=0,\"%s:%d\",%u,1,\"%s\",\"%s\"",
             host_u, port_u, MQTT_KEEPALIVE_SECONDS, user_u, pass_u);

    print("Connecting to Broker: %s:%d\r\n", host_u, port_u);

    /* --- ASYNC FIX ---
     * Step A: Send the command and wait for the modem's immediate "OK".
     * We use flush_rx=true here to ensure the buffer is clean before sending.
     */
    fail_stage = "CONNECT_OK";
    if (!SIM800_SendATCommand2(cmd, "OK", MQTT_CONNECT_CMD_OK_TIMEOUT_MS, true)) {
        print("MQTT CONNECT command did not return OK within timeout\r\n");
        goto fail;
    }

    /* Step B: Wait for the Cloud URC (+CMQTTCONNECT: 0,0).
     * We use u2rx_wait_for because the success message might take several seconds
     * while the TLS handshake completes.
     */
    print("Waiting for Cloud Ack (TLS Handshake)...\r\n");
    fail_stage = "CONNECT_URC";
    if (!u2rx_wait_for(&g_u2rx, "+CMQTTCONNECT: 0,0", MQTT_CONNECT_URC_TIMEOUT_MS)) {
        print("MQTT Connection Failed or Timed Out (No URC seen)\r\n");
        goto fail;
    }

    print("MQTT Connected (TLS, no-verify)\r\n");
    lastMQTTSuccessTime = HAL_GetTick();
    s_consecutive_mqtt_connect_fail = 0;
    return 1;

    fail:
        char failure_msg[96];

        /*
         * MQTT_Connect() can be called repeatedly by comm_manager.
         * Log only at thresholds to avoid filling W25Q every retry.
         */
        s_consecutive_mqtt_connect_fail++;

        mqtt_build_failure_message(failure_msg,
                                   sizeof(failure_msg),
                                   "MQTT connect",
                                   fail_stage);

        print("MQTT Connect FAIL -> stage=%s consecutive=%u detail=%s\r\n",
              fail_stage,
              s_consecutive_mqtt_connect_fail,
              failure_msg);

        app_log_push(APP_LOG_ERROR,
                     "MQTT",
                     "CONNECT_FAIL",
                     failure_msg,
                     s_consecutive_mqtt_connect_fail,
                     0);

        if (s_consecutive_mqtt_connect_fail == 3U) {
            app_log_critical(APP_LOG_ERROR,
                             "MQTT",
                             "CONNECT_FAILED_3X",
                             failure_msg,
                             s_consecutive_mqtt_connect_fail,
                             0,
                             "MQTT_RECONNECT");
        } else if (s_consecutive_mqtt_connect_fail == 5U) {
            app_log_critical(APP_LOG_FATAL,
                             "MQTT",
                             "CONNECT_FAILED_5X",
                             failure_msg,
                             s_consecutive_mqtt_connect_fail,
                             0,
                             "MODEM_SYNC");
        } else if ((s_consecutive_mqtt_connect_fail > 5U) &&
                   ((s_consecutive_mqtt_connect_fail % 10U) == 0U)) {
            app_log_critical(APP_LOG_ERROR,
                             "MQTT",
                             "CONNECT_STILL_FAILING",
                             failure_msg,
                             s_consecutive_mqtt_connect_fail,
                             0,
                             "MODEM_SYNC");
        }

        osDelay(500);
        return 0;
}

/* =========================================================
   Standard Operations (Updated Signatures)
   ========================================================= */

uint8_t MQTT_Subscribe(const char *topic)
{
    const char *topic_u = (topic && topic[0]) ? topic : topic_c2d_commands;
    char cmd[80];

    snprintf(cmd, sizeof(cmd),
             "AT+CMQTTSUBTOPIC=0,%d,1",
             (int)strlen(topic_u));

    if (!SendCommandWithRetry(cmd, ">", 3000, 2)) {
        goto fail;
    }

    osDelay(100);

    if (!SendCommandWithRetry(topic_u, "OK", 3000, 2)) {
        goto fail;
    }

    osDelay(100);

    /*
     * CMQTTSUB gives immediate OK first, then async URC:
     * +CMQTTSUB: 0,0
     */
    if (!SendCommandWithRetry("AT+CMQTTSUB=0", "OK", 5000, 2)) {
        goto fail;
    }

    if (!u2rx_wait_for(&g_u2rx, "+CMQTTSUB: 0,0", 10000)) {
        print("Subscribe FAIL: no +CMQTTSUB success URC\r\n");
        goto fail;
    }

    print("Subscribed OK\r\n");
    s_consecutive_mqtt_subscribe_fail = 0;
    return 1;

fail:
    s_consecutive_mqtt_subscribe_fail++;

    app_log_push(APP_LOG_ERROR,
                 "MQTT",
                 "SUBSCRIBE_FAIL",
                 "MQTT subscribe failed",
                 s_consecutive_mqtt_subscribe_fail,
                 0);

    if (s_consecutive_mqtt_subscribe_fail == 3U) {
        app_log_critical(APP_LOG_ERROR,
                         "MQTT",
                         "SUBSCRIBE_FAILED_3X",
                         "MQTT subscribe failed 3 times",
                         s_consecutive_mqtt_subscribe_fail,
                         0,
                         "MQTT_RECONNECT");
    } else if (s_consecutive_mqtt_subscribe_fail == 5U) {
        app_log_critical(APP_LOG_FATAL,
                         "MQTT",
                         "SUBSCRIBE_FAILED_5X",
                         "MQTT subscribe failed 5 times",
                         s_consecutive_mqtt_subscribe_fail,
                         0,
                         "MODEM_SYNC");
    }

    print("Subscribe FAIL -> returning failure to comm_manager, consecutive=%u\r\n",
          s_consecutive_mqtt_subscribe_fail);
    osDelay(500);
    return 0;
}

#define MQTT_PUBLISH_MIN_GAP_MS          3000UL
#define MQTT_PUBLISH_FAIL_COOLDOWN_MS   15000UL

static uint32_t s_last_mqtt_publish_attempt_tick = 0;
static uint32_t s_mqtt_publish_block_until_tick = 0;

uint8_t MQTT_Publish(const char *topic, const char *payload)
{
    if (!topic || !topic[0] || !payload) return 0;

    const char *fail_stage = "START";
    s_last_publish_attempted = 1U;
    s_last_publish_stage = "START";

    uint32_t now = HAL_GetTick();

    /*
     * If previous publish failed badly, give modem time to recover.
     */
    if ((int32_t)(now - s_mqtt_publish_block_until_tick) < 0) {
        print("MQTT PUB skipped: publish cooldown active\r\n");
        s_last_publish_attempted = 0U;
        s_last_publish_stage = "COOLDOWN";
        return 0;
    }

    /*
     * Avoid back-to-back MQTT publish commands.
     * SIMCom MQTT engine becomes unstable in weak network if hammered.
     */
    uint32_t elapsed = now - s_last_mqtt_publish_attempt_tick;
    if (elapsed < MQTT_PUBLISH_MIN_GAP_MS) {
        osDelay(MQTT_PUBLISH_MIN_GAP_MS - elapsed);
    }

    s_last_mqtt_publish_attempt_tick = HAL_GetTick();

    char cmd[128];

    snprintf(cmd, sizeof(cmd), "AT+CMQTTTOPIC=0,%d", (int)strlen(topic));
    fail_stage = "TOPIC_PROMPT";
    if (!SendCommandWithRetry(cmd, ">", MQTT_PUBLISH_TOPIC_PROMPT_MS, 1)) {
        print("MQTT PUB failed: topic prompt timeout\r\n");
        goto fail;
    }

    osDelay(100);

    fail_stage = "TOPIC_SEND";
    if (!SendCommandWithRetry(topic, "OK", MQTT_PUBLISH_TOPIC_SEND_MS, 1)) {
        print("MQTT PUB failed: topic send failed\r\n");
        goto fail;
    }

    osDelay(100);

    snprintf(cmd, sizeof(cmd), "AT+CMQTTPAYLOAD=0,%d", (int)strlen(payload));
    fail_stage = "PAYLOAD_PROMPT";
    if (!SendCommandWithRetry(cmd, ">", MQTT_PUBLISH_PAYLOAD_PROMPT_MS, 1)) {
        print("MQTT PUB failed: payload prompt timeout\r\n");
        goto fail;
    }

    osDelay(100);

    fail_stage = "PAYLOAD_SEND";
    if (!SendCommandWithRetry(payload, "OK", MQTT_PUBLISH_PAYLOAD_SEND_MS, 1)) {
        print("MQTT PUB failed: payload send failed\r\n");
        goto fail;
    }

    osDelay(100);

    fail_stage = "PUB_CMD_OK";
    if (!SendCommandWithRetry("AT+CMQTTPUB=0,1,60", "OK", MQTT_PUBLISH_CMD_OK_TIMEOUT_MS, 1)) {
        print("MQTT PUB command rejected\r\n");
        goto fail;
    }

    fail_stage = "PUB_URC";
    if (!u2rx_wait_for(&g_u2rx, "+CMQTTPUB: 0,0", MQTT_PUBLISH_URC_TIMEOUT_MS)) {
        print("MQTT PUB failed: no +CMQTTPUB success URC\r\n");
        goto fail;
    }

    print("MQTT PUB confirmed: +CMQTTPUB: 0,0\r\n");

    lastMQTTSuccessTime = HAL_GetTick();
    s_last_publish_stage = "OK";
    return 1;

fail:
    s_last_publish_stage = fail_stage;
    s_mqtt_publish_block_until_tick = HAL_GetTick() + MQTT_PUBLISH_FAIL_COOLDOWN_MS;
    mqtt_log_diag("publish", fail_stage);
    return 0;
}

uint8_t MQTT_LastPublishAttempted(void)
{
    return s_last_publish_attempted;
}

const char *MQTT_LastPublishStage(void)
{
    return s_last_publish_stage ? s_last_publish_stage : "UNKNOWN";
}

uint8_t MQTT_CheckDisconnectURC(void)
{
    if (u2rx_wait_for(&g_u2rx, "+CMQTTCONNLOST", 10)) {
        app_log_critical(APP_LOG_ERROR,
                         "MQTT",
                         "CONN_LOST_URC",
                         "CMQTTCONNLOST received",
                         0,
                         0,
                         "MQTT_RECONNECT");

        print("MQTT disconnect URC detected\r\n");
        return 1;
    }

    if (u2rx_wait_for(&g_u2rx, "+PDP DEACT", 10)) {
        app_log_critical(APP_LOG_ERROR,
                         "MQTT",
                         "PDP_DEACT_URC",
                         "PDP deactivated URC received",
                         0,
                         0,
                         "MODEM_SYNC");

        print("PDP deactivated URC detected\r\n");
        return 1;
    }

    if (u2rx_wait_for(&g_u2rx, "+CGEV: ME PDN DEACT", 10)) {
        app_log_critical(APP_LOG_ERROR,
                         "MQTT",
                         "PDN_DEACT_URC",
                         "CGEV ME PDN DEACT received",
                         0,
                         0,
                         "MODEM_SYNC");

        print("PDN deactivated URC detected\r\n");
        return 1;
    }

    return 0;
}

uint8_t MQTT_Maintain(uint32_t keepalive_ms,
                      const char *topic,
                      const char *ping_payload,
                      uint8_t *attempted)
{
    if (attempted) {
        *attempted = 0U;
    }

    if (keepalive_ms == 0) return 1U;

    uint32_t now = HAL_GetTick();
    if ((now - lastMQTTSuccessTime) > keepalive_ms) {
        const char *t = (topic && topic[0]) ? topic : topic_d2c_telemetry;
        const char *p = (ping_payload && ping_payload[0]) ? ping_payload : "{\"type\":\"ping\"}";
        uint8_t ok = MQTT_Publish(t, p);

        if (attempted) {
            *attempted = MQTT_LastPublishAttempted();
        }

        return ok;
    }

    return 1U;
}

uint8_t mqttPublish(const char *message)
{
    return MQTT_Publish(topic_d2c_telemetry, message);
}

void mqttPublishf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    (void)mqttPublish(buf);
}

/* =========================================================
   Subscriber Logic (UNCHANGED)
   ========================================================= */

static int parse_reported_len(const char *line)
{
    const char *last = strrchr(line, ',');
    if (!last) return -1;
    int n = 0;
    last++;
    while (*last == ' ') last++;
    while (*last >= '0' && *last <= '9') {
        n = (n * 10) + (*last - '0');
        last++;
    }
    return (n > 0) ? n : -1;
}

static int u2_read_exact(uint8_t *dst, int want, uint32_t timeout_ms)
{
    uint32_t t0 = HAL_GetTick();
    int got = 0;
    while (got < want && (HAL_GetTick() - t0) < timeout_ms) {
        size_t avail = u2rx_available(&g_u2rx);
        if (avail == 0) {
            (void)osSemaphoreAcquire(g_u2rx.rxSem, 50);
            continue;
        }
        size_t take = avail;
        if (take > (size_t)(want - got)) take = (size_t)(want - got);
        got += (int)u2rx_read(&g_u2rx, dst + got, take);
    }
    return got;
}

char* MQTT_Subscriber_Run(void)
{
    static char payload[MQTT_MAX_PAYLOAD];

    // 1. Wait for the header (don't clear buffer yet)
    if (!u2rx_wait_for(&g_u2rx, "+CMQTTRXPAYLOAD:", 100)) return NULL;

    // 2. Read ONLY the header line to get the length
    char line[128];
    int n = u2rx_read_line(&g_u2rx, line, sizeof(line), 500);
    if (n <= 0) return NULL;

    // 3. Extract the exact length (e.g., 545 from "+CMQTTRXPAYLOAD: 0,545")
    int reported_len = parse_reported_len(line);
    if (reported_len <= 0) return NULL;

    if (reported_len >= (int)MQTT_MAX_PAYLOAD) {
        print("[MQTT RX][ERR] payload too large: %d max=%u\r\n",
              reported_len,
              (unsigned)MQTT_MAX_PAYLOAD);
        (void)u2rx_wait_for(&g_u2rx, "+CMQTTRXEND", 500);
        return NULL;
    }

    // 4. Read the EXACT number of bytes.
    // Do NOT use read_line here. Newlines in JSON will break it.
    int got = u2_read_exact((uint8_t*)payload, reported_len, 5000);
    payload[got] = 0; // Manual null termination

    // 5. Check for footer
    (void)u2rx_wait_for(&g_u2rx, "+CMQTTRXEND", 500);

    print("[MQTT RX] Total Expected: %d, Actually Read: %d\r\n", reported_len, got);
    return (got > 0) ? payload : NULL;
}

static void publish_actuation_ack(const char *actuation_id,
                                  const char *status)
{
    char ack[192];

    if (!actuation_id || actuation_id[0] == '\0') {
        actuation_id = "unknown";
    }

    if (!status || status[0] == '\0') {
        status = "FAILED";
    }

    snprintf(ack, sizeof(ack),
             "{"
             "\"type\":\"actuation_ack\","
             "\"actuationId\":\"%s\","
             "\"status\":\"%s\""
             "}",
             actuation_id,
             status);

    print("[ACT] ACK publish: %s\r\n", ack);

    MQTT_Publish(topic_d2c_message, ack);
}

static uint8_t handle_actuation_cjson(cJSON *root)
{
    cJSON *actuation_id = cJSON_GetObjectItem(root, "actuationId");
    cJSON *port_key     = cJSON_GetObjectItem(root, "portKey");
    cJSON *action       = cJSON_GetObjectItem(root, "action");

    const char *id_str     = cJSON_IsString(actuation_id) ? actuation_id->valuestring : "unknown";
    const char *port_str   = cJSON_IsString(port_key) ? port_key->valuestring : NULL;
    const char *action_str = cJSON_IsString(action) ? action->valuestring : NULL;

    if (!port_str || !action_str) {
        print("[ACT][ERR] missing portKey/action\r\n");
        publish_actuation_ack(id_str, "FAILED");
        return 1U;
    }

    uint8_t ok = 0U;

    if (strcmp(action_str, "TURN_ON") == 0)
    {
        ok = app_relay_set_by_key(port_str, 1U);
    }
    else if (strcmp(action_str, "TURN_OFF") == 0)
    {
        ok = app_relay_set_by_key(port_str, 0U);
    }
    else if (strcmp(action_str, "TOGGLE") == 0)
    {
        uint8_t current = app_relay_get_by_key(port_str);
        ok = app_relay_set_by_key(port_str, current ? 0U : 1U);
    }
    else
    {
        print("[ACT][ERR] unsupported action=%s\r\n", action_str);
        ok = 0U;
    }

    if (ok)
    {
        publish_actuation_ack(id_str, "ACKED");
    }
    else
    {
        publish_actuation_ack(id_str, "FAILED");
    }

    return 1U;
}

void handle_downlink_cjson(const char *payload_str)
{
    if (!payload_str || !payload_str[0]) return;

    app_log_push(APP_LOG_INFO,
                 "DOWNLINK",
                 "RECEIVED",
                 "downlink payload received",
                 0,
                 0);

    cJSON *root = cJSON_Parse(payload_str);
    if (!root) {
        print("CFG: JSON parse failed\r\n");

        app_log_push(APP_LOG_ERROR,
                     "CONFIG",
                     "INVALID_JSON",
                     "config JSON parse failed",
                     0,
                     0);

        publish_config_status("unknown", "error", "invalid_json_format");
        return;
    }

    cJSON *msg = cJSON_GetObjectItem(root, "type");

    if (cJSON_IsString(msg) && strcmp(msg->valuestring, "actuation") == 0)
    {
        handle_actuation_cjson(root);
        cJSON_Delete(root);
        return;
    }

    if (!cJSON_IsString(msg) || strcmp(msg->valuestring, "config") != 0) {
        app_log_push(APP_LOG_WARN,
                     "DOWNLINK",
                     "UNKNOWN_TYPE",
                     "unsupported downlink type",
                     0,
                     0);

        cJSON_Delete(root);
        return;
    }

    cJSON *cfgId = cJSON_GetObjectItem(root, "configId");
    cJSON *cfg   = cJSON_GetObjectItem(root, "config");

    /*
     * New config has hash inside config.
     * Older config may have hash at root.
     *
     * Support both:
     *   root.hash
     *   root.config.hash
     */
    cJSON *hash = cJSON_GetObjectItem(root, "hash");
    if (!cJSON_IsString(hash) && cJSON_IsObject(cfg)) {
        hash = cJSON_GetObjectItem(cfg, "hash");
    }

    const char *id_str = (cJSON_IsString(cfgId)) ? cfgId->valuestring : "missing";

    if (strcmp(id_str, "missing") == 0) {
        app_log_push(APP_LOG_ERROR,
                     "CONFIG",
                     "MISSING_CONFIG_ID",
                     "configId missing",
                     0,
                     0);

        publish_config_status("missing", "error", "missing_configId");
        cJSON_Delete(root);
        return;
    }

    if (!cJSON_IsObject(cfg)) {
        app_log_push(APP_LOG_ERROR,
                     "CONFIG",
                     "MISSING_CONFIG_OBJECT",
                     "config object missing",
                     0,
                     0);

        publish_config_status(id_str, "error", "missing_config_object");
        cJSON_Delete(root);
        return;
    }

    if (!cJSON_IsString(hash) || !hash->valuestring || !hash->valuestring[0]) {
        app_log_push(APP_LOG_ERROR,
                     "CONFIG",
                     "MISSING_HASH",
                     "config hash missing",
                     0,
                     0);

        publish_config_status(id_str, "error", "missing_hash");
        cJSON_Delete(root);
        return;
    }

    /*
     * For hash calculation:
     * If hash is inside config, do NOT calculate hash over config including hash.
     */
    cJSON *hash_base_cfg = cJSON_Duplicate(cfg, 1);
    if (!hash_base_cfg) {
        app_log_push(APP_LOG_ERROR,
                     "CONFIG",
                     "MEMORY_ERROR",
                     "failed to duplicate config for hash",
                     0,
                     0);

        publish_config_status(id_str, "error", "memory_allocation_error");
        cJSON_Delete(root);
        return;
    }

    cJSON_DeleteItemFromObject(hash_base_cfg, "hash");

    char *hash_base_min = cJSON_PrintUnformatted(hash_base_cfg);
    cJSON_Delete(hash_base_cfg);

    if (!hash_base_min) {
        app_log_push(APP_LOG_ERROR,
                     "CONFIG",
                     "MEMORY_ERROR",
                     "failed to print hash base config",
                     0,
                     0);

        publish_config_status(id_str, "error", "memory_allocation_error");
        cJSON_Delete(root);
        return;
    }

    char hash_calc[65] = {0};
    sha256_hex((const uint8_t *)hash_base_min,
               strlen(hash_base_min),
               hash_calc);

    print("Calculated Hash: %s\r\n", hash_calc);
    print("Provided Hash:   %s\r\n", hash->valuestring);

    /*
     * Hash check is currently disabled in your file.
     * Re-enable this once backend hash generation is stable.
     */
//    if (strcmp(hash->valuestring, hash_calc) != 0) {
//        app_log_push(APP_LOG_ERROR,
//                     "CONFIG",
//                     "HASH_MISMATCH",
//                     "config hash mismatch",
//                     0,
//                     0);
//
//        snprintf(message, sizeof(message),
//                 "{\"status\":\"error\",\"configId\":\"%s\",\"type\":\"hash_mismatch\"}",
//                 id_str);
//        MQTT_Publish(topic_d2c_message, message);
//        cJSON_free(hash_base_min);
//        cJSON_Delete(root);
//        return;
//    }

    char *cfg_min = cJSON_PrintUnformatted(cfg);
    if (!cfg_min) {
        app_log_push(APP_LOG_ERROR,
                     "CONFIG",
                     "MEMORY_ERROR",
                     "failed to print config JSON",
                     0,
                     0);

        publish_config_status(id_str, "error", "memory_allocation_error");
        cJSON_free(hash_base_min);
        cJSON_Delete(root);
        return;
    }

    modbus_cfg_t new_cfg;
    memset(&new_cfg, 0, sizeof(new_cfg));

    if (!modbus_cfg_from_json(cfg_min, &new_cfg)) {
        app_log_push(APP_LOG_ERROR,
                     "CONFIG",
                     "PARSE_FAILED",
                     "modbus config parse failed",
                     0,
                     0);

        publish_config_status(id_str, "error", "parse_to_struct_failed");

        cJSON_free(hash_base_min);
        cJSON_free(cfg_min);
        cJSON_Delete(root);
        return;
    }

    print("[CONFIG] parsed telemetryInterval=%lu s\r\n",
          (unsigned long)new_cfg.telemetry_interval_s);

    if (!modbus_cfg_save(&new_cfg)) {
        app_log_push(APP_LOG_ERROR,
                     "CONFIG",
                     "FLASH_SAVE_FAILED",
                     "modbus config flash save failed",
                     0,
                     0);

        publish_config_status(id_str, "error", "flash_write_error");

        cJSON_free(hash_base_min);
        cJSON_free(cfg_min);
        cJSON_Delete(root);
        return;
    }

    modbus_cfg_t saved_cfg;
    memset(&saved_cfg, 0, sizeof(saved_cfg));

    if (!modbus_cfg_load(&saved_cfg)) {
        app_log_push(APP_LOG_ERROR,
                     "CONFIG",
                     "FLASH_RELOAD_FAILED",
                     "saved config could not be reloaded",
                     0,
                     0);

        publish_config_status(id_str, "error", "flash_reload_error");

        cJSON_free(hash_base_min);
        cJSON_free(cfg_min);
        cJSON_Delete(root);
        return;
    }

    if (strcmp(saved_cfg.config_id, id_str) != 0) {
        app_log_push(APP_LOG_ERROR,
                     "CONFIG",
                     "FLASH_VERIFY_FAILED",
                     "saved configId mismatch after reload",
                     0,
                     0);

        publish_config_status(id_str, "error", "flash_verify_error");

        cJSON_free(hash_base_min);
        cJSON_free(cfg_min);
        cJSON_Delete(root);
        return;
    }

    /*
     * Apply only the copy reloaded from flash. This means APPLIED confirms
     * the device is running the durable config, not just the received payload.
     */
    app_debug_led_event(DEBUG_LED_EVENT_DOWNLINK);
    app_modbus_payload_set_cfg(&saved_cfg);
    debug_uart_set_enabled(modbus_cfg_debug_print_enabled(&saved_cfg));

    if (!app_modbus_payload_has_valid_config() ||
        strcmp(app_modbus_payload_get_config_id(), id_str) != 0) {
        app_log_push(APP_LOG_ERROR,
                     "CONFIG",
                     "APPLY_VERIFY_FAILED",
                     "runtime config verify failed after apply",
                     0,
                     0);

        publish_config_status(id_str, "error", "apply_verify_error");

        cJSON_free(hash_base_min);
        cJSON_free(cfg_min);
        cJSON_Delete(root);
        return;
    }

    print("[CONFIG] applied telemetryInterval=%lu ms\r\n",
          (unsigned long)app_modbus_payload_get_telemetry_interval_ms());

    app_log_push(APP_LOG_INFO,
                 "CONFIG",
                 "APPLIED",
                 "config saved and applied",
                 0,
                 0);

    publish_config_status(id_str, "APPLIED", NULL);

    cJSON_free(hash_base_min);
    cJSON_free(cfg_min);
    cJSON_Delete(root);

    osDelay(2000);
//    print("CFG: Success. Rebooting...\r\n");
//    NVIC_SystemReset();
}
