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
#include "mqtt_root_ca.h"
#include "app_gateway_platform.h"
#include "main.h"
#include "app_board_io.h"
#include "debug_uart.h"
#include "modem_handler.h"
#include "modem_urc.h"
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
#include "app_firmware.h"
#include "comm_manager.h"
#include "app_ota_manifest.h"
#include "app_watchdog.h"
#include "rtc_manager.h"

/* ===== Externals ===== */
extern uart2_rx_t g_u2rx;
extern char imei[20];
extern char topic_d2c_telemetry[64];
extern char topic_c2d_commands[64];
extern char topic_d2c_message[64];
extern char topic_d2c_logs[64];
extern char topic_d2c_health[64];
extern char topic_d2c_event[64];
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

/* Broker certificate check (2026-11-18). Before this the MQTT TLS context
 * used authmode 0: any server could pose as the broker and collect the
 * broker credentials or send commands. Now the chain is verified against
 * the bundled public roots (DigiCert Global Root G2 for EMQX Cloud, plus
 * DigiCert Global Root CA and ISRG Root X1 in case the broker changes CA).
 * Dates are not checked (ignorelocaltime) so a wrong modem clock cannot
 * keep the unit offline. Availability guard: if no verified connection has
 * succeeded for MQTT_TLS_FALLBACK_MS, or the CA file cannot be installed,
 * every other attempt connects without verification so the fleet can still
 * be reached (for example to ship new roots by OTA); the mode is reported
 * in hello as "tls". */
#define MQTT_CA_FILE            "mqtt_roots_v1.pem"
#define MQTT_TLS_FALLBACK_MS    (24UL * 3600UL * 1000UL)
static uint8_t  s_tls_verified = 0U;        /* mode of the current connection */
static uint8_t  s_tls_fail_streak_started = 0U;
static uint32_t s_tls_fail_since = 0U;      /* first failed verified attempt of the streak */
static uint8_t  s_tls_try_insecure = 0U;    /* alternate attempts once in fallback */

uint8_t MQTT_TlsVerified(void)
{
    return s_tls_verified;
}
static uint8_t s_consecutive_mqtt_subscribe_fail = 0;

/* Updated Prototypes */
uint8_t SIM800_SendATCommand2(const char *command, const char *expected, uint32_t timeout, bool flush_rx);
uint8_t SendCommandWithRetry(const char *cmd, const char *expected, uint16_t timeout, uint8_t retries);

#define MQTT_MAX_PAYLOAD (1024U * 8U)
#define CONFIG_HASH_HEX_LEN                  64U
#define MQTT_RX_POLL_WAIT_MS                 100UL

#define MQTT_CONNECT_CMD_OK_TIMEOUT_MS      30000UL
#define MQTT_CONNECT_URC_TIMEOUT_MS         45000UL

#define MQTT_PUBLISH_TOPIC_PROMPT_MS        8000U
#define MQTT_PUBLISH_TOPIC_SEND_MS          8000U
#define MQTT_PUBLISH_PAYLOAD_PROMPT_MS      8000U
#define MQTT_PUBLISH_PAYLOAD_SEND_MS        12000U
#define MQTT_PUBLISH_CMD_OK_TIMEOUT_MS      15000U
#define MQTT_PUBLISH_URC_TIMEOUT_MS         30000UL

#define OTA_STAGING_BASE_ADDR               APP_OTA_STAGING_W25Q_ADDR
#define OTA_STAGING_MAX_SIZE                APP_OTA_SLOT_SIZE
#define OTA_MAX_ARTIFACT_URL_LEN            2000U
#define OTA_SERIAL_DEBUG_PRINT              1U

#define OTA_LOG(...) do { if (OTA_SERIAL_DEBUG_PRINT) { print(__VA_ARGS__); } } while (0)

static const char *s_last_publish_stage = "NONE";
static uint8_t s_last_publish_attempted = 1U;
static W25Q_Handle *s_ota_flash = NULL;
static volatile uint8_t s_ota_busy = 0U;
static uint8_t s_ota_boot_result_seen_empty = 0U;
static uint8_t s_ota_installed_ack_sent = 0U;
static uint32_t s_ota_trial_started_tick = 0U;
static char s_ota_rollback_version[32] = {0};
#define OTA_TRIAL_CONFIRM_DELAY_MS 300000UL
static char s_mqtt_rx_payload[MQTT_MAX_PAYLOAD] __attribute__((section(".ccmram"), aligned(4)));

static uint8_t config_hash_is_valid_hex(const char *hash)
{
    if (!hash || strlen(hash) != CONFIG_HASH_HEX_LEN) {
        return 0U;
    }

    for (size_t i = 0U; i < CONFIG_HASH_HEX_LEN; i++) {
        const char c = hash[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return 0U;
        }
    }

    return 1U;
}

static uint8_t config_json_span(const char *json,
                                const uint8_t **config_start,
                                size_t *config_len)
{
    if (!json || !config_start || !config_len) return 0U;

    const char *key = json;
    const char *p = NULL;
    do {
        key = strstr(key, "\"config\"");
        if (!key) return 0U;
        p = key + strlen("\"config\"");
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        key++;
    } while (*p != ':');

    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '{') return 0U;

    const char *start = p;
    uint32_t depth = 0U;
    uint8_t in_string = 0U;
    uint8_t escaped = 0U;

    for (; *p; p++) {
        const char c = *p;

        if (in_string) {
            if (escaped) {
                escaped = 0U;
            } else if (c == '\\') {
                escaped = 1U;
            } else if (c == '"') {
                in_string = 0U;
            }
            continue;
        }

        if (c == '"') {
            in_string = 1U;
        } else if (c == '{') {
            depth++;
        } else if (c == '}') {
            if (depth == 0U) return 0U;
            depth--;
            if (depth == 0U) {
                *config_start = (const uint8_t *)start;
                *config_len = (size_t)(p - start + 1);
                return 1U;
            }
        }
    }

    return 0U;
}

typedef struct {
    uint8_t last_percent;
} ota_progress_ctx_t;

static void ota_download_progress(uint32_t downloaded,
                                  uint32_t total,
                                  void *ctx);

void MQTT_SetOtaFlash(W25Q_Handle *flash)
{
    s_ota_flash = flash;
}

uint8_t MQTT_OtaIsBusy(void)
{
    return s_ota_busy;
}

static void mqtt_ota_release_busy(void)
{
    app_debug_led_set_ota_active(0U);
    s_ota_busy = 0U;
}

/* The A7670C shares a limited TLS/session pool between MQTT and HTTP. Keep
 * MQTT as the normal transport, but release its live TLS session while OTA
 * HTTPS is active. CMQTTSTOP is the final confirmation that the MQTT service
 * (and its socket/TLS session) has been torn down. */
static uint8_t mqtt_stop_for_ota_http(void)
{
    /* The immediate OK only means the command was accepted. The modem keeps
     * releasing MQTT's socket/TLS state asynchronously; starting HTTP before
     * these URCs can leave the shared SSL session pool busy (HTTP status 710).
     */
    if (!SIM800_SendATCommand2("AT+CMQTTDISC=0,10", "OK", 5000U, true)) {
        print("[OTA][MQTT] disconnect command was not accepted\r\n");
        return 0U;
    }
    if (!u2rx_wait_for(&g_u2rx, "+CMQTTDISC: 0,0", 15000U)) {
        print("[OTA][MQTT] disconnect completion URC missing or failed\r\n");
        return 0U;
    }

    if (!SIM800_SendATCommand2("AT+CMQTTREL=0", "OK", 5000U, true)) {
        print("[OTA][MQTT] client release failed after disconnect\r\n");
        return 0U;
    }

    if (!SIM800_SendATCommand2("AT+CMQTTSTOP", "OK", 8000U, true)) {
        print("[OTA][MQTT] failed to stop MQTT service before HTTPS\r\n");
        return 0U;
    }
    if (!u2rx_wait_for(&g_u2rx, "+CMQTTSTOP: 0", 15000U)) {
        print("[OTA][MQTT] stop completion URC missing or failed\r\n");
        return 0U;
    }

    print("[OTA][MQTT] service stopped; HTTPS can use modem TLS resources\r\n");
    return 1U;
}

static uint8_t mqtt_resume_after_ota_http(void)
{
    if (!MQTT_Connect(MQTT_HOST_URI,
                      MQTT_PORT,
                      MQTT_USERNAME,
                      MQTT_PASSWORD)) {
        print("[OTA][MQTT] reconnect failed after HTTPS download\r\n");
        comm_manager_report_mqtt_disconnected();
        return 0U;
    }

    if (!MQTT_Subscribe(NULL)) {
        print("[OTA][MQTT] resubscribe failed after HTTPS download\r\n");
        comm_manager_report_mqtt_disconnected();
        return 0U;
    }

    print("[OTA][MQTT] reconnected and downlink subscription restored\r\n");
    return 1U;
}

static void ota_download_progress(uint32_t downloaded,
                                  uint32_t total,
                                  void *ctx)
{
    ota_progress_ctx_t *progress = (ota_progress_ctx_t *)ctx;

    if (total == 0U || !progress) {
        return;
    }

    uint8_t percent = (uint8_t)((downloaded * 100U) / total);
    if (percent >= 100U || percent >= (uint8_t)(progress->last_percent + 10U)) {
        progress->last_percent = percent;
        OTA_LOG("[OTA][DOWNLOAD] progress=%u%% bytes=%lu/%lu\r\n",
                percent,
                (unsigned long)downloaded,
                (unsigned long)total);
    }
}

static uint8_t ota_url_is_supported(const char *url, const char **reason)
{
    size_t len;

    if (reason) {
        *reason = "invalid_artifact_url";
    }

    if (!url || !url[0]) {
        return 0U;
    }

    len = strlen(url);
    if (len > OTA_MAX_ARTIFACT_URL_LEN) {
        if (reason) {
            *reason = "artifact_url_too_long";
        }
        return 0U;
    }

    if (strncmp(url, "https://", 8U) != 0) {
        if (reason) {
            *reason = "https_required";
        }
        return 0U;
    }

    return 1U;
}

/* On a hash mismatch, print a short hash of each 256-byte block of what was
 * hashed, so the changed block can be found by comparing with the sender. */
static void config_hash_block_diag(const uint8_t *data, size_t len)
{
    if (!data) return;
    for (size_t off = 0U, blk = 0U; off < len; off += 256U, blk++) {
        size_t n = (len - off > 256U) ? 256U : (len - off);
        char hex[65];
        sha256_hex(data + off, n, hex);
        hex[8] = 0;
        print("[CONFIG][HASH][BLK] %u %s\r\n", (unsigned)blk, hex);
        osDelay(5);
    }    size_t start = (len > 96U) ? len - 96U : 0U;
    print("[CONFIG][HASH][TAIL] ");
    for (size_t i = start; i < len; i++) {
        uint8_t c = data[i];
        print("%c", (c >= 32U && c < 127U) ? (char)c : '?');
    }
    print("\r\n");
}

static void publish_config_status(const char *config_id,
                                  const char *deployment_id,
                                  const char *status,
                                  const char *type)
{
    char message[256];
    const char *safe_id = (config_id && config_id[0]) ? config_id : "unknown";
    const char *safe_deployment = (deployment_id && deployment_id[0]) ? deployment_id : "unknown";

    if (type && type[0]) {
        snprintf(message, sizeof(message),
                 "{\"status\":\"%s\",\"deploymentId\":\"%s\",\"configId\":\"%s\",\"type\":\"%s\"}",
                 status,
                 safe_deployment,
                 safe_id,
                 type);
    } else {
        snprintf(message, sizeof(message),
                 "{\"type\":\"deployment_ack\",\"deploymentId\":\"%s\",\"configId\":\"%s\",\"status\":\"%s\"}",
                 safe_deployment,
                 safe_id,
                 status);
    }

    MQTT_Publish(topic_d2c_message, message);
}

static const char *json_str_or_empty(cJSON *root, const char *key)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    return (cJSON_IsString(item) && item->valuestring) ? item->valuestring : "";
}

static int json_int_or(cJSON *root, const char *key, int fallback)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static uint32_t ota_crc32_sw(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFUL;

    for (uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (uint8_t b = 0; b < 8U; b++) {
            uint32_t mask = (uint32_t)(-(int32_t)(crc & 1UL));
            crc = (crc >> 1) ^ (0xEDB88320UL & mask);
        }
    }

    return ~crc;
}

static int8_t ota_hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return (int8_t)(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return (int8_t)(c - 'a' + 10);
    }
    if (c >= 'A' && c <= 'F') {
        return (int8_t)(c - 'A' + 10);
    }
    return -1;
}

static uint8_t ota_hex_to_bytes_32(const char *hex, uint8_t out[32])
{
    if (!hex || !out || strlen(hex) != 64U) {
        return 0U;
    }

    for (uint32_t i = 0U; i < 32U; i++) {
        int8_t hi = ota_hex_nibble(hex[i * 2U]);
        int8_t lo = ota_hex_nibble(hex[i * 2U + 1U]);

        if (hi < 0 || lo < 0) {
            return 0U;
        }

        out[i] = (uint8_t)(((uint8_t)hi << 4U) | (uint8_t)lo);
    }

    return 1U;
}

static uint8_t ota_hex_to_bytes_64(const char *hex, uint8_t out[64])
{
    if (!hex || !out || strlen(hex) != 128U) return 0U;
    for (uint32_t i = 0U; i < 64U; i++) {
        int8_t hi = ota_hex_nibble(hex[i * 2U]);
        int8_t lo = ota_hex_nibble(hex[i * 2U + 1U]);
        if (hi < 0 || lo < 0) return 0U;
        out[i] = (uint8_t)(((uint8_t)hi << 4U) | (uint8_t)lo);
    }
    return 1U;
}

static void ota_copy_manifest_string(char *dst, uint32_t dst_len, const char *src)
{
    if (!dst || dst_len == 0U) {
        return;
    }

    memset(dst, 0, dst_len);

    if (!src) {
        return;
    }

    strncpy(dst, src, dst_len - 1U);
}

static uint8_t ota_write_pending_manifest(uint32_t image_size,
                                          const char *sha256_hex,
                                          const char *signature_hex,
                                          const char *deployment_id,
                                          const char *target_version)
{
    app_ota_manifest_t manifest;
    app_ota_manifest_t verify;

    if (!s_ota_flash || image_size == 0U || image_size > APP_OTA_MAX_IMAGE_SIZE) {
        return 0U;
    }

    memset(&manifest, 0xFF, sizeof(manifest));
    manifest.magic = APP_OTA_MANIFEST_MAGIC;
    manifest.version = APP_OTA_MANIFEST_VERSION;
    manifest.header_size = sizeof(manifest);
    manifest.state = APP_OTA_STATE_PENDING;
    manifest.image_size = image_size;
    manifest.image_addr = APP_OTA_STAGING_W25Q_ADDR;
    manifest.app_addr = APP_OTA_APPLICATION_ADDR;
    manifest.app_max_size = APP_OTA_APPLICATION_SIZE;
    manifest.install_result = APP_OTA_RESULT_NONE;
    manifest.install_error = APP_OTA_ERR_UNSET;
    manifest.backup_valid = 0xFFFFFFFFUL;
    manifest.trial_attempts = 0xFFFFFFFFUL;
    manifest.trial_confirmed = 0xFFFFFFFFUL;
    manifest.rollback_requested = 0xFFFFFFFFUL;

    if (!ota_hex_to_bytes_32(sha256_hex, manifest.sha256)) {
        return 0U;
    }
    if (!ota_hex_to_bytes_64(signature_hex, manifest.signature_ed25519)) return 0U;

    ota_copy_manifest_string(manifest.deployment_id,
                             sizeof(manifest.deployment_id),
                             deployment_id);
    ota_copy_manifest_string(manifest.target_version,
                             sizeof(manifest.target_version),
                             target_version);
    ota_copy_manifest_string(manifest.rollback_version,
                             sizeof(manifest.rollback_version),
                             APP_FIRMWARE_VERSION);

    sha256_ctx_t rollback_sha;
    sha256_init(&rollback_sha);
    for (uint32_t offset = 0U; offset < APP_OTA_SLOT_SIZE; offset += 512U) {
        uint32_t chunk = APP_OTA_SLOT_SIZE - offset;
        if (chunk > 512U) chunk = 512U;
        sha256_update(&rollback_sha,
                      (const uint8_t *)(APP_OTA_APPLICATION_ADDR + offset),
                      chunk);
    }
    sha256_final(&rollback_sha, manifest.rollback_sha256);

    manifest.crc32 = ota_crc32_sw(&manifest, APP_OTA_MANIFEST_CRC_LEN);

    if (!w25q_erase_sector_4k(s_ota_flash, APP_OTA_MANIFEST_W25Q_ADDR)) {
        return 0U;
    }

    if (!w25q_write(s_ota_flash,
                    APP_OTA_MANIFEST_W25Q_ADDR,
                    (const uint8_t *)&manifest,
                    sizeof(manifest))) {
        return 0U;
    }

    memset(&verify, 0, sizeof(verify));
    if (!w25q_read(s_ota_flash,
                   APP_OTA_MANIFEST_W25Q_ADDR,
                   (uint8_t *)&verify,
                   sizeof(verify))) {
        return 0U;
    }

    return (memcmp(&manifest, &verify, sizeof(manifest)) == 0) ? 1U : 0U;
}

static uint8_t ota_read_manifest(app_ota_manifest_t *manifest)
{
    if (!s_ota_flash || !manifest) {
        return 0U;
    }

    memset(manifest, 0, sizeof(*manifest));
    if (!w25q_read(s_ota_flash,
                   APP_OTA_MANIFEST_W25Q_ADDR,
                   (uint8_t *)manifest,
                   sizeof(*manifest))) {
        return 0U;
    }

    if (manifest->magic != APP_OTA_MANIFEST_MAGIC ||
        manifest->version != APP_OTA_MANIFEST_VERSION ||
        manifest->header_size != sizeof(*manifest) ||
        manifest->state != APP_OTA_STATE_PENDING ||
        manifest->image_addr != APP_OTA_STAGING_W25Q_ADDR ||
        manifest->app_addr != APP_OTA_APPLICATION_ADDR ||
        manifest->app_max_size != APP_OTA_APPLICATION_SIZE ||
        manifest->image_size == 0U ||
        manifest->image_size > APP_OTA_MAX_IMAGE_SIZE) {
        return 0U;
    }

    return (ota_crc32_sw(manifest, APP_OTA_MANIFEST_CRC_LEN) == manifest->crc32) ? 1U : 0U;
}

static uint8_t ota_clear_manifest(void)
{
    if (!s_ota_flash) {
        return 0U;
    }

    return w25q_erase_sector_4k(s_ota_flash, APP_OTA_MANIFEST_W25Q_ADDR);
}

static uint8_t ota_write_manifest_marker(size_t field_offset, uint32_t value)
{
    return s_ota_flash && w25q_write(s_ota_flash,
                                     APP_OTA_MANIFEST_W25Q_ADDR + (uint32_t)field_offset,
                                     (const uint8_t *)&value,
                                     sizeof(value));
}

static const char *ota_boot_error_reason(uint32_t err)
{
    switch (err) {
    case APP_OTA_ERR_NONE:
        return "bootloader_install_ok";
    case APP_OTA_ERR_STAGED_HASH:
        return "bootloader_staged_hash_mismatch";
    case APP_OTA_ERR_FLASH_PROGRAM:
        return "bootloader_flash_program_failed";
    case APP_OTA_ERR_FLASH_VERIFY:
        return "bootloader_flash_verify_failed";
    case APP_OTA_ERR_BACKUP:
        return "bootloader_previous_image_backup_failed";
    case APP_OTA_ERR_TRIAL_EXHAUSTED:
        return "trial_boot_limit_reached_previous_image_restored";
    case APP_OTA_ERR_MANUAL_ROLLBACK:
        return "manual_rollback_previous_image_restored";
    case APP_OTA_ERR_SIGNATURE_INVALID:
        return "bootloader_ed25519_signature_invalid";
    default:
        return "bootloader_unknown_result";
    }
}

static const char *json_trimmed_str_or_empty(cJSON *root, const char *key)
{
    const char *s = json_str_or_empty(root, key);

    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        s++;
    }

    return s;
}

static uint8_t json_type_equals(const char *type, const char *expected)
{
    if (!type || !expected) {
        return 0U;
    }

    while (*type == ' ' || *type == '\t' || *type == '\r' || *type == '\n') {
        type++;
    }

    while (*type && *expected) {
        char a = *type++;
        char b = *expected++;

        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b - 'A' + 'a');
        }

        if (a != b) {
            return 0U;
        }
    }

    while (*type == ' ' || *type == '\t' || *type == '\r' || *type == '\n') {
        type++;
    }

    return (*type == '\0' && *expected == '\0') ? 1U : 0U;
}

static uint8_t publish_ota_status(const char *deployment_id,
                                  const char *status,
                                  const char *stage,
                                  int progress,
                                  const char *target_version,
                                  const char *reason)
{
    char message[512];

    const char *safe_deployment = (deployment_id && deployment_id[0]) ? deployment_id : "unknown";
    const char *safe_status = (status && status[0]) ? status : "OTA_UNKNOWN";
    const char *safe_stage = (stage && stage[0]) ? stage : "UNKNOWN";
    const char *safe_target = (target_version && target_version[0]) ? target_version : "";
    const char *safe_reason = (reason && reason[0]) ? reason : "";

    if (progress < 0) {
        progress = 0;
    } else if (progress > 100) {
        progress = 100;
    }

    snprintf(message,
             sizeof(message),
             "{"
             "\"type\":\"ota_ack\","
             "\"deploymentId\":\"%s\","
             "\"status\":\"%s\","
             "\"stage\":\"%s\","
             "\"progress\":%d,"
             "\"currentVersion\":\"%s\","
             "\"rollbackVersion\":\"%s\","
             "\"targetVersion\":\"%s\","
             "\"deviceModel\":\"%s\","
             "\"reason\":\"%s\""
             "}",
             safe_deployment,
             safe_status,
             safe_stage,
             progress,
             APP_FIRMWARE_VERSION,
             s_ota_rollback_version[0] ? s_ota_rollback_version : APP_FIRMWARE_VERSION,
             safe_target,
             APP_DEVICE_MODEL,
             safe_reason);

    OTA_LOG("[OTA] ACK publish: %s\r\n", message);
    return MQTT_Publish(topic_d2c_message, message);
}

static uint8_t publish_ota_status_reported(const char *deployment_id,
                                           const char *status,
                                           const char *stage,
                                           int progress,
                                           const char *target_version,
                                           const char *reason)
{
    uint8_t ok = publish_ota_status(deployment_id,
                                    status,
                                    stage,
                                    progress,
                                    target_version,
                                    reason);
    comm_manager_report_publish_result(ok);
    return ok;
}

uint8_t MQTT_ReportOtaBootResultOnce(void)
{
    app_ota_manifest_t manifest;
    uint8_t ok;

    if (s_ota_boot_result_seen_empty) {
        return 0U;
    }

    if (!ota_read_manifest(&manifest)) {
        s_ota_boot_result_seen_empty = 1U;
        return 0U;
    }

    if (manifest.install_result == APP_OTA_RESULT_NONE) {
        return 0U;
    }

    if (manifest.install_result == APP_OTA_RESULT_INSTALLED) {
        ota_copy_manifest_string(s_ota_rollback_version, sizeof(s_ota_rollback_version), manifest.rollback_version);
        if (!s_ota_installed_ack_sent) {
            ok = publish_ota_status((const char *)manifest.deployment_id,
                                    "OTA_INSTALLED", "BOOT", 100,
                                    (const char *)manifest.target_version,
                                    "bootloader_install_ok_trial_started");
            comm_manager_report_publish_result(ok);
            if (!ok) return 0U;
            s_ota_installed_ack_sent = 1U;
            s_ota_trial_started_tick = HAL_GetTick();
        }
        if (manifest.trial_confirmed != APP_OTA_TRIAL_CONFIRMED) {
            if ((uint32_t)(HAL_GetTick() - s_ota_trial_started_tick) < OTA_TRIAL_CONFIRM_DELAY_MS) return 0U;
            if (!ota_write_manifest_marker(offsetof(app_ota_manifest_t, trial_confirmed), APP_OTA_TRIAL_CONFIRMED)) return 0U;
        }
        ok = publish_ota_status((const char *)manifest.deployment_id,
                                "OTA_CONFIRMED", "HEALTH_CHECK", 100,
                                (const char *)manifest.target_version,
                                "five_minute_trial_passed");
        comm_manager_report_publish_result(ok);
        if (ok) s_ota_boot_result_seen_empty = 1U;
        return ok;
    }

    if (manifest.install_result == APP_OTA_RESULT_FAILED ||
        manifest.install_result == APP_OTA_RESULT_ROLLED_BACK) {
        ota_copy_manifest_string(s_ota_rollback_version, sizeof(s_ota_rollback_version), manifest.rollback_version);
        const uint8_t rolled_back = manifest.install_result == APP_OTA_RESULT_ROLLED_BACK;
        /*
         * After INSTALLED the error word is already 0 (ERR_NONE), so the
         * bootloader's rollback error code cannot be programmed over it. Tell
         * a manual rollback from a failed trial by the rollback-request marker.
         */
        const char *reason = rolled_back
            ? (manifest.rollback_requested == APP_OTA_ROLLBACK_REQUESTED
                   ? ota_boot_error_reason(APP_OTA_ERR_MANUAL_ROLLBACK)
                   : ota_boot_error_reason(APP_OTA_ERR_TRIAL_EXHAUSTED))
            : ota_boot_error_reason(manifest.install_error);
        ok = publish_ota_status((const char *)manifest.deployment_id,
                                rolled_back ? "OTA_ROLLED_BACK" : "OTA_INSTALL_FAILED",
                                "BOOT", 100,
                                (const char *)manifest.target_version,
                                reason);
        comm_manager_report_publish_result(ok);
        if (ok) {
            (void)ota_clear_manifest();
            s_ota_boot_result_seen_empty = 1U;
        }
        return ok;
    }

    return 0U;
}

static uint8_t handle_ota_rollback_cjson(cJSON *root)
{
    const char *deployment_id = json_str_or_empty(root, "deploymentId");
    app_ota_manifest_t manifest;
    if (!deployment_id[0] || !ota_read_manifest(&manifest) ||
        strcmp(deployment_id, manifest.deployment_id) != 0 ||
        manifest.install_result != APP_OTA_RESULT_INSTALLED ||
        manifest.backup_valid != APP_OTA_BACKUP_VALID ||
        manifest.trial_confirmed != APP_OTA_TRIAL_CONFIRMED ||
        strcmp(json_str_or_empty(root, "targetVersion"), manifest.rollback_version) != 0) {
        if (deployment_id[0]) {
            (void)publish_ota_status_reported(deployment_id, "OTA_REJECTED", "ROLLBACK", 0,
                                               json_str_or_empty(root, "targetVersion"),
                                               "rollback_backup_or_deployment_unavailable");
        }
        return 0U;
    }
    if (!ota_write_manifest_marker(offsetof(app_ota_manifest_t, rollback_requested),
                                   APP_OTA_ROLLBACK_REQUESTED)) return 0U;
    app_gateway_set_reset_intent(APP_RESET_INTENT_OTA_ROLLBACK);
    NVIC_SystemReset();
    return 1U;
}

static uint8_t handle_ota_cjson(cJSON *root)
{
    const char *deployment_id = json_str_or_empty(root, "deploymentId");
    const char *target_version = json_str_or_empty(root, "firmwareVersion");
    const char *device_model = json_str_or_empty(root, "deviceModel");
    const char *artifact_url = json_str_or_empty(root, "artifactUrl");
    const char *sha256 = json_str_or_empty(root, "sha256");
    const char *signature = json_str_or_empty(root, "signatureEd25519");
    uint8_t signature_bytes[64];
    int size = json_int_or(root, "size", 0);

    if (target_version[0] == '\0') {
        target_version = json_str_or_empty(root, "targetVersion");
    }
    if (target_version[0] == '\0') {
        target_version = json_str_or_empty(root, "version");
    }
    if (artifact_url[0] == '\0') {
        artifact_url = json_str_or_empty(root, "downloadUrl");
    }
    if (sha256[0] == '\0') {
        sha256 = json_str_or_empty(root, "checksumSha256");
    }
    if (signature[0] == '\0') signature = json_str_or_empty(root, "signature");
    if (size <= 0) {
        size = json_int_or(root, "sizeBytes", 0);
    }

    if (deployment_id[0] == '\0') {
        (void)publish_ota_status_reported("unknown",
                                          "OTA_REJECTED",
                                          "VALIDATE",
                                          0,
                                          target_version,
                                          "missing_deploymentId");
        return 1U;
    }

    if (s_ota_busy) {
        (void)publish_ota_status_reported(deployment_id,
                                          "OTA_REJECTED",
                                          "VALIDATE",
                                          0,
                                          target_version,
                                          "ota_busy");
        return 1U;
    }

    /* Hold the OTA gate from the moment this queued command is accepted.
     * The runtime uses this flag to suppress telemetry while OTA work runs. */
    s_ota_busy = 1U;
    if (!publish_ota_status_reported(deployment_id,
                                     "OTA_RECEIVED",
                                     "RECEIVE",
                                     0,
                                     target_version,
                                     "received")) {
        mqtt_ota_release_busy();
        return 1U;
    }

    if (target_version[0] == '\0' ||
        device_model[0] == '\0' ||
        artifact_url[0] == '\0' ||
        sha256[0] == '\0' ||
        signature[0] == '\0' ||
        size <= 0) {
        (void)publish_ota_status_reported(deployment_id,
                                          "OTA_REJECTED",
                                          "VALIDATE",
                                          0,
                                          target_version,
                                          signature[0] == '\0' ? "missing_ed25519_signature" : "missing_required_field");
        mqtt_ota_release_busy();
        return 1U;
    }

    if (strlen(signature) != 128U || !ota_hex_to_bytes_64(signature, signature_bytes)) {
        (void)publish_ota_status_reported(deployment_id, "OTA_REJECTED", "VALIDATE", 0,
                                          target_version, "invalid_ed25519_signature_format");
        mqtt_ota_release_busy();
        return 1U;
    }

    if (strcmp(device_model, APP_DEVICE_MODEL) != 0) {
        (void)publish_ota_status_reported(deployment_id,
                                          "OTA_REJECTED",
                                          "VALIDATE",
                                          0,
                                          target_version,
                                          "device_model_mismatch");
        mqtt_ota_release_busy();
        return 1U;
    }

    const char *url_reject_reason = NULL;
    if (!ota_url_is_supported(artifact_url, &url_reject_reason)) {
        (void)publish_ota_status_reported(deployment_id,
                                          "OTA_REJECTED",
                                          "VALIDATE",
                                          0,
                                          target_version,
                                          url_reject_reason);
        mqtt_ota_release_busy();
        return 1U;
    }

    if (!s_ota_flash) {
        (void)publish_ota_status_reported(deployment_id,
                                          "OTA_REJECTED",
                                          "VALIDATE",
                                          0,
                                          target_version,
                                          "ota_flash_not_ready");
        mqtt_ota_release_busy();
        return 1U;
    }

    if ((uint32_t)size > OTA_STAGING_MAX_SIZE ||
        (uint32_t)size > APP_OTA_MAX_IMAGE_SIZE) {
        (void)publish_ota_status_reported(deployment_id,
                                          "OTA_REJECTED",
                                          "VALIDATE",
                                          0,
                                          target_version,
                                          "artifact_too_large");
        mqtt_ota_release_busy();
        return 1U;
    }

    if (!publish_ota_status_reported(deployment_id,
                                     "OTA_DOWNLOAD_STARTED",
                                     "DOWNLOAD",
                                     1,
                                     target_version,
                                     "staging_to_external_flash")) {
        mqtt_ota_release_busy();
        return 1U;
    }

    /* Keep modem command/result lines visible during an OTA download even
     * when the saved device config has routine USB diagnostics disabled. */
    debug_uart_force_enabled_for(180000U);
    print("[OTA] download trace enabled on USB\r\n");

    if (!mqtt_stop_for_ota_http()) {
        (void)publish_ota_status_reported(deployment_id,
                                          "OTA_DOWNLOAD_FAILED",
                                          "DOWNLOAD",
                                          1,
                                          target_version,
                                          "mqtt_tls_release_failed");
        debug_uart_force_enabled_for(0U);
        mqtt_ota_release_busy();
        return 1U;
    }

    uint32_t downloaded_size = 0U;
    char downloaded_sha256[65] = {0};
    ota_progress_ctx_t progress = {0U};

    app_debug_led_set_ota_active(1U);
    /* Use the modem HTTP(S) client so HTTPS requests follow the configured
     * SSL context. The older manual TCP path always opened port 80 and would
     * silently bypass TLS even when the URL looked secure. */
    uint8_t download_ok = modem_http_download_to_w25q(artifact_url,
                                                      s_ota_flash,
                                                      OTA_STAGING_BASE_ADDR,
                                                      OTA_STAGING_MAX_SIZE,
                                                      (uint32_t)size,
                                                      sha256,
                                                      &downloaded_size,
                                                      downloaded_sha256,
                                                      ota_download_progress,
                                                      &progress);
    app_debug_led_set_ota_active(0U);

    /* Restore MQTT before reporting any download/verification result. The
     * normal runtime remains gated by s_ota_busy until the OTA path exits. */
    if (!mqtt_resume_after_ota_http()) {
        app_log_critical(APP_LOG_ERROR,
                         "OTA",
                         "MQTT_RESUME_FAILED",
                         "MQTT did not resume after OTA HTTPS operation",
                         download_ok,
                         progress.last_percent,
                         "MQTT_RECONNECT");
        debug_uart_force_enabled_for(0U);
        mqtt_ota_release_busy();
        return 1U;
    }

    if (!download_ok) {
        const char *fail_reason = modem_http_download_last_error();
        if (!fail_reason || !fail_reason[0] || strcmp(fail_reason, "none") == 0) {
            fail_reason = "download_or_verify_failed";
        }
        (void)publish_ota_status_reported(deployment_id,
                                          "OTA_DOWNLOAD_FAILED",
                                          "DOWNLOAD",
                                          (int)progress.last_percent,
                                          target_version,
                                          fail_reason);
        debug_uart_force_enabled_for(0U);
        mqtt_ota_release_busy();
        return 1U;
    }

    uint8_t ack_ok = publish_ota_status_reported(deployment_id,
                                                 "OTA_DOWNLOAD_COMPLETE",
                                                 "DOWNLOAD",
                                                 100,
                                                 target_version,
                                                 "downloaded_to_w25q");
    if (!ack_ok) {
        debug_uart_force_enabled_for(0U);
        mqtt_ota_release_busy();
        return 1U;
    }

    ack_ok = publish_ota_status_reported(deployment_id,
                                         "OTA_VERIFY_OK",
                                         "VERIFY",
                                         100,
                                         target_version,
                                         downloaded_sha256);
    if (!ack_ok) {
        debug_uart_force_enabled_for(0U);
        mqtt_ota_release_busy();
        return 1U;
    }

    if (!ota_write_pending_manifest(downloaded_size,
                                    downloaded_sha256,
                                    signature,
                                    deployment_id,
                                    target_version)) {
        (void)publish_ota_status_reported(deployment_id,
                                          "OTA_INSTALL_FAILED",
                                          "MANIFEST",
                                          100,
                                          target_version,
                                          "manifest_write_failed");
        debug_uart_force_enabled_for(0U);
        mqtt_ota_release_busy();
        return 1U;
    }

    ack_ok = publish_ota_status_reported(deployment_id,
                                         "OTA_INSTALLING",
                                         "RESET",
                                         100,
                                         target_version,
                                         "bootloader_install_scheduled");
    if (!ack_ok) {
        debug_uart_force_enabled_for(0U);
        mqtt_ota_release_busy();
        return 1U;
    }

    osDelay(1500U);
    app_gateway_set_reset_intent(APP_RESET_INTENT_OTA_INSTALL);
    NVIC_SystemReset();

    return 1U;
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

static const char *mqtt_topic_kind(const char *topic)
{
    if (!topic || !topic[0]) return "unknown";
    if (strcmp(topic, topic_d2c_telemetry) == 0) return "telemetry";
    if (strcmp(topic, topic_d2c_logs) == 0) return "log";
    if (strcmp(topic, topic_d2c_health) == 0) return "health";
    if (strcmp(topic, topic_d2c_event) == 0) return "event";
    if (strcmp(topic, topic_d2c_message) == 0) return "message";
    if (strcmp(topic, topic_c2d_commands) == 0) return "command";
    return "custom";
}

/* =========================================================
   Topics
   ========================================================= */
void Prepare_MQTT_Topics(void)
{
    /* Use the modem-reported IMEI; a fixed topic identity can route this
     * physical unit's traffic to another registered device. */
    if (!imei[0])
    {
        print("[MQTT] topics not prepared: modem IMEI unavailable\r\n");
        return;
    }
    const char *device_id = imei;

    snprintf(topic_d2c_telemetry, sizeof(topic_d2c_telemetry),
             "d2c/%s/telemetry_test", device_id);

    snprintf(topic_c2d_commands, sizeof(topic_c2d_commands),
             "c2d/%s/commands_test", device_id);

    snprintf(topic_d2c_message, sizeof(topic_d2c_message),
             "d2c/%s/message_test", device_id);

    snprintf(topic_d2c_logs, sizeof(topic_d2c_logs),
             "d2c/%s/logs_test", device_id);

    snprintf(topic_d2c_health, sizeof(topic_d2c_health),
             "d2c/%s/health_test", device_id);

    snprintf(topic_d2c_event, sizeof(topic_d2c_event),
             "d2c/%s/event_test", device_id);

    print("[MQTT] topics ready imei=%s commandTopic=%s\r\n",
          device_id,
          topic_c2d_commands);
}

/* =========================================================
   FIXED MQTT CONNECT (Old Logic + New Engine)
   ========================================================= */
uint8_t MQTT_Connect(const char *host, int port, const char *user, const char *pass)
{
    char cmd[180];
    const char *fail_stage = "START";
    uint8_t verify = 0U;

    /* Fallback to config.h if arguments are NULL */
    const char *host_u = (host && host[0]) ? host : MQTT_HOST_URI;
    int port_u         = (port > 0) ? port : MQTT_PORT;
    const char *user_u = (user && user[0]) ? user : MQTT_USERNAME;
    const char *pass_u = (pass && pass[0]) ? pass : MQTT_PASSWORD;

    print("[MQTT] connecting to broker host=%s port=%d\r\n", host_u, port_u);

    /* 1. Reset Session (flush_rx = true to clear previous noise) */
    /* 1. Reset/Cleanup previous MQTT session */
    print("[MQTT] cleaning previous modem MQTT session\r\n");

    fail_stage = "CLEANUP_DISC";
    (void)SIM800_SendATCommand2("AT+CMQTTDISC=0,120", "OK", 3000, true);
    osDelay(300);

    fail_stage = "CLEANUP_REL";
    (void)SIM800_SendATCommand2("AT+CMQTTREL=0", "OK", 3000, true);
    osDelay(300);

    fail_stage = "CLEANUP_STOP";
    (void)SIM800_SendATCommand2("AT+CMQTTSTOP", "OK", 5000, true);
    osDelay(1000);

    /* 2. TLS setup: verify the broker chain (see MQTT_CA_FILE above). */
    verify = 1U;
    uint8_t ca_ok = modem_tls_ensure_ca(MQTT_CA_FILE, mqtt_root_ca_pem, (uint16_t)MQTT_ROOT_CA_PEM_SIZE);
    uint8_t in_fallback = (!ca_ok) ||
        (s_tls_fail_streak_started && (uint32_t)(HAL_GetTick() - s_tls_fail_since) >= MQTT_TLS_FALLBACK_MS);
    if (in_fallback) {
        verify = s_tls_try_insecure ? 0U : 1U;
        if (!ca_ok) verify = 0U;
        s_tls_try_insecure ^= 1U;
    }
    print("[MQTT] TLS %s\r\n", verify ? "verifying broker certificate" : "WITHOUT certificate check (fallback)");

    fail_stage = "SSL_VERSION";
    if (!SendCommandWithRetry("AT+CSSLCFG=\"sslversion\",0,4", "OK", 5000, 3)) goto fail;
    fail_stage = "SSL_AUTHMODE";
    if (verify) {
        if (!SendCommandWithRetry("AT+CSSLCFG=\"authmode\",0,1", "OK", 5000, 3)) goto fail;
        fail_stage = "SSL_CACERT";
        snprintf(cmd, sizeof(cmd), "AT+CSSLCFG=\"cacert\",0,\"%s\"", MQTT_CA_FILE);
        if (!SendCommandWithRetry(cmd, "OK", 5000, 3)) goto fail;
        fail_stage = "SSL_IGNORETIME";
        if (!SendCommandWithRetry("AT+CSSLCFG=\"ignorelocaltime\",0,1", "OK", 5000, 3)) goto fail;
    } else {
        if (!SendCommandWithRetry("AT+CSSLCFG=\"authmode\",0,0", "OK", 5000, 3)) goto fail;
    }

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

    /* --- ASYNC FIX ---
     * Step A: Send the command and wait for the modem's immediate "OK".
     * We use flush_rx=true here to ensure the buffer is clean before sending.
     */
    fail_stage = "CONNECT_OK";
    if (!SIM800_SendATCommand2(cmd, "OK", MQTT_CONNECT_CMD_OK_TIMEOUT_MS, true)) {
        print("[MQTT][ERR] connect command was not accepted by modem\r\n");
        goto fail;
    }

    /* Step B: Wait for the Cloud URC (+CMQTTCONNECT: 0,0).
     * We use u2rx_wait_for because the success message might take several seconds
     * while the TLS handshake completes.
     */
    print("[MQTT] waiting for broker connection confirmation\r\n");
    fail_stage = "CONNECT_URC";
    if (!u2rx_wait_for(&g_u2rx, "+CMQTTCONNECT: 0,0", MQTT_CONNECT_URC_TIMEOUT_MS)) {
        print("[MQTT][ERR] broker connection was not confirmed\r\n");
        goto fail;
    }

    print("[MQTT] connected\r\n");
    /* Disconnect notices from the previous session are stale now. */
    modem_urc_clear_disconnect_flags();
    lastMQTTSuccessTime = HAL_GetTick();
    s_consecutive_mqtt_connect_fail = 0;
    s_tls_verified = verify;
    if (verify) {
        s_tls_fail_streak_started = 0U;
        s_tls_try_insecure = 0U;
    } else {
        app_log_critical(APP_LOG_WARN, "MQTT", "TLS_UNVERIFIED",
                     "connected without broker certificate check (fallback)", 0, 0, "NONE");
    }
    return 1;

    fail:
        if (verify && (strcmp(fail_stage, "CONNECT_URC") == 0 || strcmp(fail_stage, "CONNECT_OK") == 0)) {
            if (!s_tls_fail_streak_started) {
                s_tls_fail_streak_started = 1U;
                s_tls_fail_since = HAL_GetTick();
            }
        }
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

        print("[MQTT][ERR] connect failed stage=%s consecutive=%u detail=%s\r\n",
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
            app_log_critical(APP_LOG_ERROR,
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
        print("[MQTT][ERR] subscribe was not confirmed by broker\r\n");
        goto fail;
    }

    print("[MQTT] subscribed to downlink topic=%s\r\n", topic_u);
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
        app_log_critical(APP_LOG_ERROR,
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
/* A normal publish keeps MQTT_PUBLISH_MIN_GAP_MS from the previous NORMAL
 * publish, and only this short breather from a priority one (ack). Before
 * 2026-11-17 every ack restarted the 3 s wait, so a command every ~1.2 s held
 * back telemetry and health for minutes (bench: 7 min, unit marked offline). */
#define MQTT_PUBLISH_AFTER_PRIORITY_MS    500UL
#define MQTT_PUBLISH_FAIL_COOLDOWN_MS   15000UL

static uint32_t s_last_mqtt_publish_attempt_tick = 0;
static uint32_t s_last_normal_publish_tick = 0;

/* The 3 s gap between normal publishes also capped telemetry at one every
 * ~3.4 s whatever the interval. With a faster interval (1.0.15: 500 ms /
 * 1 s options) the gap follows the interval, never under 300 ms. */
static uint32_t normal_publish_gap_ms(void)
{
    uint32_t interval = app_modbus_payload_get_telemetry_interval_ms();
    if (interval >= MQTT_PUBLISH_MIN_GAP_MS) return MQTT_PUBLISH_MIN_GAP_MS;
    /* Never under 1 s: at 500 ms the unit handed the modem ~2 KB faster than
     * the cellular uplink carried it, readings queued and reached the cloud
     * 7-19 s late, and command acks waited up to 19 s. At 1 s the backlog
     * does not build (bench 2026-11-19: delay p50 1.0 s). */
    return (interval < 1000UL) ? 1000UL : interval;
}

static uint32_t normal_publish_wait_ms(uint32_t now)
{
    uint32_t since_normal = now - s_last_normal_publish_tick;
    uint32_t since_any = now - s_last_mqtt_publish_attempt_tick;
    uint32_t gap = normal_publish_gap_ms();
    uint32_t a = (since_normal < gap) ? (gap - since_normal) : 0U;
    uint32_t b = (since_any < MQTT_PUBLISH_AFTER_PRIORITY_MS) ? (MQTT_PUBLISH_AFTER_PRIORITY_MS - since_any) : 0U;
    return (a > b) ? a : b;
}
static uint32_t s_mqtt_publish_block_until_tick = 0;

#define MQTT_FAST_TELEMETRY_MAX_MS 2000UL

static uint8_t mqtt_publish_impl(const char *topic, const char *payload, uint8_t enforce_min_gap)
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
        print("[MQTT PUB] skipped: modem publish cooldown active\r\n");
        s_last_publish_attempted = 0U;
        s_last_publish_stage = "COOLDOWN";
        return 0;
    }

    /*
     * Avoid back-to-back MQTT publish commands.
     * SIMCom MQTT engine becomes unstable in weak network if hammered.
     */
    uint32_t elapsed = now - s_last_mqtt_publish_attempt_tick;
    if (enforce_min_gap) {
        uint32_t wait_ms = normal_publish_wait_ms(now);
        if (wait_ms) osDelay(wait_ms);
    } else if (elapsed < MQTT_PUBLISH_MIN_GAP_MS) {
        print("[MQTT PUB] priority publish bypassed min gap elapsed=%lu ms\r\n",
              (unsigned long)elapsed);
    }

    s_last_mqtt_publish_attempt_tick = HAL_GetTick();
    if (enforce_min_gap) s_last_normal_publish_tick = s_last_mqtt_publish_attempt_tick;

    char cmd[128];
    /* Fast telemetry (and health while it runs): QoS 0, short pauses
     * between the modem steps. Health is included because a QoS 1 health
     * message waited 13 s for its PUBACK on the bench and stalled telemetry. */
    uint8_t fast = ((strcmp(topic, topic_d2c_telemetry) == 0 || strcmp(topic, topic_d2c_health) == 0) &&
                    app_modbus_payload_get_telemetry_interval_ms() <= MQTT_FAST_TELEMETRY_MAX_MS) ? 1U : 0U;
    uint32_t step_delay = fast ? 30U : 100U;
    print("[MQTT PUB] sending %s payloadLen=%lu\r\n",
          mqtt_topic_kind(topic),
          (unsigned long)strlen(payload));

    snprintf(cmd, sizeof(cmd), "AT+CMQTTTOPIC=0,%d", (int)strlen(topic));
    fail_stage = "TOPIC_PROMPT";
    if (!SendCommandWithRetry(cmd, ">", MQTT_PUBLISH_TOPIC_PROMPT_MS, 1)) {
        print("[MQTT PUB][ERR] modem did not accept publish topic\r\n");
        goto fail;
    }

    osDelay(step_delay);

    fail_stage = "TOPIC_SEND";
    if (!SendCommandWithRetry(topic, "OK", MQTT_PUBLISH_TOPIC_SEND_MS, 1)) {
        print("[MQTT PUB][ERR] topic send failed\r\n");
        goto fail;
    }

    osDelay(step_delay);

    snprintf(cmd, sizeof(cmd), "AT+CMQTTPAYLOAD=0,%d", (int)strlen(payload));
    fail_stage = "PAYLOAD_PROMPT";
    if (!SendCommandWithRetry(cmd, ">", MQTT_PUBLISH_PAYLOAD_PROMPT_MS, 1)) {
        print("[MQTT PUB][ERR] modem did not accept publish payload\r\n");
        goto fail;
    }

    osDelay(step_delay);

    fail_stage = "PAYLOAD_SEND";
    if (!SendCommandWithRetry(payload, "OK", MQTT_PUBLISH_PAYLOAD_SEND_MS, 1)) {
        print("[MQTT PUB][ERR] payload send failed\r\n");
        goto fail;
    }

    osDelay(step_delay);

    /* Fast telemetry (interval <= 2 s, 1.0.15) goes at QoS 0: waiting for
     * the broker's PUBACK took 1-5 s on the bench (India -> eu-central-1
     * over cellular), so at QoS 1 a 500 ms / 1 s interval delivered one
     * reading every ~6 s. At QoS 0 an occasional reading can be lost; the
     * platform counts it as a gap (telemetry seq). Everything else stays
     * QoS 1. */
    uint8_t qos = fast ? 0U : 1U;
    snprintf(cmd, sizeof(cmd), "AT+CMQTTPUB=0,%u,60", (unsigned)qos);
    fail_stage = "PUB_CMD_OK";
    if (!SendCommandWithRetry(cmd, "OK", MQTT_PUBLISH_CMD_OK_TIMEOUT_MS, 1)) {
        print("[MQTT PUB][ERR] publish command was rejected by modem\r\n");
        goto fail;
    }

    fail_stage = "PUB_URC";
    if (!u2rx_wait_for(&g_u2rx, "+CMQTTPUB: 0,0", MQTT_PUBLISH_URC_TIMEOUT_MS)) {
        print("[MQTT PUB][ERR] broker publish acknowledgement timed out\r\n");
        goto fail;
    }

    print("[MQTT PUB] published %s payloadLen=%lu\r\n",
          mqtt_topic_kind(topic),
          (unsigned long)strlen(payload));

    lastMQTTSuccessTime = HAL_GetTick();
    s_last_publish_stage = "OK";
    app_debug_led_event(DEBUG_LED_EVENT_UPLINK);
    return 1;

fail:
    s_last_publish_stage = fail_stage;
    s_mqtt_publish_block_until_tick = HAL_GetTick() + MQTT_PUBLISH_FAIL_COOLDOWN_MS;
    print("[MQTT PUB][ERR] failed %s payloadLen=%lu stage=%s\r\n",
          mqtt_topic_kind(topic),
          (unsigned long)strlen(payload),
          fail_stage);
    mqtt_log_diag("publish", fail_stage);
    return 0;
}

uint8_t MQTT_Publish(const char *topic, const char *payload)
{
    return mqtt_publish_impl(topic, payload, 1U);
}

uint8_t MQTT_PublishPriority(const char *topic, const char *payload)
{
    return mqtt_publish_impl(topic, payload, 0U);
}

/* Time left before a normal (gap-enforcing) publish may start without
 * sleeping. The runtime waits this out itself while still serving
 * downlinks, instead of MQTT_Publish blocking the runtime task in osDelay. */
uint32_t MQTT_PublishGapRemainingMs(void)
{
    return normal_publish_wait_ms(HAL_GetTick());
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
    /*
     * Disconnect notices are latched by the modem URC hook whichever reader
     * sees them. This used to scan the serial stream directly, which also
     * consumed (and lost) any MQTT downlink arriving in that window.
     */
    modem_rx_service(0U);
    uint32_t flags = modem_urc_take_disconnect_flags();

    if (flags & MODEM_URC_FLAG_MQTT_CONN_LOST) {
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

    if (flags & MODEM_URC_FLAG_PDP_DEACT) {
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

    if (flags & MODEM_URC_FLAG_PDN_DEACT) {
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

/* A config too large to receive used to vanish without a reply, so the
 * deployment timed out with no reason. Answer it with "too_large". */
static void report_oversize_downlink(void)
{
    char head[MODEM_URC_OVERSIZE_HEAD];
    uint32_t len = modem_urc_take_oversize(head, sizeof(head));
    if (!len || !strstr(head, "\"config\"")) return;
    char config_id[48] = "unknown";
    const char *p = strstr(head, "\"configId\":\"");
    if (p) {
        p += 12;
        size_t i = 0U;
        while (p[i] && p[i] != '"' && i < sizeof(config_id) - 1U) { config_id[i] = p[i]; i++; }
        config_id[i] = 0;
    }
    print("[CONFIG][ERR] config too large len=%lu configId=%s\r\n", (unsigned long)len, config_id);
    publish_config_status(config_id, "unknown", "error", "too_large");
}

char* MQTT_Subscriber_Run(void)
{
    /*
     * Downlinks are captured into a queue by the modem URC hook as soon as
     * any reader meets them (AT command, publish, signal poll, this call).
     * Here we only service the serial stream briefly and pop the oldest one.
     */
    size_t len = 0U;

    if (!modem_urc_downlink_pending()) {
        modem_rx_service(MQTT_RX_POLL_WAIT_MS);
    }

    if (!modem_urc_pop_downlink(s_mqtt_rx_payload, sizeof(s_mqtt_rx_payload), &len)) {
        report_oversize_downlink();
        return NULL;
    }

    print("[MQTT RX] downlink payload read len=%lu still_queued=%u\r\n",
          (unsigned long)len,
          (unsigned)modem_urc_downlink_pending());
    return s_mqtt_rx_payload;
}

/* Actuation acks are queued and retried until the broker accepts them.
 * Before, each ack was published once, behind the 3 s publish gap, and a
 * failed publish (or the 15 s cooldown after one) lost it: the relay had
 * switched but the platform showed the command as unconfirmed. */
#define ACT_ACK_OUTBOX_SLOTS     24U  /* ~0.85 acks/s is the modem limit; absorb bursts while telemetry publishes */
#define ACT_ACK_MAX_ATTEMPTS     30U
#define ACT_ACK_PAYLOAD_MAX      192U
#define ACT_RECENT_IDS           32U  /* commands also expire after 60 s */
#define ACT_ID_MAX               40U

typedef struct {
    uint8_t used;
    uint8_t attempts;
    char    payload[ACT_ACK_PAYLOAD_MAX];
} act_ack_slot_t;

static act_ack_slot_t s_act_ack_outbox[ACT_ACK_OUTBOX_SLOTS];
static uint8_t        s_act_ack_head;   /* oldest queued slot */
static uint8_t        s_act_ack_count;

/* Ids of actuations already executed, newest last, so a redelivered command
 * (QoS 1 retry, broker redelivery after reconnect) is acked again but does
 * not switch the relay a second time. */
static char    s_act_recent_ids[ACT_RECENT_IDS][ACT_ID_MAX];
static uint8_t s_act_recent_next;

static uint8_t act_recent_contains(const char *id)
{
    if (!id || !id[0] || strcmp(id, "unknown") == 0) return 0U;
    for (uint8_t i = 0U; i < ACT_RECENT_IDS; i++) {
        if (strncmp(s_act_recent_ids[i], id, ACT_ID_MAX - 1U) == 0) return 1U;
    }
    return 0U;
}

static void act_recent_remember(const char *id)
{
    if (!id || !id[0] || strcmp(id, "unknown") == 0) return;
    strncpy(s_act_recent_ids[s_act_recent_next], id, ACT_ID_MAX - 1U);
    s_act_recent_ids[s_act_recent_next][ACT_ID_MAX - 1U] = '\0';
    s_act_recent_next = (uint8_t)((s_act_recent_next + 1U) % ACT_RECENT_IDS);
}

static void publish_actuation_ack(const char *actuation_id,
                                  const char *status,
                                  const char *reason)
{
    if (!actuation_id || actuation_id[0] == '\0') {
        actuation_id = "unknown";
    }

    if (!status || status[0] == '\0') {
        status = "FAILED";
    }

    if (s_act_ack_count >= ACT_ACK_OUTBOX_SLOTS) {
        /* Full: drop the oldest. Eight unsent acks means the link is down
         * for a while; the newest outcome per relay matters most. */
        print("[ACT][WARN] ack outbox full, dropping oldest\r\n");
        app_log_push(APP_LOG_WARN, "ACT", "ACK_OUTBOX_FULL", "oldest ack dropped", 0, 0);
        s_act_ack_outbox[s_act_ack_head].used = 0U;
        s_act_ack_head = (uint8_t)((s_act_ack_head + 1U) % ACT_ACK_OUTBOX_SLOTS);
        s_act_ack_count--;
    }

    uint8_t slot = (uint8_t)((s_act_ack_head + s_act_ack_count) % ACT_ACK_OUTBOX_SLOTS);
    act_ack_slot_t *s = &s_act_ack_outbox[slot];

    if (reason && reason[0]) {
        snprintf(s->payload, sizeof(s->payload), "{\"type\":\"actuation_ack\",\"actuationId\":\"%s\",\"status\":\"%s\",\"reason\":\"%s\"}", actuation_id, status, reason);
    } else {
        snprintf(s->payload, sizeof(s->payload), "{\"type\":\"actuation_ack\",\"actuationId\":\"%s\",\"status\":\"%s\"}", actuation_id, status);
    }
    s->used = 1U;
    s->attempts = 0U;
    s_act_ack_count++;

    print("[ACT] acknowledgement queued actuationId=%s status=%s payloadLen=%lu queued=%u\r\n",
          actuation_id,
          status,
          (unsigned long)strlen(s->payload),
          (unsigned)s_act_ack_count);
}

/* Publish queued acks, oldest first, at priority (no 3 s gap). Returns the
 * number still queued. Called by the runtime right after each downlink and
 * at the top of every loop, before telemetry. */
uint8_t MQTT_FlushActuationAcks(void)
{
    /* Send only the acks queued when this call started. Commands keep
     * arriving (and queueing acks) while each ack is published, so looping
     * until the outbox is empty never returned under a steady command stream
     * and held back telemetry and health (bench soak, 2026-11-17). */
    uint8_t budget = s_act_ack_count;
    while (s_act_ack_count > 0U && budget-- > 0U) {
        act_ack_slot_t *s = &s_act_ack_outbox[s_act_ack_head];

        uint8_t ok = MQTT_PublishPriority(topic_d2c_message, s->payload);
        if (!ok) {
            if (MQTT_LastPublishAttempted()) {
                s->attempts++;
            }
            if (s->attempts >= ACT_ACK_MAX_ATTEMPTS) {
                print("[ACT][ERR] ack dropped after %u attempts\r\n", (unsigned)s->attempts);
                app_log_critical(APP_LOG_ERROR, "ACT", "ACK_DROPPED", "ack not accepted after retries", s->attempts, 0, "NONE");
            } else {
                print("[ACT][WARN] ack publish failed stage=%s attempts=%u, will retry\r\n",
                      MQTT_LastPublishStage(), (unsigned)s->attempts);
                break;
            }
        } else {
            print("[ACT] acknowledgement published queued=%u\r\n", (unsigned)(s_act_ack_count - 1U));
        }

        s->used = 0U;
        s_act_ack_head = (uint8_t)((s_act_ack_head + 1U) % ACT_ACK_OUTBOX_SLOTS);
        s_act_ack_count--;
    }
    return s_act_ack_count;
}

static int64_t actuation_days_from_civil(int year, unsigned month, unsigned day)
{
    year -= month <= 2U;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = (unsigned)(year - era * 400);
    const unsigned doy = (153U * (month + (month > 2U ? (unsigned)-3 : 9U)) + 2U) / 5U + day - 1U;
    const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static uint8_t actuation_parse_expiry(const char *text, int64_t *epoch_out)
{
    int y, mo, d, h, mi, sec;
    int consumed = 0;
    if (!text || !epoch_out || sscanf(text, "%4d-%2d-%2dT%2d:%2d:%2d%n", &y, &mo, &d, &h, &mi, &sec, &consumed) != 6) return 0U;
    if (y < 2000 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || sec > 59) return 0U;
    const char *tz = text + consumed;
    if (*tz == '.') { tz++; while (*tz >= '0' && *tz <= '9') tz++; }
    int offset = 0;
    if (*tz == 'Z' && tz[1] == '\0') {
        offset = 0;
    } else if ((*tz == '+' || *tz == '-') && strlen(tz) == 6U && tz[3] == ':') {
        int oh = (tz[1] - '0') * 10 + tz[2] - '0';
        int om = (tz[4] - '0') * 10 + tz[5] - '0';
        if (oh > 23 || om > 59) return 0U;
        offset = (oh * 60 + om) * (*tz == '+' ? 1 : -1);
    } else return 0U;
    *epoch_out = actuation_days_from_civil(y, (unsigned)mo, (unsigned)d) * 86400 + h * 3600 + mi * 60 + sec - offset * 60;
    return 1U;
}

static uint8_t actuation_expiry_status(cJSON *root, const char **status, const char **reason)
{
    cJSON *expires = cJSON_GetObjectItemCaseSensitive(root, "expiresAt");
    if (!expires) return 1U; /* Legacy commands without expiry remain supported. */
    if (!cJSON_IsString(expires) || !expires->valuestring) { *status = "FAILED"; *reason = "invalid_expiry"; return 0U; }
    int64_t expiry_epoch;
    rtc_datetime_t current;
    if (!actuation_parse_expiry(expires->valuestring, &expiry_epoch)) { *status = "FAILED"; *reason = "invalid_expiry"; return 0U; }
    if (!rtc_is_datetime_valid() || !rtc_get_datetime(&current)) { *status = "FAILED"; *reason = "device_clock_invalid"; return 0U; }
    int64_t now = actuation_days_from_civil(current.year, current.month, current.day) * 86400 + current.hour * 3600 + current.minute * 60 + current.second - (5 * 3600 + 30 * 60);
    if (now >= expiry_epoch) { *status = "EXPIRED"; *reason = "command_expired"; return 0U; }
    return 1U;
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
        publish_actuation_ack(id_str, "FAILED", "missing_port_or_action");
        return 1U;
    }

    /* Redelivered command already executed: confirm again, do not re-run.
     * Checked before expiry so a late redelivery of a command we did run is
     * not reported as EXPIRED. */
    if (act_recent_contains(id_str)) {
        print("[ACT] duplicate actuationId=%s ignored, re-acking\r\n", id_str);
        publish_actuation_ack(id_str, "ACKED", NULL);
        return 1U;
    }

    const char *expiry_status = "FAILED";
    const char *expiry_reason = "";
    if (!actuation_expiry_status(root, &expiry_status, &expiry_reason)) {
        publish_actuation_ack(id_str, expiry_status, expiry_reason);
        return 1U;
    }

    uint8_t ok = 0U;
    const char *fail_reason = "relay_action_failed";

    if (strcmp(action_str, "TURN_ON") == 0)
    {
        ok = app_relay_command(port_str, 1U, &fail_reason);
    }
    else if (strcmp(action_str, "TURN_OFF") == 0)
    {
        ok = app_relay_command(port_str, 0U, &fail_reason);
    }
    else if (strcmp(action_str, "TOGGLE") == 0)
    {
        uint8_t current = app_relay_get_by_key(port_str);
        ok = app_relay_command(port_str, current ? 0U : 1U, &fail_reason);
    }
    else
    {
        print("[ACT][ERR] unsupported action=%s\r\n", action_str);
        fail_reason = "unsupported_action";
        ok = 0U;
    }

    if (ok)
    {
        act_recent_remember(id_str);
        publish_actuation_ack(id_str, "ACKED", NULL);
    }
    else
    {
        publish_actuation_ack(id_str, "FAILED", fail_reason);
    }

    return 1U;
}

/* Fast path for relay commands (see modem_urc_set_fast_handler): runs from
 * inside the modem reader the moment the block ends, even in the middle of
 * a telemetry publish, so a relay no longer waits for that publish to
 * finish. Only actuation is taken here; it touches GPIO and queues its ack,
 * no modem I/O. Everything else (config, OTA, unparsable) goes to the queue
 * and the normal runtime path. */
static uint8_t mqtt_fast_downlink(const char *payload, size_t len)
{
    if (len == 0U || !strstr(payload, "\"actuation\"")) return 0U;
    if (s_ota_busy) return 0U;

    cJSON *root = cJSON_Parse(payload);
    if (!root) return 0U;

    cJSON *msg = cJSON_GetObjectItem(root, "type");
    const char *type_str = cJSON_IsString(msg) ? json_trimmed_str_or_empty(root, "type") : "";
    if (!json_type_equals(type_str, "actuation")) {
        cJSON_Delete(root);
        return 0U;
    }

    print("[ACT] command handled on arrival\r\n");
    handle_actuation_cjson(root);
    cJSON_Delete(root);
    return 1U;
}

void MQTT_InstallFastDownlinkHandler(void)
{
    modem_urc_set_fast_handler(mqtt_fast_downlink);
}

void app_cjson_arena_begin(void);
void app_cjson_arena_end(void);
size_t app_cjson_arena_peak(void);
static void handle_downlink_cjson_inner(const char *payload_str);

/* Every downlink is parsed in the dedicated cJSON arena (see main.c). */
void handle_downlink_cjson(const char *payload_str)
{
    app_cjson_arena_begin();
    handle_downlink_cjson_inner(payload_str);
    app_cjson_arena_end();
    print("[CJSON] downlink parse arena peak=%lu bytes\r\n", (unsigned long)app_cjson_arena_peak());
}

static void handle_downlink_cjson_inner(const char *payload_str)
{
    if (!payload_str || !payload_str[0]) return;

    const uint8_t *hash_input = NULL;
    size_t hash_input_len = 0U;
    char hash_calc[CONFIG_HASH_HEX_LEN + 1U] = {0};
    uint8_t hash_input_ok = config_json_span(payload_str,
                                             &hash_input,
                                             &hash_input_len);
    if (hash_input_ok) {
        sha256_hex(hash_input, hash_input_len, hash_calc);
    }

    app_log_push(APP_LOG_INFO,
                 "DOWNLINK",
                 "RECEIVED",
                 "downlink payload received",
                 0,
                 0);

    cJSON *root = cJSON_Parse(payload_str);
    if (!root) {
        const char *error_ptr = cJSON_GetErrorPtr();
        size_t payload_len = strlen(payload_str);
        size_t error_offset = error_ptr && error_ptr >= payload_str ?
                              (size_t)(error_ptr - payload_str) : payload_len;
        size_t dump_start = (error_offset > 8U) ? (error_offset - 8U) : 0U;
        size_t dump_end = error_offset + 8U;
        if (dump_end > payload_len) dump_end = payload_len;

        print("[CONFIG][ERR] downlink JSON parse failed strlen=%lu offset=%lu bytes=",
              (unsigned long)payload_len,
              (unsigned long)error_offset);
        for (size_t i = dump_start; i < dump_end; i++) {
            print("%02X", (unsigned int)(uint8_t)payload_str[i]);
        }
        print("\r\n");

        app_log_push(APP_LOG_ERROR,
                     "CONFIG",
                     "INVALID_JSON",
                     "config JSON parse failed",
                     0,
                     0);

        publish_config_status("unknown", "unknown", "error", "invalid_json_format");
        return;
    }

    cJSON *msg = cJSON_GetObjectItem(root, "type");
    const char *type_str = cJSON_IsString(msg) ? json_trimmed_str_or_empty(root, "type") : "";

    if (json_type_equals(type_str, "actuation"))
    {
        handle_actuation_cjson(root);
        cJSON_Delete(root);
        return;
    }

    if (json_type_equals(type_str, "ota") ||
        json_type_equals(type_str, "firmware_update"))
    {
        handle_ota_cjson(root);
        cJSON_Delete(root);
        return;
    } else if (json_type_equals(type_str, "ota_rollback")) {
        (void)handle_ota_rollback_cjson(root);
        cJSON_Delete(root);
        return;
    }

    if (!json_type_equals(type_str, "config")) {
        print("[DOWNLINK][ERR] unsupported type='%s'\r\n",
              type_str[0] ? type_str : "(missing/non-string)");

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

    /* The digest belongs to the envelope and covers only the inner config. */
    cJSON *hash = cJSON_GetObjectItem(root, "hash");

    const char *id_str = (cJSON_IsString(cfgId)) ? cfgId->valuestring : "missing";
    const char *deployment_str = cJSON_IsObject(cfg) ?
                                 json_str_or_empty(cfg, "deploymentId") : "";

    if (strcmp(id_str, "missing") == 0) {
        app_log_push(APP_LOG_ERROR,
                     "CONFIG",
                     "MISSING_CONFIG_ID",
                     "configId missing",
                     0,
                     0);

        publish_config_status("missing", deployment_str, "error", "missing_configId");
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

        publish_config_status(id_str, deployment_str, "error", "missing_config_object");
        cJSON_Delete(root);
        return;
    }

    if (cJSON_GetObjectItemCaseSensitive(cfg, "modbusSlaves") != NULL) {
        app_log_push(APP_LOG_ERROR,
                     "CONFIG",
                     "LEGACY_MODBUS_SCHEMA",
                     "top-level modbusSlaves is not supported",
                     0,
                     0);

        publish_config_status(id_str,
                              deployment_str,
                              "error",
                              "legacy_modbusSlaves_not_supported");
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

        publish_config_status(id_str, deployment_str, "error", "missing_hash");
        cJSON_Delete(root);
        return;
    }

    if (!config_hash_is_valid_hex(hash->valuestring)) {
        app_log_push(APP_LOG_ERROR,
                     "CONFIG",
                     "INVALID_HASH",
                     "config hash must be 64 lowercase hex characters",
                     0,
                     0);

        publish_config_status(id_str, deployment_str, "error", "invalid_hash_format");
        cJSON_Delete(root);
        return;
    }

    if (!hash_input_ok) {
        app_log_push(APP_LOG_ERROR,
                     "CONFIG",
                     "HASH_INPUT_TOO_LARGE",
                     "config bytes could not be isolated for hash validation",
                     0,
                     0);

        publish_config_status(id_str, deployment_str, "error", "config_hash_input_invalid");
        cJSON_Delete(root);
        return;
    }

    print("[CONFIG][HASH] innerLen=%lu expected=%s calculated=%s\r\n",
          (unsigned long)hash_input_len,
          hash->valuestring,
          hash_calc);

    if (strcmp(hash->valuestring, hash_calc) != 0) {
        print("[CONFIG][HASH][ERR] mismatch\r\n");
        config_hash_block_diag(hash_input, hash_input_len);
        app_log_push(APP_LOG_ERROR,
                     "CONFIG",
                     "HASH_MISMATCH",
                     "config hash mismatch",
                     0,
                     0);

        publish_config_status(id_str, deployment_str, "error", "hash_mismatch");
        cJSON_Delete(root);
        return;
    }

    print("[CONFIG] hash verified\r\n");

    modbus_cfg_t *work_cfg = modbus_cfg_workspace();
    memset(work_cfg, 0, sizeof(*work_cfg));

    if (!modbus_cfg_from_cjson(cfg, work_cfg)) {
        app_log_push(APP_LOG_ERROR,
                     "CONFIG",
                     "PARSE_FAILED",
                     "modbus config parse failed",
                     0,
                     0);

        publish_config_status(id_str, deployment_str, "error", modbus_cfg_parse_error());

        cJSON_Delete(root);
        return;
    }

    strncpy(work_cfg->hash, hash->valuestring, sizeof(work_cfg->hash) - 1U);
    work_cfg->hash[sizeof(work_cfg->hash) - 1U] = '\0';

    print("[CONFIG] parsed telemetryInterval=%lu ms sampleInterval=%lu ms points=%u\r\n",
          (unsigned long)work_cfg->telemetry_interval_ms,
          (unsigned long)work_cfg->sample_interval_ms,
          work_cfg->point_count);

    if (!modbus_cfg_save(work_cfg)) {
        app_log_critical(APP_LOG_ERROR,
                     "CONFIG",
                     "FLASH_SAVE_FAILED",
                     "modbus config flash save failed",
                     0,
                     0, "KEEP_PREVIOUS_CONFIG");

        publish_config_status(id_str, deployment_str, "error", "flash_write_error");

        cJSON_Delete(root);
        return;
    }

    memset(work_cfg, 0, sizeof(*work_cfg));

    if (!modbus_cfg_load(work_cfg)) {
        app_log_critical(APP_LOG_ERROR,
                     "CONFIG",
                     "FLASH_RELOAD_FAILED",
                     "saved config could not be reloaded",
                     0,
                     0, "KEEP_PREVIOUS_CONFIG");

        publish_config_status(id_str, deployment_str, "error", "flash_reload_error");

        cJSON_Delete(root);
        return;
    }

    if (strcmp(work_cfg->config_id, id_str) != 0) {
        app_log_critical(APP_LOG_ERROR,
                     "CONFIG",
                     "FLASH_VERIFY_FAILED",
                     "saved configId mismatch after reload",
                     0,
                     0, "KEEP_PREVIOUS_CONFIG");

        publish_config_status(id_str, deployment_str, "error", "flash_verify_error");

        cJSON_Delete(root);
        return;
    }

    /*
     * Apply only the copy reloaded from flash. This means APPLIED confirms
     * the device is running the durable config, not just the received payload.
     */
    app_debug_led_event(DEBUG_LED_EVENT_DOWNLINK);
    app_modbus_payload_set_cfg(work_cfg);
    debug_uart_set_enabled(modbus_cfg_debug_print_enabled(work_cfg));

    if (!app_modbus_payload_has_valid_config() ||
        strcmp(app_modbus_payload_get_config_id(), id_str) != 0) {
        app_log_critical(APP_LOG_ERROR,
                     "CONFIG",
                     "APPLY_VERIFY_FAILED",
                     "runtime config verify failed after apply",
                     0,
                     0, "KEEP_PREVIOUS_CONFIG");

        publish_config_status(id_str, deployment_str, "error", "apply_verify_error");

        cJSON_Delete(root);
        return;
    }

    print("[CONFIG] applied telemetryInterval=%lu ms\r\n",
          (unsigned long)app_modbus_payload_get_telemetry_interval_ms());

    app_log_critical(APP_LOG_INFO,
                 "CONFIG",
                 "APPLIED",
                 "config saved and applied",
                 0,
                 0, "NONE");

    publish_config_status(id_str, deployment_str, "APPLIED", NULL);

    cJSON_Delete(root);

    osDelay(2000);
//    print("CFG: Success. Rebooting...\r\n");
//    NVIC_SystemReset();
}
