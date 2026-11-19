#include "modem_handler.h"
#include "rtc_manager.h"
#include "main.h"
#include "config.h"
#include "debug_uart.h"
#include "uart2_rx_engine.h"
#include "cmsis_os.h" // Added for osDelay
#include "app_debug_led.h"
#include "app_log.h"
#include "app_watchdog.h"
#include "sha256.h"
#include "ota_root_ca.h"
#include "modem_urc.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdbool.h>

/* Externals */
extern uart2_rx_t g_u2rx;
extern UART_HandleTypeDef huart6;
#define MODEM_UART_HANDLE huart6

/* ================= AT DEBUG CAPTURE ================= */
static char              at_rx_buf[512];
static volatile uint16_t at_rx_len = 0;
static volatile uint8_t  at_rx_active = 0;

/* Global IMEI */
char imei[20];

/* ================= RX OWNERSHIP ================= */
uint8_t modem_at_rx_lock(void) { return at_rx_active; }
uint8_t modem_at_rx_is_active(void) { return at_rx_active; }


static osMutexId_t g_at_mutex;

void ModemAT_InitMutex(void)
{
    if (g_at_mutex == NULL) {
        g_at_mutex = osMutexNew(NULL);
    }
}

void modem_at_rx_feed(const uint8_t *data, uint16_t len)
{
    if (!at_rx_active) return;
    uint16_t room = (uint16_t)(sizeof(at_rx_buf) - 1U);
    uint16_t cur  = at_rx_len;
    if (cur >= room) return;
    uint16_t copy = len;
    if ((uint32_t)cur + copy > room) copy = (uint16_t)(room - cur);
    memcpy(&at_rx_buf[cur], data, copy);
    at_rx_len = (uint16_t)(cur + copy);
    at_rx_buf[at_rx_len] = 0;
}

/* ================= UART HELPERS ================= */
static void u2_send_cmd(const char *cmd)
{
    if (!cmd || !*cmd) return;
    HAL_UART_Transmit(&huart6, (uint8_t*)cmd, (uint16_t)strlen(cmd), 2000);
    if (!strchr(cmd, '\r') && !strchr(cmd, '\n')) {
        static const uint8_t crlf[2] = { '\r', '\n' };
        HAL_UART_Transmit(&huart6, (uint8_t*)crlf, 2, 2000);
    }
}

static void u2_flush_all(void)
{
    u2rx_drop_all(&g_u2rx);
    at_rx_len = 0;
    at_rx_buf[0] = 0;
}

/* Clear stale AT output before a command without losing unsolicited
 * messages: pending bytes go through the line hook (MQTT downlinks are
 * queued, disconnect notices latched) and only unclaimed lines are dropped.
 * The raw drop above stays for HTTP/OTA paths, where MQTT is stopped and the
 * buffer may hold binary data. */
static void u2_drain_pending(void)
{
    u2rx_drain(&g_u2rx);
    at_rx_len = 0;
    at_rx_buf[0] = 0;
}

void modem_rx_service(uint32_t wait_ms)
{
    /* Someone else is mid AT exchange: their reads feed the hook already. */
    if (g_at_mutex && osMutexAcquire(g_at_mutex, 0U) != osOK) return;

    u2rx_drain(&g_u2rx);
    if (wait_ms > 0U && !modem_urc_downlink_pending()) {
        uint32_t t0 = HAL_GetTick();
        while (!modem_urc_downlink_pending() && (HAL_GetTick() - t0) < wait_ms) {
            uint32_t left = wait_ms - (HAL_GetTick() - t0);
            if (g_u2rx.rxSem) (void)osSemaphoreAcquire(g_u2rx.rxSem, (left > 50U) ? 50U : left);
            else osDelay(1);
            u2rx_drain(&g_u2rx);
        }
    }

    if (g_at_mutex) osMutexRelease(g_at_mutex);
}

static void trim_crlf(char *s)
{
    if (!s) return;
    int n = (int)strlen(s);
    while (n > 0 && (s[n-1] == '\r' || s[n-1] == '\n')) {
        s[n-1] = 0;
        n--;
    }
}

static uint8_t modem_text_is_sensitive_or_payload(const char *text)
{
    const char *p = text;

    if (!text) {
        return 0U;
    }

    while (*p == ' ' || *p == '\t') {
        p++;
    }

    if (*p == '{') {
        return 1U;
    }

    if (strstr(text, "apiKey") ||
        strstr(text, "\"values\"") ||
        strstr(text, "\"type\":\"log\"") ||
        strstr(text, "\"type\":\"config\"") ||
        strstr(text, "\"type\":\"ota\""))
    {
        return 1U;
    }

    return 0U;
}

static void modem_print_text_or_summary(const char *prefix, const char *text)
{
    /* The broker connect command carries the shared MQTT username and password
     * (the same for every unit): print only up to the host and port
     * (2026-11-19, section 27; was printed in clear on USB and the SD log). */
    if (text && strstr(text, "CMQTTCONNECT=")) {
        const char *cut = strstr(text, "\",");
        int keep = cut ? (int)(cut - text) + 1 : 0;
        print("%s %.*s,<credentials hidden>\r\n", prefix ? prefix : "", keep, text);
        return;
    }
    if (modem_text_is_sensitive_or_payload(text)) {
        print("%s <payload hidden len=%lu>\r\n",
              prefix ? prefix : "",
              (unsigned long)strlen(text));
    } else {
        print("%s %s\r\n",
              prefix ? prefix : "",
              (text && text[0]) ? text : "(NULL/EMPTY)");
    }
}

/* ================= OTA HTTP DOWNLOAD ================= */
#define HTTP_OTA_CMD_MAX       2300U
#define HTTP_OTA_CHUNK_SIZE    512U
/*
 * A failed binary HTTPREAD can leave late payload bytes in the modem UART.
 * Retrying the same HTTPREAD inside that response risks corrupting the staged
 * image, so OTA retries happen by reopening a fresh HTTP range instead.
 */
#define HTTP_OTA_CHUNK_RETRIES        1U
#define HTTP_OTA_RANGE_OPEN_RETRIES   3U
#define HTTP_OTA_RANGE_STATUS_RETRIES 8U
#define HTTP_OTA_RANGE_READ_RETRIES   4U
#define HTTP_OTA_RANGE_WINDOW  32768U
#define HTTP_OTA_USE_RANGE_WINDOWS    1U
#define HTTP_OTA_SSL_CONTEXT_ID       0U
/* Private RSA root of the backend OTA download proxy. The modem cannot do
 * TLS with ECDSA-certificate servers (R2/Cloudflare), so OTA artifacts are
 * fetched through that RSA-only proxy and this root is pinned. A new file
 * name forces the modem to download the new root instead of reusing an old
 * file with the same name. */
#define HTTP_OTA_CA_FILE              "ota_dl_rsa_root.pem"
#define HTTP_OTA_ACTION_MS     120000UL
#define HTTP_OTA_READ_MS       30000UL
#define HTTP_OTA_TAIL_WAIT_MS  80UL
#define HTTP_OTA_TAIL_SLICE_MS 20UL
#define HTTP_OTA_ABORT_DRAIN_MS 1200UL
#define HTTP_OTA_ABORT_QUIET_MS 250UL
#define HTTP_OTA_DEBUG_PRINT   1U

#define HTTP_OTA_LOG(...) do { if (HTTP_OTA_DEBUG_PRINT) { print(__VA_ARGS__); } } while (0)

#define TCP_OTA_SOCKET_ID       1U
#define TCP_OTA_RANGE_SIZE      2048U
#define TCP_OTA_RX_READ_SIZE    512U
#define TCP_OTA_HEADER_MAX      1024U
#define TCP_OTA_CHUNK_RETRIES   6U
#define TCP_OTA_CONNECT_MS      45000UL
#define TCP_OTA_RESPONSE_MS     45000UL
#define TCP_OTA_MUTEX_WAIT_MS   10000UL
#define TCP_OTA_TOTAL_MAX_MS    1800000UL
#define TCP_OTA_DEBUG_PRINT     0U
#define TCP_OTA_DOUBLE_READ_VERIFY 1U

#define TCP_OTA_LOG(...) do { if (TCP_OTA_DEBUG_PRINT) { print(__VA_ARGS__); } } while (0)

#define AT_TRACE_LOG(...) do { if (debug_uart_at_is_enabled()) { print(__VA_ARGS__); } } while (0)
#define AT_TRACE_TEXT(prefix, text) do { if (debug_uart_at_is_enabled()) { modem_print_text_or_summary((prefix), (text)); } } while (0)

static char s_http_ota_cmd[HTTP_OTA_CMD_MAX];
static uint8_t s_ota_ca_ready;
static uint8_t s_ota_tls_clock_ready;
static char s_http_ota_ssl_context_detail[64];
static uint8_t s_http_ota_chunk[HTTP_OTA_CHUNK_SIZE];
static uint8_t s_tcp_ota_range_buf[TCP_OTA_RANGE_SIZE];
static uint8_t s_tcp_ota_verify_buf[TCP_OTA_RANGE_SIZE];
static uint8_t s_tcp_ota_rx_buf[TCP_OTA_RX_READ_SIZE];
static char s_tcp_ota_request[512];
static char s_tcp_ota_headers[TCP_OTA_HEADER_MAX];
static const char *s_http_ota_last_error = "none";
static char s_http_ota_last_error_buf[128];

static void modem_http_set_last_error(const char *reason)
{
    s_http_ota_last_error = (reason && reason[0]) ? reason : "unknown";
}

static void modem_http_set_last_error_status(const char *prefix, int status)
{
    snprintf(s_http_ota_last_error_buf,
             sizeof(s_http_ota_last_error_buf),
             "%s_%d",
             (prefix && prefix[0]) ? prefix : "status",
             status);
    s_http_ota_last_error = s_http_ota_last_error_buf;
}

static const char *modem_http_action_error_name(int status)
{
    switch (status) {
    case 600: return "http_non_pdu";
    case 601: return "http_network_error";
    case 602: return "http_no_memory";
    case 603: return "http_dns_error";
    case 604: return "http_stack_busy";
    case 701: return "tls_alert_state";
    case 702: return "tls_unknown_error";
    case 703: return "tls_busy";
    case 704: return "tls_connection_closed";
    case 705: return "tls_timeout";
    case 706: return "tls_data_transfer_failed";
    case 707: return "tls_memory_error";
    case 708: return "tls_invalid_parameter";
    case 709: return "tls_network_error";
    case 710:
        if (s_http_ota_ssl_context_detail[0]) {
            snprintf(s_http_ota_last_error_buf,
                     sizeof(s_http_ota_last_error_buf),
                     "tls_session_start_failed_%s",
                     s_http_ota_ssl_context_detail);
            return s_http_ota_last_error_buf;
        }
        return "tls_session_start_failed_ctx_readback_missing";
    case 711: return "tls_wrong_state";
    case 712: return "tls_socket_create_failed";
    case 713: return "tls_dns_failed";
    case 714: return "tls_socket_connect_failed";
    case 715: return "tls_handshake_failed";
    case 716: return "tls_socket_close_failed";
    case 717: return "tls_no_network";
    case 718: return "tls_send_timeout";
    case 719: return "tls_ca_missing";
    default: return NULL;
    }
}

const char *modem_http_download_last_error(void)
{
    return s_http_ota_last_error;
}

static void modem_http_kick_watchdog(void)
{
    app_watchdog_mark_task(APP_WATCHDOG_TASK_RUNTIME);
    app_debug_led_run_once();
    app_watchdog_refresh();
}

static uint8_t modem_http_mutex_acquire_with_watchdog(uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();

    if (!g_at_mutex) {
        return 1U;
    }

    while ((HAL_GetTick() - start) < timeout_ms) {
        modem_http_kick_watchdog();
        if (osMutexAcquire(g_at_mutex, 250U) == osOK) {
            return 1U;
        }
    }

    modem_http_set_last_error("tcp_mutex_timeout");
    return 0U;
}

static uint8_t modem_http_is_url_command(const char *cmd)
{
    static const char url_command[] = "AT+HTTPPARA=\"URL\"";
    return cmd && strncmp(cmd, url_command, sizeof(url_command) - 1U) == 0;
}

static void modem_http_log_command(const char *prefix, const char *cmd)
{
    if (modem_http_is_url_command(cmd)) {
        HTTP_OTA_LOG("%s AT+HTTPPARA=\"URL\",<redacted>\r\n", prefix);
        return;
    }
    HTTP_OTA_LOG("%s %s\r\n", prefix, cmd ? cmd : "");
}

static void modem_http_log_response(const char *line)
{
    if (line && (strstr(line, "https://") || strstr(line, "http://"))) {
        HTTP_OTA_LOG("[GSM->HTTP] URL-containing response redacted\r\n");
        return;
    }
    HTTP_OTA_LOG("[GSM->HTTP] %s\r\n", line ? line : "");
}

static void modem_http_capture_ssl_context(const char *cmd, const char *line)
{
    unsigned context_id = 0U;
    unsigned version = 0U;
    unsigned auth_mode = 0U;
    unsigned ignore_local_time = 0U;
    unsigned timeout = 0U;

    if (!cmd || !line || strncmp(cmd, "AT+CSSLCFG=", 11U) != 0 ||
        strchr(cmd, '\"') != NULL || !strstr(line, "+CSSLCFG:")) {
        return;
    }

    if (sscanf(line, "+CSSLCFG: %u,%u,%u,%u,%u",
               &context_id, &version, &auth_mode,
               &ignore_local_time, &timeout) == 5) {
        (void)snprintf(s_http_ota_ssl_context_detail,
                       sizeof(s_http_ota_ssl_context_detail),
                       "ctx%u_ver%u_auth%u_ignorelocaltime%u_timeout%u",
                       context_id, version, auth_mode,
                       ignore_local_time, timeout);
    } else {
        (void)snprintf(s_http_ota_ssl_context_detail,
                       sizeof(s_http_ota_ssl_context_detail),
                       "ctx_readback_unparsed");
    }
}

static uint8_t modem_http_send_wait_ok_locked(const char *cmd,
                                              const char *expected,
                                              uint32_t timeout_ms)
{
    char line[256];
    uint8_t expected_seen = (expected == NULL) ? 1U : 0U;
    uint32_t t0 = HAL_GetTick();

    if (!cmd || !cmd[0]) {
        return 0U;
    }

    modem_http_log_command("[HTTP->GSM]", cmd);
    u2_send_cmd(cmd);

    while ((HAL_GetTick() - t0) < timeout_ms) {
        modem_http_kick_watchdog();

        uint32_t elapsed = HAL_GetTick() - t0;
        uint32_t remain = (elapsed < timeout_ms) ? (timeout_ms - elapsed) : 0U;
        uint32_t slice = (remain > 250U) ? 250U : remain;
        int n = u2rx_read_line(&g_u2rx, line, (int)sizeof(line), slice);

        if (n <= 0) {
            osDelay(1);
            continue;
        }

        trim_crlf(line);
        if (!line[0]) {
            continue;
        }

        if (!strcmp(line, cmd)) {
            modem_http_log_command("[GSM->HTTP] command echo", cmd);
            continue;
        }
        modem_http_log_response(line);
        modem_http_capture_ssl_context(cmd, line);

        if (!expected_seen && expected && strstr(line, expected)) {
            expected_seen = 1U;
        }

        if (strstr(line, "ERROR") || strstr(line, "+CME ERROR")) {
            return 0U;
        }

        if (!strcmp(line, "OK")) {
            return expected_seen;
        }
    }

    if (modem_http_is_url_command(cmd)) {
        HTTP_OTA_LOG("[HTTP] timeout waiting OK for URL command (value redacted)\r\n");
    } else {
        HTTP_OTA_LOG("[HTTP] timeout waiting OK for %s\r\n", cmd);
    }
    return 0U;
}

static uint8_t modem_ca_file_exists_locked(const char *file, uint8_t *out_present)
{
    char line[192];
    uint32_t start = HAL_GetTick();
    uint8_t present = 0U;

    if (!out_present) return 0U;
    *out_present = 0U;
    u2_flush_all();
    u2_send_cmd("AT+CCERTLIST");

    while ((HAL_GetTick() - start) < 8000U) {
        modem_http_kick_watchdog();
        int n = u2rx_read_line(&g_u2rx, line, (int)sizeof(line), 250U);
        if (n <= 0) {
            osDelay(1U);
            continue;
        }

        trim_crlf(line);
        if (strstr(line, file)) present = 1U;
        if (strstr(line, "ERROR") || strstr(line, "+CME ERROR")) return 0U;
        if (!strcmp(line, "OK")) {
            *out_present = present;
            return 1U;
        }
    }

    return 0U;
}

static uint8_t modem_http_wait_ok_after_data_locked(uint32_t timeout_ms)
{
    char line[192];
    uint32_t start = HAL_GetTick();

    while ((HAL_GetTick() - start) < timeout_ms) {
        modem_http_kick_watchdog();
        int n = u2rx_read_line(&g_u2rx, line, (int)sizeof(line), 250U);
        if (n <= 0) {
            osDelay(1U);
            continue;
        }

        trim_crlf(line);
        if (strstr(line, "ERROR") || strstr(line, "+CME ERROR")) return 0U;
        if (!strcmp(line, "OK")) return 1U;
    }

    return 0U;
}

static uint8_t modem_http_modem_clock_is_valid_locked(void)
{
    char line[192];
    uint32_t start = HAL_GetTick();

    u2_flush_all();
    u2_send_cmd("AT+CCLK?");
    while ((HAL_GetTick() - start) < 5000U) {
        modem_http_kick_watchdog();
        int n = u2rx_read_line(&g_u2rx, line, (int)sizeof(line), 250U);
        if (n <= 0) {
            osDelay(1U);
            continue;
        }

        trim_crlf(line);
        unsigned year, month, day, hour, minute, second;
        if (sscanf(line, "+CCLK: \"%2u/%2u/%2u,%2u:%2u:%2u",
                   &year, &month, &day, &hour, &minute, &second) == 6) {
            return (year >= 20U && month >= 1U && month <= 12U &&
                    day >= 1U && day <= 31U && hour <= 23U &&
                    minute <= 59U && second <= 60U) ? 1U : 0U;
        }
        if (!strcmp(line, "ERROR") || strstr(line, "+CME ERROR")) return 0U;
    }
    return 0U;
}

static uint8_t modem_http_provision_ca_locked(void)
{
    char cmd[128];
    uint8_t present = 0U;
    if (!s_ota_tls_clock_ready) {
        if (!modem_http_send_wait_ok_locked("AT+CTZU=1", NULL, 5000U)) {
            modem_http_set_last_error("ota_modem_network_time_enable_failed");
            return 0U;
        }
        /* Use network-provided modem time. Never overwrite it with an MCU RTC
         * that could have been initialized from a build-time fallback. */
        uint8_t clock_valid = 0U;
        for (uint8_t attempt = 0U; attempt < 6U; attempt++) {
            if (modem_http_modem_clock_is_valid_locked()) {
                clock_valid = 1U;
                break;
            }
            osDelay(5000U);
        }
        if (!clock_valid) {
            modem_http_set_last_error("ota_modem_network_time_unavailable");
            return 0U;
        }
        s_ota_tls_clock_ready = 1U;
    }

    if (s_ota_ca_ready) return 1U;
    if (!modem_ca_file_exists_locked(HTTP_OTA_CA_FILE, &present)) {
        modem_http_set_last_error("ota_ca_list_failed");
        return 0U;
    }

    if (!present) {
        int n = snprintf(cmd, sizeof(cmd), "AT+CCERTDOWN=\"%s\",%u",
                         HTTP_OTA_CA_FILE, OTA_ROOT_CA_PEM_SIZE);
        if (n < 0 || (size_t)n >= sizeof(cmd)) {
            modem_http_set_last_error("ota_ca_command_invalid");
            return 0U;
        }

        u2_flush_all();
        HTTP_OTA_LOG("[HTTP->GSM] AT+CCERTDOWN file=%s bytes=%u\r\n",
                     HTTP_OTA_CA_FILE, OTA_ROOT_CA_PEM_SIZE);
        u2_send_cmd(cmd);
        if (!u2rx_wait_for(&g_u2rx, ">", 8000U)) {
            modem_http_set_last_error("ota_ca_download_prompt_timeout");
            return 0U;
        }
        if (HAL_UART_Transmit(&huart6,
                              (uint8_t *)ota_root_ca_pem,
                              (uint16_t)OTA_ROOT_CA_PEM_SIZE,
                              10000U) != HAL_OK ||
            !modem_http_wait_ok_after_data_locked(10000U)) {
            modem_http_set_last_error("ota_ca_download_failed");
            return 0U;
        }

        if (!modem_ca_file_exists_locked(HTTP_OTA_CA_FILE, &present) || !present) {
            modem_http_set_last_error("ota_ca_install_unverified");
            return 0U;
        }
    }

    s_ota_ca_ready = 1U;
    HTTP_OTA_LOG("[OTA][HTTP] trusted CA installed: %s\r\n", HTTP_OTA_CA_FILE);
    return 1U;
}

static void modem_http_drain_until_quiet_locked(uint32_t max_ms,
                                                uint32_t quiet_ms)
{
    uint8_t discard[64];
    uint32_t start = HAL_GetTick();
    uint32_t last_data = start;

    for (;;) {
        modem_http_kick_watchdog();

        size_t avail = u2rx_available(&g_u2rx);
        if (avail > 0U) {
            if (avail > sizeof(discard)) {
                avail = sizeof(discard);
            }
            (void)u2rx_read(&g_u2rx, discard, avail);
            last_data = HAL_GetTick();
            continue;
        }

        uint32_t now = HAL_GetTick();
        if ((now - start) >= max_ms || (now - last_data) >= quiet_ms) {
            break;
        }

        if (g_u2rx.rxSem) {
            (void)osSemaphoreAcquire(g_u2rx.rxSem, 20);
        } else {
            osDelay(5);
        }
    }
}

static void modem_http_abort_response_locked(void)
{
    (void)modem_http_send_wait_ok_locked("AT+HTTPTERM", NULL, 5000U);
    modem_http_drain_until_quiet_locked(HTTP_OTA_ABORT_DRAIN_MS,
                                        HTTP_OTA_ABORT_QUIET_MS);
    u2_flush_all();
}

static int modem_http_wait_action_locked(uint32_t timeout_ms,
                                         int *out_status,
                                         uint32_t *out_len)
{
    char line[160];
    uint32_t t0 = HAL_GetTick();

    while ((HAL_GetTick() - t0) < timeout_ms) {
        modem_http_kick_watchdog();

        uint32_t elapsed = HAL_GetTick() - t0;
        uint32_t remain = (elapsed < timeout_ms) ? (timeout_ms - elapsed) : 0U;
        uint32_t slice = (remain > 500U) ? 500U : remain;
        int n = u2rx_read_line(&g_u2rx, line, (int)sizeof(line), slice);

        if (n <= 0) {
            osDelay(1);
            continue;
        }

        trim_crlf(line);
        if (!line[0]) {
            continue;
        }

        modem_http_log_response(line);

        char *p = strstr(line, "+HTTPACTION:");
        if (p) {
            int method = -1;
            int status = 0;
            unsigned long len = 0;

            if (sscanf(p, "+HTTPACTION: %d,%d,%lu", &method, &status, &len) == 3 &&
                method == 0) {
                if (out_status) {
                    *out_status = status;
                }
                if (out_len) {
                    *out_len = (uint32_t)len;
                }
                if (status == 710) {
                    s_http_ota_ssl_context_detail[0] = '\0';
                    snprintf(s_http_ota_cmd, sizeof(s_http_ota_cmd),
                             "AT+CSSLCFG=%u", (unsigned)HTTP_OTA_SSL_CONTEXT_ID);
                    HTTP_OTA_LOG("[OTA][TLS] reading back SSL context after session-start failure\r\n");
                    if (!modem_http_send_wait_ok_locked(s_http_ota_cmd, NULL, 5000U)) {
                        HTTP_OTA_LOG("[OTA][TLS] SSL context readback failed\r\n");
                    }
                }
                return 1;
            }
        }

        if (strstr(line, "ERROR") || strstr(line, "+CME ERROR")) {
            modem_http_set_last_error("http_action_modem_error");
            return 0;
        }
    }

    HTTP_OTA_LOG("[HTTP] no HTTPACTION URC\r\n");
    modem_http_set_last_error("http_action_timeout");
    return 0;
}

static uint8_t modem_http_start_request_locked(const char *url,
                                               uint8_t use_range,
                                               uint32_t range_start,
                                               uint32_t range_len,
                                               int *out_status,
                                               uint32_t *out_len)
{
    modem_http_set_last_error("none");
    (void)modem_http_send_wait_ok_locked("AT+HTTPTERM", NULL, 5000U);

    if (!strncmp(url, "https://", 8U) && !modem_http_provision_ca_locked()) {
        HTTP_OTA_LOG("[OTA][HTTP] trusted CA provisioning failed\r\n");
        return 0U;
    }

    if (!modem_http_send_wait_ok_locked("AT+HTTPINIT", NULL, 10000U)) {
        modem_http_set_last_error("http_init_failed");
        HTTP_OTA_LOG("[OTA][HTTP] HTTPINIT failed\r\n");
        return 0U;
    }

    /* A76XX HTTPINIT activates the configured PDP context itself. Its
     * AT+HTTPPARA implementation has no CID parameter; PDP context 1 is
     * configured and activated by the modem bring-up path before OTA. */

    if (!strncmp(url, "https://", 8U)) {
        /* MQTT is fully stopped for this request. Reuse context 0 because the
         * SIM7670C HTTP client and MQTT service share the modem TLS resources;
         * MQTT reapplies its own context when it reconnects. Restrict TLS to
         * TLS 1.2 (3) and keep strict server certificate verification. HTTPS
         * is selected with the URL scheme plus HTTPPARA=SSLCFG; this modem
         * does not implement the unrelated AT+HTTPSSL command. */
        snprintf(s_http_ota_cmd, sizeof(s_http_ota_cmd),
                 "AT+CSSLCFG=\"sslversion\",%u,3",
                 (unsigned)HTTP_OTA_SSL_CONTEXT_ID);
        if (!modem_http_send_wait_ok_locked(s_http_ota_cmd, NULL, 5000U)) {
            modem_http_set_last_error("tls_version_config_failed");
            return 0U;
        }
        snprintf(s_http_ota_cmd, sizeof(s_http_ota_cmd),
                 "AT+CSSLCFG=\"authmode\",%u,1",
                 (unsigned)HTTP_OTA_SSL_CONTEXT_ID);
        if (!modem_http_send_wait_ok_locked(s_http_ota_cmd, NULL, 5000U)) {
            modem_http_set_last_error("tls_auth_mode_config_failed");
            return 0U;
        }
        snprintf(s_http_ota_cmd, sizeof(s_http_ota_cmd),
                 "AT+CSSLCFG=\"ignorelocaltime\",%u,0",
                 (unsigned)HTTP_OTA_SSL_CONTEXT_ID);
        if (!modem_http_send_wait_ok_locked(s_http_ota_cmd, NULL, 5000U)) {
            modem_http_set_last_error("tls_time_validation_config_failed");
            return 0U;
        }
        snprintf(s_http_ota_cmd, sizeof(s_http_ota_cmd),
                 "AT+CSSLCFG=\"enableSNI\",%u,1",
                 (unsigned)HTTP_OTA_SSL_CONTEXT_ID);
        if (!modem_http_send_wait_ok_locked(s_http_ota_cmd, NULL, 5000U)) {
            modem_http_set_last_error("tls_sni_config_failed");
            return 0U;
        }
        snprintf(s_http_ota_cmd, sizeof(s_http_ota_cmd),
                 "AT+CSSLCFG=\"cacert\",%u,\"%s\"",
                 (unsigned)HTTP_OTA_SSL_CONTEXT_ID, HTTP_OTA_CA_FILE);
        if (!modem_http_send_wait_ok_locked(s_http_ota_cmd, NULL, 5000U)) {
            modem_http_set_last_error("tls_ca_select_failed");
            HTTP_OTA_LOG("[OTA][HTTP] trusted CA selection failed\r\n");
            return 0U;
        }
        snprintf(s_http_ota_cmd, sizeof(s_http_ota_cmd),
                 "AT+HTTPPARA=\"SSLCFG\",%u", (unsigned)HTTP_OTA_SSL_CONTEXT_ID);
        if (!modem_http_send_wait_ok_locked(s_http_ota_cmd, NULL, 5000U)) {
            modem_http_set_last_error("tls_context_select_failed");
            HTTP_OTA_LOG("[OTA][HTTP] SSL context selection failed\r\n");
            return 0U;
        }
    }

    snprintf(s_http_ota_cmd,
             sizeof(s_http_ota_cmd),
             "AT+HTTPPARA=\"URL\",\"%s\"",
             url);

    if (!modem_http_send_wait_ok_locked(s_http_ota_cmd, NULL, 15000U)) {
        modem_http_set_last_error("http_url_set_failed");
        HTTP_OTA_LOG("[OTA][HTTP] set URL failed\r\n");
        return 0U;
    }

    if (use_range) {
        uint32_t range_end = range_start + range_len - 1U;
        snprintf(s_http_ota_cmd,
                 sizeof(s_http_ota_cmd),
                 "AT+HTTPPARA=\"USERDATA\",\"Range: bytes=%lu-%lu\"",
                 (unsigned long)range_start,
                 (unsigned long)range_end);

        if (!modem_http_send_wait_ok_locked(s_http_ota_cmd, NULL, 5000U)) {
            modem_http_set_last_error("http_range_header_set_failed");
            HTTP_OTA_LOG("[OTA][HTTP] set Range header failed\r\n");
            return 0U;
        }
    }

    u2_flush_all();
    if (!modem_http_send_wait_ok_locked("AT+HTTPACTION=0", NULL, 5000U)) {
        modem_http_set_last_error("http_action_command_failed");
        HTTP_OTA_LOG("[OTA][HTTP] HTTPACTION command failed\r\n");
        return 0U;
    }

    if (!modem_http_wait_action_locked(HTTP_OTA_ACTION_MS,
                                       out_status,
                                       out_len)) {
        if (!strcmp(modem_http_download_last_error(), "none")) {
            modem_http_set_last_error("http_action_failed");
        }
        return 0U;
    }

    HTTP_OTA_LOG("[OTA][HTTP] action status=%d len=%lu%s\r\n",
                 out_status ? *out_status : 0,
                 (unsigned long)(out_len ? *out_len : 0U),
                 use_range ? " range" : "");

    if (out_status) {
        const char *action_error = modem_http_action_error_name(*out_status);
        if (action_error) {
            HTTP_OTA_LOG("[OTA][HTTP] modem transport result=%s code=%d\r\n",
                         action_error, *out_status);
        }
    }

    return 1U;
}

static size_t modem_http_read_exact_locked(uint8_t *buf,
                                           size_t len,
                                           uint32_t timeout_ms)
{
    size_t done = 0U;
    uint32_t t0 = HAL_GetTick();

    while (done < len && (HAL_GetTick() - t0) < timeout_ms) {
        modem_http_kick_watchdog();

        size_t avail = u2rx_available(&g_u2rx);
        if (avail == 0U) {
            if (g_u2rx.rxSem) {
                (void)osSemaphoreAcquire(g_u2rx.rxSem, 50);
            } else {
                osDelay(1);
            }
            continue;
        }

        size_t want = len - done;
        if (want > avail) {
            want = avail;
        }

        done += u2rx_read(&g_u2rx, &buf[done], want);
    }

    return done;
}

static int modem_http_read_chunk_locked(uint32_t offset,
                                        uint32_t want,
                                        uint8_t *buf,
                                        uint32_t buf_size,
                                        uint32_t timeout_ms)
{
    char line[128];
    uint32_t t0 = HAL_GetTick();

    if (!buf || want == 0U || want > buf_size) {
        return -1;
    }

    snprintf(s_http_ota_cmd,
             sizeof(s_http_ota_cmd),
             "AT+HTTPREAD=%lu,%lu",
             (unsigned long)offset,
             (unsigned long)want);

    /* Anything still buffered here belongs to an earlier response. Reading
     * it as part of this chunk is how a binary payload got misframed; drop it
     * and say so. */
    if (u2rx_available(&g_u2rx) > 0U) {
        HTTP_OTA_LOG("[HTTP] dropping %lu stale bytes before read offset=%lu\r\n",
                     (unsigned long)u2rx_available(&g_u2rx),
                     (unsigned long)offset);
        u2_flush_all();
    }

    HTTP_OTA_LOG("[HTTP->GSM] %s\r\n", s_http_ota_cmd);
    u2_send_cmd(s_http_ota_cmd);

    while ((HAL_GetTick() - t0) < timeout_ms) {
        modem_http_kick_watchdog();

        uint32_t elapsed = HAL_GetTick() - t0;
        uint32_t remain = (elapsed < timeout_ms) ? (timeout_ms - elapsed) : 0U;
        uint32_t slice = (remain > 250U) ? 250U : remain;
        int n = u2rx_read_line(&g_u2rx, line, (int)sizeof(line), slice);

        if (n <= 0) {
            osDelay(1);
            continue;
        }

        trim_crlf(line);
        if (!line[0]) {
            continue;
        }

        modem_http_log_response(line);

        if (!strcmp(line, s_http_ota_cmd)) {
            continue;
        }

        /*
         * Some SIMCom firmwares acknowledge AT+HTTPREAD with OK first, then
         * emit +HTTPREAD:<len> and the data. The payload header is what starts
         * the binary read, so a leading OK is not a completion condition here.
         */
        if (!strcmp(line, "OK")) {
            continue;
        }

        char *p = strstr(line, "+HTTPREAD:");
        if (p) {
            unsigned long body_len = 0;

            if (sscanf(p, "+HTTPREAD: DATA,%lu", &body_len) != 1 &&
                sscanf(p, "+HTTPREAD: %lu", &body_len) != 1) {
                return -1;
            }

            if (body_len > buf_size) {
                return -1;
            }

            if (body_len == 0UL) {
                continue;
            }

            size_t got = modem_http_read_exact_locked(buf,
                                                      (size_t)body_len,
                                                      timeout_ms);
            if (got != (size_t)body_len) {
                HTTP_OTA_LOG("[HTTP] chunk body timeout got=%lu want=%lu\r\n",
                             (unsigned long)got,
                             body_len);
                return -1;
            }

            uint32_t ok_start = HAL_GetTick();
            while ((HAL_GetTick() - ok_start) < HTTP_OTA_TAIL_WAIT_MS) {
                modem_http_kick_watchdog();
                int ok_n = u2rx_read_line(&g_u2rx,
                                          line,
                                          (int)sizeof(line),
                                          HTTP_OTA_TAIL_SLICE_MS);
                if (ok_n <= 0) {
                    osDelay(1);
                    continue;
                }

                trim_crlf(line);
                if (!line[0]) {
                    continue;
                }

                modem_http_log_response(line);

                if (!strcmp(line, "OK")) {
                    return (int)body_len;
                }

                /*
                 * Observed module response after binary payload:
                 *   OK
                 *   +HTTPREAD: <len>
                 *   <binary>
                 *   +HTTPREAD: 0
                 * Treat the zero-length marker as successful chunk end.
                 */
                if (strstr(line, "+HTTPREAD:")) {
                    unsigned long tail_len = 1UL;
                    if ((sscanf(line, "+HTTPREAD: DATA,%lu", &tail_len) == 1 ||
                         sscanf(line, "+HTTPREAD: %lu", &tail_len) == 1) &&
                        tail_len == 0UL) {
                        return (int)body_len;
                    }
                }

                /* Anything else right after the payload means the byte
                 * count and the stream disagree: the chunk is misframed. */
                HTTP_OTA_LOG("[HTTP] unexpected line after chunk offset=%lu; rejecting\r\n",
                             (unsigned long)offset);
                return -1;
            }

            HTTP_OTA_LOG("[HTTP] chunk complete without tail marker\r\n");
            return (int)body_len;
        }

        if (strstr(line, "ERROR") || strstr(line, "+CME ERROR")) {
            return -1;
        }

        /* Before the +HTTPREAD header only the echo, OK and a stale zero
         * marker are legitimate. Any other line (typically binary payload
         * read as text) means the stream is out of step with this request. */
        HTTP_OTA_LOG("[HTTP] unexpected line before chunk header offset=%lu; rejecting\r\n",
                     (unsigned long)offset);
        return -1;
    }

    HTTP_OTA_LOG("[HTTP] chunk header timeout offset=%lu\r\n", (unsigned long)offset);
    return -1;
}

static uint8_t modem_http_sha_matches(const char *expected, const char actual[65])
{
    if (!expected || !expected[0]) {
        return 1U;
    }

    if (strlen(expected) != 64U) {
        return 0U;
    }

    for (uint8_t i = 0; i < 64U; i++) {
        char a = (char)tolower((unsigned char)expected[i]);
        char b = (char)tolower((unsigned char)actual[i]);
        if (a != b) {
            return 0U;
        }
    }

    return 1U;
}

static void modem_send_raw_locked(const uint8_t *data, uint32_t len)
{
    if (!data || len == 0U) {
        return;
    }

    (void)HAL_UART_Transmit(&huart6, (uint8_t *)data, (uint16_t)len, 5000U);
}

static uint8_t modem_tcp_wait_line_contains_locked(const char *needle,
                                                   uint32_t timeout_ms,
                                                   char *matched,
                                                   uint32_t matched_len)
{
    char line[192];
    uint32_t start = HAL_GetTick();

    while ((HAL_GetTick() - start) < timeout_ms) {
        modem_http_kick_watchdog();

        uint32_t elapsed = HAL_GetTick() - start;
        uint32_t remain = (elapsed < timeout_ms) ? (timeout_ms - elapsed) : 0U;
        uint32_t slice = (remain > 500U) ? 500U : remain;
        int n = u2rx_read_line(&g_u2rx, line, (int)sizeof(line), slice);

        if (n <= 0) {
            osDelay(1);
            continue;
        }

        trim_crlf(line);
        if (!line[0]) {
            continue;
        }

        TCP_OTA_LOG("[TCP<-GSM] %s\r\n", line);

        if (!needle || strstr(line, needle)) {
            if (matched && matched_len > 0U) {
                strncpy(matched, line, matched_len - 1U);
                matched[matched_len - 1U] = '\0';
            }
            return 1U;
        }

        if (strstr(line, "ERROR") || strstr(line, "+CME ERROR")) {
            if (matched && matched_len > 0U) {
                strncpy(matched, line, matched_len - 1U);
                matched[matched_len - 1U] = '\0';
            }
            return 0U;
        }
    }

    return 0U;
}

static uint8_t modem_tcp_read_full_line_locked(char *out,
                                               uint32_t out_len,
                                               uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    uint32_t idx = 0U;

    if (!out || out_len < 2U) {
        return 0U;
    }

    out[0] = '\0';

    while ((HAL_GetTick() - start) < timeout_ms) {
        modem_http_kick_watchdog();

        if (u2rx_available(&g_u2rx) == 0U) {
            if (g_u2rx.rxSem) {
                (void)osSemaphoreAcquire(g_u2rx.rxSem, 50U);
            } else {
                osDelay(1U);
            }
            continue;
        }

        uint8_t ch = 0U;
        if (u2rx_read(&g_u2rx, &ch, 1U) == 0U) {
            osDelay(1U);
            continue;
        }

        if (ch == '\r') {
            continue;
        }

        if (ch == '\n') {
            if (idx == 0U) {
                continue;
            }
            out[idx] = '\0';
            TCP_OTA_LOG("[TCP<-GSM] %s\r\n", out);
            return 1U;
        }

        if (idx < (out_len - 1U)) {
            out[idx++] = (char)ch;
        } else {
            out[out_len - 1U] = '\0';
            modem_http_set_last_error("tcp_line_too_long");
            return 0U;
        }
    }

    return 0U;
}

static uint8_t modem_tcp_send_wait_ok_locked(const char *cmd, uint32_t timeout_ms)
{
    if (!cmd || !cmd[0]) {
        return 0U;
    }

    TCP_OTA_LOG("[TCP->GSM] %s\r\n", cmd);
    u2_send_cmd(cmd);
    return modem_tcp_wait_line_contains_locked("OK", timeout_ms, NULL, 0U);
}

static void modem_tcp_close_locked(void)
{
    snprintf(s_http_ota_cmd,
             sizeof(s_http_ota_cmd),
             "AT+CIPCLOSE=%u",
             (unsigned int)TCP_OTA_SOCKET_ID);
    (void)modem_tcp_send_wait_ok_locked(s_http_ota_cmd, 5000U);
    modem_http_drain_until_quiet_locked(1000U, 150U);
    u2_flush_all();
}

static uint8_t modem_tcp_parse_url(const char *url,
                                   char *host,
                                   uint32_t host_len,
                                   const char **path)
{
    const char *p = NULL;
    const char *slash = NULL;
    size_t len = 0U;

    if (!url || strncmp(url, "http://", 7U) != 0 || !host || host_len == 0U || !path) {
        return 0U;
    }

    p = url + 7U;
    slash = strchr(p, '/');
    if (!slash || slash == p) {
        return 0U;
    }

    len = (size_t)(slash - p);
    if (len >= host_len) {
        return 0U;
    }

    memcpy(host, p, len);
    host[len] = '\0';
    *path = slash;
    return 1U;
}

static uint8_t modem_tcp_open_locked(const char *host, uint16_t port)
{
    char line[192];

    (void)modem_tcp_send_wait_ok_locked("AT+NETOPEN", 10000U);
    (void)modem_tcp_wait_line_contains_locked("+NETOPEN:", 10000U, NULL, 0U);
    (void)modem_tcp_send_wait_ok_locked("AT+CIPRXGET=1", 5000U);
    modem_tcp_close_locked();

    snprintf(s_http_ota_cmd,
             sizeof(s_http_ota_cmd),
             "AT+CIPOPEN=%u,\"TCP\",\"%s\",%u",
             (unsigned int)TCP_OTA_SOCKET_ID,
             host,
             (unsigned int)port);

    TCP_OTA_LOG("[TCP->GSM] %s\r\n", s_http_ota_cmd);
    u2_send_cmd(s_http_ota_cmd);

    if (!modem_tcp_wait_line_contains_locked("OK", 10000U, NULL, 0U)) {
        return 0U;
    }

    if (!modem_tcp_wait_line_contains_locked("+CIPOPEN:", TCP_OTA_CONNECT_MS, line, sizeof(line))) {
        modem_http_set_last_error("tcp_open_timeout");
        return 0U;
    }

    if (strstr(line, "+CIPOPEN:") && strstr(line, ",0")) {
        return 1U;
    }

    modem_http_set_last_error("tcp_open_failed");
    return 0U;
}

static uint8_t modem_tcp_send_http_request_locked(const char *request, uint32_t request_len)
{
    if (!request || request_len == 0U) {
        return 0U;
    }

    snprintf(s_http_ota_cmd,
             sizeof(s_http_ota_cmd),
             "AT+CIPSEND=%u,%lu",
             (unsigned int)TCP_OTA_SOCKET_ID,
             (unsigned long)request_len);

    TCP_OTA_LOG("[TCP->GSM] %s\r\n", s_http_ota_cmd);
    u2_send_cmd(s_http_ota_cmd);

    if (!modem_tcp_wait_line_contains_locked(">", 10000U, NULL, 0U)) {
        modem_http_set_last_error("tcp_send_prompt_timeout");
        return 0U;
    }

    modem_send_raw_locked((const uint8_t *)request, request_len);

    if (!modem_tcp_wait_line_contains_locked("OK", 15000U, NULL, 0U)) {
        modem_http_set_last_error("tcp_send_failed");
        return 0U;
    }

    return 1U;
}

static int modem_tcp_available_locked(void)
{
    char line[128];
    unsigned int link = 0U;
    unsigned long len = 0UL;
    uint32_t start = HAL_GetTick();

    snprintf(s_http_ota_cmd,
             sizeof(s_http_ota_cmd),
             "AT+CIPRXGET=4,%u",
             (unsigned int)TCP_OTA_SOCKET_ID);

    u2_send_cmd(s_http_ota_cmd);

    while ((HAL_GetTick() - start) < 5000U) {
        if (!modem_tcp_read_full_line_locked(line, sizeof(line), 500U)) {
            continue;
        }

        if (sscanf(line, "+CIPRXGET: 4,%u,%lu", &link, &len) == 2 &&
            link == TCP_OTA_SOCKET_ID) {
            (void)modem_tcp_wait_line_contains_locked("OK", 1000U, NULL, 0U);
            return (int)len;
        }
    }

    return -1;
}

static int modem_tcp_read_locked(uint8_t *dst, uint32_t want)
{
    char line[128];
    unsigned int mode = 0U;
    unsigned int link = 0U;
    unsigned long got = 0UL;
    unsigned long remain = 0UL;
    uint32_t start = HAL_GetTick();

    if (!dst || want == 0U || want > TCP_OTA_RX_READ_SIZE) {
        return -1;
    }

    snprintf(s_http_ota_cmd,
             sizeof(s_http_ota_cmd),
             "AT+CIPRXGET=2,%u,%lu",
             (unsigned int)TCP_OTA_SOCKET_ID,
             (unsigned long)want);

    u2_send_cmd(s_http_ota_cmd);

    for (;;) {
        if ((HAL_GetTick() - start) >= 5000U) {
            return -1;
        }

        if (!modem_tcp_read_full_line_locked(line, sizeof(line), 500U)) {
            continue;
        }

        if (sscanf(line, "+CIPRXGET: %u,%u,%lu,%lu", &mode, &link, &got, &remain) == 4 &&
            mode == 2U &&
            link == TCP_OTA_SOCKET_ID) {
            break;
        }
    }

    if (got > want) {
        return -1;
    }

    if (got == 0UL) {
        (void)modem_tcp_wait_line_contains_locked("OK", 1000U, NULL, 0U);
        return 0;
    }

    size_t read = modem_http_read_exact_locked(dst, (size_t)got, 10000U);
    (void)modem_tcp_wait_line_contains_locked("OK", 3000U, NULL, 0U);

    return (read == (size_t)got) ? (int)got : -1;
}

static const char *modem_tcp_find_header_value(const char *headers, const char *key)
{
    size_t key_len = strlen(key);
    const char *p = headers;

    while (p && *p) {
        if ((p == headers || p[-1] == '\n') &&
            strncmp(p, key, key_len) == 0) {
            p += key_len;
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            return p;
        }

        p = strchr(p, '\n');
        if (p) {
            p++;
        }
    }

    return NULL;
}

static uint8_t modem_tcp_http_download_range_locked(const char *host,
                                                    const char *path,
                                                    uint32_t start,
                                                    uint32_t len,
                                                    uint32_t total_size,
                                                    uint8_t *out,
                                                    uint32_t *out_len)
{
    uint32_t header_len = 0U;
    uint8_t header_done = 0U;
    uint32_t body_len = 0U;
    uint32_t deadline = HAL_GetTick() + TCP_OTA_RESPONSE_MS;
    int http_status = 0;
    int content_len = -1;
    unsigned long cr_start = 0UL;
    unsigned long cr_end = 0UL;
    unsigned long cr_total = 0UL;

    if (!host || !path || !out || !out_len || len == 0U || len > TCP_OTA_RANGE_SIZE) {
        return 0U;
    }

    *out_len = 0U;
    memset(s_tcp_ota_headers, 0, sizeof(s_tcp_ota_headers));

    if (!modem_tcp_open_locked(host, 80U)) {
        return 0U;
    }

    snprintf(s_tcp_ota_request,
             sizeof(s_tcp_ota_request),
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "Range: bytes=%lu-%lu\r\n"
             "Connection: close\r\n"
             "\r\n",
             path,
             host,
             (unsigned long)start,
             (unsigned long)(start + len - 1U));

    if (!modem_tcp_send_http_request_locked(s_tcp_ota_request,
                                            (uint32_t)strlen(s_tcp_ota_request))) {
        modem_tcp_close_locked();
        return 0U;
    }

    while ((int32_t)(deadline - HAL_GetTick()) > 0 && body_len < len) {
        modem_http_kick_watchdog();

        int avail = modem_tcp_available_locked();
        if (avail <= 0) {
            (void)modem_tcp_wait_line_contains_locked("+CIPRXGET:", 500U, NULL, 0U);
            osDelay(20U);
            continue;
        }

        uint32_t want = (uint32_t)avail;
        if (want > TCP_OTA_RX_READ_SIZE) {
            want = TCP_OTA_RX_READ_SIZE;
        }

        int got = modem_tcp_read_locked(s_tcp_ota_rx_buf, want);
        if (got <= 0) {
            osDelay(20U);
            continue;
        }

        uint32_t pos = 0U;
        while (pos < (uint32_t)got) {
            if (!header_done) {
                if (header_len >= (sizeof(s_tcp_ota_headers) - 1U)) {
                    modem_http_set_last_error("tcp_http_header_too_large");
                    modem_tcp_close_locked();
                    return 0U;
                }

                s_tcp_ota_headers[header_len++] = (char)s_tcp_ota_rx_buf[pos++];
                s_tcp_ota_headers[header_len] = '\0';

                if (header_len >= 4U &&
                    s_tcp_ota_headers[header_len - 4U] == '\r' &&
                    s_tcp_ota_headers[header_len - 3U] == '\n' &&
                    s_tcp_ota_headers[header_len - 2U] == '\r' &&
                    s_tcp_ota_headers[header_len - 1U] == '\n') {
                    const char *cl = NULL;
                    const char *cr = NULL;
                    header_done = 1U;

                    if (sscanf(s_tcp_ota_headers, "HTTP/%*s %d", &http_status) != 1 ||
                        http_status != 206) {
                        modem_http_set_last_error_status("tcp_http_status", http_status);
                        modem_tcp_close_locked();
                        return 0U;
                    }

                    cl = modem_tcp_find_header_value(s_tcp_ota_headers, "Content-Length:");
                    if (!cl || sscanf(cl, "%d", &content_len) != 1 ||
                        content_len != (int)len) {
                        modem_http_set_last_error("tcp_http_len_mismatch");
                        modem_tcp_close_locked();
                        return 0U;
                    }

                    cr = modem_tcp_find_header_value(s_tcp_ota_headers, "Content-Range:");
                    if (!cr ||
                        sscanf(cr, "bytes %lu-%lu/%lu", &cr_start, &cr_end, &cr_total) != 3 ||
                        cr_start != (unsigned long)start ||
                        cr_end != (unsigned long)(start + len - 1U) ||
                        cr_total != (unsigned long)total_size) {
                        modem_http_set_last_error("tcp_content_range_mismatch");
                        modem_tcp_close_locked();
                        return 0U;
                    }
                }
            } else {
                uint32_t copy = (uint32_t)got - pos;
                if (copy > (len - body_len)) {
                    copy = len - body_len;
                }

                memcpy(&out[body_len], &s_tcp_ota_rx_buf[pos], copy);
                body_len += copy;
                pos += copy;
            }
        }
    }

    modem_tcp_close_locked();

    if (body_len != len) {
        print("[OTA][TCP] body timeout start=%lu got=%lu want=%lu hdr=%lu status=%d clen=%d\r\n",
              (unsigned long)start,
              (unsigned long)body_len,
              (unsigned long)len,
              (unsigned long)header_len,
              http_status,
              content_len);
        modem_http_set_last_error("tcp_body_timeout");
        return 0U;
    }

    *out_len = body_len;
    return 1U;
}

uint8_t modem_tcp_http_download_to_w25q(const char *url,
                                        W25Q_Handle *flash,
                                        uint32_t base_addr,
                                        uint32_t max_size,
                                        uint32_t expected_size,
                                        const char *expected_sha256,
                                        uint32_t *out_size,
                                        char out_sha256[65],
                                        modem_http_download_progress_cb_t progress_cb,
                                        void *progress_ctx)
{
    char host[96];
    const char *path = NULL;
    uint32_t written = 0U;
    uint32_t ota_start = HAL_GetTick();
    uint8_t ok = 0U;
    uint8_t mutex_acquired = 0U;
    sha256_ctx_t sha;
    char calc_sha256[65] = {0};

    if (out_size) {
        *out_size = 0U;
    }
    if (out_sha256) {
        out_sha256[0] = '\0';
    }

    modem_http_set_last_error("none");

    if (!url || !flash || expected_size == 0U || expected_size > max_size) {
        modem_http_set_last_error("tcp_invalid_args");
        return 0U;
    }

    if (!modem_tcp_parse_url(url, host, sizeof(host), &path)) {
        modem_http_set_last_error("tcp_url_not_supported");
        return 0U;
    }

    if (!modem_http_mutex_acquire_with_watchdog(TCP_OTA_MUTEX_WAIT_MS)) {
        return 0U;
    }
    mutex_acquired = (g_at_mutex != NULL) ? 1U : 0U;

    at_rx_active = 1U;
    u2_flush_all();

    uint32_t erase_len = ((expected_size + W25Q_SECTOR_SIZE - 1U) /
                          W25Q_SECTOR_SIZE) * W25Q_SECTOR_SIZE;

    print("[OTA][TCP] start host=%s size=%lu chunk=%lu\r\n",
          host,
          (unsigned long)expected_size,
          (unsigned long)TCP_OTA_RANGE_SIZE);

    for (uint32_t off = 0U; off < erase_len; off += W25Q_SECTOR_SIZE) {
        modem_http_kick_watchdog();
        if ((HAL_GetTick() - ota_start) >= TCP_OTA_TOTAL_MAX_MS) {
            modem_http_set_last_error("tcp_total_timeout");
            goto done;
        }
        if (!w25q_erase_sector_4k(flash, base_addr + off)) {
            modem_http_set_last_error("tcp_flash_erase_failed");
            goto done;
        }
    }

    sha256_init(&sha);

    while (written < expected_size) {
        if ((HAL_GetTick() - ota_start) >= TCP_OTA_TOTAL_MAX_MS) {
            modem_http_set_last_error("tcp_total_timeout");
            goto done;
        }

        uint32_t range_len = expected_size - written;
        uint32_t got_len = 0U;
        uint8_t range_ok = 0U;

        if (range_len > TCP_OTA_RANGE_SIZE) {
            range_len = TCP_OTA_RANGE_SIZE;
        }

        for (uint8_t attempt = 1U; attempt <= TCP_OTA_CHUNK_RETRIES; attempt++) {
            modem_http_kick_watchdog();
            if ((HAL_GetTick() - ota_start) >= TCP_OTA_TOTAL_MAX_MS) {
                modem_http_set_last_error("tcp_total_timeout");
                goto done;
            }

            print("[OTA][TCP] range %lu-%lu attempt=%u\r\n",
                  (unsigned long)written,
                  (unsigned long)(written + range_len - 1U),
                  (unsigned int)attempt);

            if (modem_tcp_http_download_range_locked(host,
                                                     path,
                                                     written,
                                                     range_len,
                                                     expected_size,
                                                     s_tcp_ota_range_buf,
                                                     &got_len) &&
                got_len == range_len) {
#if TCP_OTA_DOUBLE_READ_VERIFY
                uint32_t verify_len = 0U;
                if (!modem_tcp_http_download_range_locked(host,
                                                          path,
                                                          written,
                                                          range_len,
                                                          expected_size,
                                                          s_tcp_ota_verify_buf,
                                                          &verify_len) ||
                    verify_len != range_len ||
                    memcmp(s_tcp_ota_range_buf, s_tcp_ota_verify_buf, range_len) != 0) {
                    modem_http_set_last_error("tcp_range_compare_mismatch");
                    print("[OTA][TCP] range verify mismatch start=%lu len=%lu\r\n",
                          (unsigned long)written,
                          (unsigned long)range_len);
                    modem_tcp_close_locked();
                    osDelay(300U);
                    continue;
                }
#endif
                range_ok = 1U;
                break;
            }

            print("[OTA][TCP] range retry reason=%s\r\n",
                  modem_http_download_last_error());
            modem_tcp_close_locked();
            osDelay(500U * attempt);
        }

        if (!range_ok) {
            goto done;
        }

        if (!w25q_write(flash,
                        base_addr + written,
                        s_tcp_ota_range_buf,
                        range_len)) {
            modem_http_set_last_error("tcp_flash_write_failed");
            goto done;
        }

        sha256_update(&sha, s_tcp_ota_range_buf, range_len);
        written += range_len;

        if (progress_cb) {
            progress_cb(written, expected_size, progress_ctx);
        }
    }

    sha256_final_hex(&sha, calc_sha256);
    if (out_sha256) {
        strncpy(out_sha256, calc_sha256, 65U);
        out_sha256[64] = '\0';
    }

    print("[OTA][TCP] verify size=%lu sha=%s\r\n",
          (unsigned long)written,
          calc_sha256);

    if (!modem_http_sha_matches(expected_sha256, calc_sha256)) {
        modem_http_set_last_error("tcp_sha_mismatch");
        goto done;
    }

    if (out_size) {
        *out_size = written;
    }

    modem_http_set_last_error("none");
    ok = 1U;

done:
    modem_tcp_close_locked();
    at_rx_active = 0U;
    if (mutex_acquired) {
        osMutexRelease(g_at_mutex);
    }

    print("[OTA][TCP] %s written=%lu reason=%s\r\n",
          ok ? "OK" : "FAIL",
          (unsigned long)written,
          modem_http_download_last_error());

    return ok;
}

uint8_t modem_http_download_to_w25q(const char *url,
                                    W25Q_Handle *flash,
                                    uint32_t base_addr,
                                    uint32_t max_size,
                                    uint32_t expected_size,
                                    const char *expected_sha256,
                                    uint32_t *out_size,
                                    char out_sha256[65],
                                    modem_http_download_progress_cb_t progress_cb,
                                    void *progress_ctx)
{
    uint8_t ok = 0U;
    int http_status = 0;
    uint32_t content_len = 0U;
    uint32_t written = 0U;
    char calc_sha256[65] = {0};
    sha256_ctx_t sha;
    /* Diagnostics: per-window hash + modem UART error counters. */
    extern volatile uint32_t g_modem_uart_err_count;
    extern volatile uint32_t g_modem_uart_err_flags;
    extern volatile uint32_t g_modem_uart_rx_restarts;
    sha256_ctx_t win_sha;
    char win_hex[65] = {0};
    uint32_t win_err_start = g_modem_uart_err_count;
    uint32_t dl_err_start = g_modem_uart_err_count;

    if (out_size) {
        *out_size = 0U;
    }
    if (out_sha256) {
        out_sha256[0] = '\0';
    }

    if (!url || !url[0] || !flash || max_size == 0U) {
        HTTP_OTA_LOG("[OTA][HTTP] invalid download args\r\n");
        modem_http_set_last_error("invalid_args");
        return 0U;
    }

    if ((strlen(url) + 24U) >= sizeof(s_http_ota_cmd)) {
        HTTP_OTA_LOG("[OTA][HTTP] signed URL too long for AT command\r\n");
        modem_http_set_last_error("artifact_url_too_long");
        return 0U;
    }

    if (g_at_mutex) {
        osMutexAcquire(g_at_mutex, osWaitForever);
    }

    at_rx_active = 1;
    u2_flush_all();
    modem_http_set_last_error("none");

    HTTP_OTA_LOG("[OTA][HTTP] download start base=0x%06lX max=%lu expected=%lu\r\n",
                 (unsigned long)base_addr,
                 (unsigned long)max_size,
                 (unsigned long)expected_size);

    if (expected_size != 0U) {
        content_len = expected_size;
    } else {
        if (!modem_http_start_request_locked(url,
                                             0U,
                                             0U,
                                             0U,
                                             &http_status,
                                             &content_len)) {
            modem_http_set_last_error("http_start_failed");
            goto done;
        }

        if (http_status != 200 && http_status != 206) {
            HTTP_OTA_LOG("[OTA][HTTP] rejected HTTP status=%d\r\n", http_status);
            modem_http_set_last_error("http_status_rejected");
            goto done;
        }
    }

    if (content_len == 0U || content_len > max_size) {
        HTTP_OTA_LOG("[OTA][HTTP] invalid size len=%lu max=%lu\r\n",
                     (unsigned long)content_len,
                     (unsigned long)max_size);
        modem_http_set_last_error("invalid_size");
        goto done;
    }

    if (expected_size != 0U && content_len != expected_size) {
        HTTP_OTA_LOG("[OTA][HTTP] size mismatch expected=%lu actual=%lu\r\n",
                     (unsigned long)expected_size,
                     (unsigned long)content_len);
        modem_http_set_last_error("size_mismatch");
        goto done;
    }

    uint32_t erase_len = ((content_len + W25Q_SECTOR_SIZE - 1U) /
                          W25Q_SECTOR_SIZE) * W25Q_SECTOR_SIZE;
    HTTP_OTA_LOG("[OTA][FLASH] erase base=0x%06lX len=%lu\r\n",
                 (unsigned long)base_addr,
                 (unsigned long)erase_len);

    for (uint32_t off = 0U; off < erase_len; off += W25Q_SECTOR_SIZE) {
        modem_http_kick_watchdog();
        if (!w25q_erase_sector_4k(flash, base_addr + off)) {
            HTTP_OTA_LOG("[OTA][FLASH] erase failed addr=0x%06lX\r\n",
                         (unsigned long)(base_addr + off));
            modem_http_set_last_error("flash_erase_failed");
            goto done;
        }
    }

    sha256_init(&sha);
    uint8_t use_range_windows = HTTP_OTA_USE_RANGE_WINDOWS ? 1U : 0U;
    uint32_t window_remaining = 0U;
    uint32_t window_read = 0U;
    uint32_t active_window_start = 0xFFFFFFFFUL;
    uint8_t active_window_retries = 0U;

    if (!use_range_windows) {
        uint32_t action_len = 0U;
        http_status = 0;

        if (!modem_http_start_request_locked(url,
                                             0U,
                                             0U,
                                             0U,
                                             &http_status,
                                             &action_len)) {
            if (!strcmp(modem_http_download_last_error(), "none")) {
                modem_http_set_last_error("http_start_failed");
            }
            goto done;
        }

        if (http_status != 200 && http_status != 206) {
            HTTP_OTA_LOG("[OTA][HTTP] rejected HTTP status=%d\r\n", http_status);
            const char *action_error = modem_http_action_error_name(http_status);
            if (action_error) modem_http_set_last_error(action_error);
            else modem_http_set_last_error_status("http_status", http_status);
            goto done;
        }

        if (action_len != content_len) {
            HTTP_OTA_LOG("[OTA][HTTP] action size mismatch expected=%lu actual=%lu\r\n",
                         (unsigned long)content_len,
                         (unsigned long)action_len);
            modem_http_set_last_error("http_size_mismatch");
            goto done;
        }
    }

    while (written < content_len) {
        modem_http_kick_watchdog();

        if (use_range_windows && window_remaining == 0U) {
            uint32_t window_len = content_len - written;
            uint32_t action_len = 0U;
            if (window_len > HTTP_OTA_RANGE_WINDOW) {
                window_len = HTTP_OTA_RANGE_WINDOW;
            }

            if (active_window_start != written) {
                active_window_start = written;
                active_window_retries = 0U;
            }

            HTTP_OTA_LOG("[OTA][HTTP] range start=%lu len=%lu\r\n",
                         (unsigned long)written,
                         (unsigned long)window_len);

            http_status = 0;
            uint8_t range_open_ok = modem_http_start_request_locked(url,
                                                                    1U,
                                                                    written,
                                                                    window_len,
                                                                    &http_status,
                                                                    &action_len);
            uint8_t range_status_ok =
                (http_status == 206) ||
                (http_status == 200 &&
                 written == 0U &&
                 action_len == content_len &&
                 window_len == content_len);

            if (!range_open_ok || !range_status_ok) {
                if (!range_open_ok) {
                    HTTP_OTA_LOG("[OTA][HTTP] range open failed start=%lu cause=%s\r\n",
                                 (unsigned long)written,
                                 modem_http_download_last_error());
                    if (!strcmp(modem_http_download_last_error(), "none")) {
                        modem_http_set_last_error("range_open_failed");
                    }
                } else {
                    HTTP_OTA_LOG("[OTA][HTTP] rejected range status=%d\r\n",
                                 http_status);
                    const char *action_error = modem_http_action_error_name(http_status);
                    if (action_error) modem_http_set_last_error(action_error);
                    else modem_http_set_last_error_status("range_status", http_status);
                }

                uint8_t max_retries = range_open_ok ?
                                      HTTP_OTA_RANGE_STATUS_RETRIES :
                                      HTTP_OTA_RANGE_OPEN_RETRIES;
                /* SIMCom 7xx results are modem transport/TLS failures, not
                 * HTTP responses. Repeating an identical SSL-session setup
                 * cannot fix the configuration and only delays the failure. */
                if (range_open_ok && modem_http_action_error_name(http_status)) {
                    max_retries = 0U;
                }

                if (active_window_retries < max_retries) {
                    active_window_retries++;
                    HTTP_OTA_LOG("[OTA][HTTP] reopen range start=%lu retry=%u/%u\r\n",
                                 (unsigned long)active_window_start,
                                 active_window_retries,
                                 (unsigned int)max_retries);
                    modem_http_abort_response_locked();
                    osDelay(range_open_ok ?
                            (1500U * active_window_retries) :
                            (1000U * active_window_retries));
                    continue;
                }

                goto done;
            }

            window_remaining = window_len;
            window_read = 0U;
            sha256_init(&win_sha);
            win_err_start = g_modem_uart_err_count;
        }

        uint32_t want = use_range_windows ? window_remaining : (content_len - written);
        if (want > sizeof(s_http_ota_chunk)) {
            want = sizeof(s_http_ota_chunk);
        }

        uint32_t read_offset = use_range_windows ? window_read : written;
        int got = -1;
        for (uint32_t attempt = 1U;
             attempt <= HTTP_OTA_CHUNK_RETRIES;
             attempt++) {
            got = modem_http_read_chunk_locked(read_offset,
                                               want,
                                               s_http_ota_chunk,
                                               sizeof(s_http_ota_chunk),
                                               HTTP_OTA_READ_MS);
            if (got > 0 && got <= (int)want) {
                break;
            }

            HTTP_OTA_LOG("[OTA][HTTP] chunk retry offset=%lu attempt=%lu/%lu got=%d want=%lu\r\n",
                         (unsigned long)written,
                         (unsigned long)attempt,
                         (unsigned long)HTTP_OTA_CHUNK_RETRIES,
                         got,
                         (unsigned long)want);
            u2_flush_all();
            osDelay(250U * attempt);
        }

        if (got <= 0 || got > (int)want) {
            if (use_range_windows && active_window_retries < HTTP_OTA_RANGE_READ_RETRIES) {
                active_window_retries++;
                HTTP_OTA_LOG("[OTA][HTTP] restart range start=%lu retry=%u/%u\r\n",
                             (unsigned long)active_window_start,
                             active_window_retries,
                             (unsigned int)HTTP_OTA_RANGE_READ_RETRIES);
                modem_http_set_last_error("chunk_read_retry");
                modem_http_abort_response_locked();
                window_remaining = 0U;
                window_read = 0U;
                osDelay(500U * active_window_retries);
                continue;
            }

            HTTP_OTA_LOG("[OTA][HTTP] read failed offset=%lu\r\n",
                         (unsigned long)written);
            modem_http_set_last_error("chunk_read_failed");
            goto done;
        }

        if (got < (int)want) {
            HTTP_OTA_LOG("[OTA][HTTP] short chunk accepted offset=%lu got=%d want=%lu\r\n",
                         (unsigned long)written,
                         got,
                         (unsigned long)want);
        }

        if (!w25q_write(flash,
                        base_addr + written,
                        s_http_ota_chunk,
                        (uint32_t)got)) {
            HTTP_OTA_LOG("[OTA][FLASH] write failed offset=%lu len=%d\r\n",
                         (unsigned long)written,
                         got);
            modem_http_set_last_error("flash_write_failed");
            goto done;
        }

        sha256_update(&sha, s_http_ota_chunk, (size_t)got);
        if (use_range_windows) {
            sha256_update(&win_sha, s_http_ota_chunk, (size_t)got);
        }
        written += (uint32_t)got;
        if (use_range_windows) {
            window_read += (uint32_t)got;
            window_remaining -= (uint32_t)got;
            if (window_remaining == 0U) {
                sha256_final_hex(&win_sha, win_hex);
                HTTP_OTA_LOG("[OTA][WIN] start=%lu len=%lu sha256=%s uart_err=+%lu flags=0x%02lX restarts=%lu\r\n",
                             (unsigned long)(written - window_read),
                             (unsigned long)window_read,
                             win_hex,
                             (unsigned long)(g_modem_uart_err_count - win_err_start),
                             (unsigned long)g_modem_uart_err_flags,
                             (unsigned long)g_modem_uart_rx_restarts);
                (void)modem_http_send_wait_ok_locked("AT+HTTPTERM", NULL, 5000U);
                modem_http_drain_until_quiet_locked(250U, 80U);
                active_window_start = 0xFFFFFFFFUL;
                active_window_retries = 0U;
            }
        }

        if (progress_cb) {
            progress_cb(written, content_len, progress_ctx);
        }
    }

    sha256_final_hex(&sha, calc_sha256);
    if (out_sha256) {
        strncpy(out_sha256, calc_sha256, 65U);
        out_sha256[64] = '\0';
    }

    HTTP_OTA_LOG("[OTA][VERIFY] size=%lu sha256=%s uart_err_total=%lu flags=0x%02lX\r\n",
                 (unsigned long)written,
                 calc_sha256,
                 (unsigned long)(g_modem_uart_err_count - dl_err_start),
                 (unsigned long)g_modem_uart_err_flags);
    {
        /* Cross-check: hash what actually landed in external flash. */
        sha256_ctx_t rb_sha;
        char rb_hex[65] = {0};
        uint32_t off = 0U;
        uint8_t rb_ok = 1U;
        sha256_init(&rb_sha);
        while (off < written) {
            uint32_t n = written - off;
            if (n > sizeof(s_http_ota_chunk)) n = sizeof(s_http_ota_chunk);
            if (!w25q_read(flash, base_addr + off, s_http_ota_chunk, n)) { rb_ok = 0U; break; }
            sha256_update(&rb_sha, s_http_ota_chunk, n);
            off += n;
            modem_http_kick_watchdog();
        }
        sha256_final_hex(&rb_sha, rb_hex);
        HTTP_OTA_LOG("[OTA][VERIFY] flash readback ok=%u sha256=%s\r\n", rb_ok, rb_hex);
    }

    if (!modem_http_sha_matches(expected_sha256, calc_sha256)) {
        HTTP_OTA_LOG("[OTA][VERIFY] sha mismatch expected=%s actual=%s\r\n",
                     expected_sha256 ? expected_sha256 : "",
                     calc_sha256);
        modem_http_set_last_error("sha_mismatch");
        goto done;
    }

    if (out_size) {
        *out_size = written;
    }

    ok = 1U;
    modem_http_set_last_error("none");

done:
    (void)modem_http_send_wait_ok_locked("AT+HTTPTERM", NULL, 8000U);
    at_rx_active = 0;
    if (g_at_mutex) {
        osMutexRelease(g_at_mutex);
    }

    HTTP_OTA_LOG("[OTA][HTTP] download %s written=%lu\r\n",
                 ok ? "OK" : "FAIL",
                 (unsigned long)written);

    return ok;
}

/* ================= AT COMMAND ENGINE (UPDATED) ================= */
/**
 * @param flush_rx: If true, clears buffer before sending.
 * If false, keeps existing data (use for URC-heavy commands).
 */
uint8_t SIM800_SendATCommand2(const char *cmd,
                             const char *expected,
                             uint32_t timeout_ms,
                             bool flush_rx)
{
    uint32_t t0 = HAL_GetTick();
    uint8_t  expected_seen = 0;
    char     line[256];

    /* Guard: never run AT engine with NULL/EMPTY command */
    if (!cmd || cmd[0] == '\0') {
        AT_TRACE_LOG("[AT] IGNORE: NULL/EMPTY cmd\r\n");
        return 0;
    }

    /* Optional debug delay (keep 0 in production) */
    /* osDelay(6000); */

    if (g_at_mutex) osMutexAcquire(g_at_mutex, osWaitForever);

    at_rx_active = 1;

    if (flush_rx) {
        u2_drain_pending();
        AT_TRACE_LOG("[AT] RX flushed\r\n");
    }

    AT_TRACE_LOG("[AT] task=%s\r\n", osThreadGetName(osThreadGetId()));

    AT_TRACE_LOG("\r\n================ AT START ================\r\n");
    AT_TRACE_LOG("[AT] timeout=%lu ms\r\n", (unsigned long)timeout_ms);
    AT_TRACE_TEXT("[AT] cmd     =", cmd);
    AT_TRACE_LOG("[AT] expect  = %s\r\n", (expected) ? expected : "(none)");
    AT_TRACE_LOG("==========================================\r\n");

    /* Send command */
    if (cmd && cmd[0] != '\0') {
        AT_TRACE_LOG("\r\n");
        AT_TRACE_TEXT("[AT->GSM]", cmd);
        u2_send_cmd(cmd);
    }

    if (expected == NULL) expected_seen = 1;

    while ((HAL_GetTick() - t0) < timeout_ms) {
        app_watchdog_mark_task(APP_WATCHDOG_TASK_RUNTIME);
        app_debug_led_run_once();
        app_watchdog_refresh();

        /* slice the remaining time, don’t always block 500ms */
        uint32_t elapsed = HAL_GetTick() - t0;
        uint32_t remain  = (elapsed < timeout_ms) ? (timeout_ms - elapsed) : 0;
        uint32_t slice   = (remain > 200) ? 200 : remain;   // 0..200ms

        int n = u2rx_read_line(&g_u2rx, line, (int)sizeof(line), slice);
        if (n <= 0) {
            osDelay(1);
            continue;
        }

        /* RAW print */
        {
            uint32_t dt = HAL_GetTick() - t0;
            if (modem_text_is_sensitive_or_payload(line)) {
                AT_TRACE_LOG("[GSM->AT][%lums][RAW][n=%d] <payload echo hidden len=%lu>\r\n",
                             (unsigned long)dt,
                             n,
                             (unsigned long)strlen(line));
            } else {
                AT_TRACE_LOG("[GSM->AT][%lums][RAW][n=%d] %s\r\n", (unsigned long)dt, n, line);
            }
        }

        trim_crlf(line);
        if (!line[0]) continue;

        /* Trimmed print */
        {
            uint32_t dt = HAL_GetTick() - t0;
            if (modem_text_is_sensitive_or_payload(line)) {
                AT_TRACE_LOG("[GSM->AT][%lums] <payload echo hidden len=%lu>\r\n",
                             (unsigned long)dt,
                             (unsigned long)strlen(line));
            } else {
                AT_TRACE_LOG("[GSM->AT][%lums] %s\r\n", (unsigned long)dt, line);
            }
        }

        /* Ignore echo */
        if (cmd && cmd[0] && !strcmp(line, cmd)) {
            AT_TRACE_LOG("[AT] (ignored echo)\r\n");
            continue;
        }

        /* ERROR terminators */
        if (strstr(line, "ERROR") || strstr(line, "+CME ERROR")) {
            AT_TRACE_LOG("[AT] FAIL: %s\r\n", line);
            AT_TRACE_LOG("================ AT END (FAIL) ================\r\n");
            at_rx_active = 0;
            if (g_at_mutex) osMutexRelease(g_at_mutex);
            return 0;
        }

        /* Prompt terminator */
        if (strchr(line, '>')) {
            AT_TRACE_LOG("[AT] PROMPT received: %s\r\n", line);
            AT_TRACE_LOG("================ AT END (PROMPT) ==============\r\n");
            at_rx_active = 0;
            if (g_at_mutex) osMutexRelease(g_at_mutex);
            return 1;
        }

        /* Mark expected seen (DO NOT return here) */
        if (!expected_seen && expected && strstr(line, expected)) {
            expected_seen = 1;
            AT_TRACE_LOG("[AT] expected matched: %s\r\n", expected);
            /* keep waiting for OK/ERROR */
        }

        /* OK terminator */
        if (!strcmp(line, "OK")) {
            AT_TRACE_LOG("[AT] OK\r\n");
            AT_TRACE_LOG("[AT] expected_seen=%d\r\n", expected_seen);
            AT_TRACE_LOG("================ AT END (OK) ==================\r\n");
            at_rx_active = 0;
            if (g_at_mutex) osMutexRelease(g_at_mutex);
            return expected_seen ? 1 : 0;
        }
    }

    /* Timeout */
    AT_TRACE_LOG("[AT] TIMEOUT after %lu ms\r\n", (unsigned long)timeout_ms);
    AT_TRACE_TEXT("[AT] Cmd=", cmd);
    AT_TRACE_LOG("[AT] expected_seen=%d\r\n", expected_seen);
    AT_TRACE_LOG("[AT] rx_available=%lu uart_error=0x%08lX\r\n",
                 (unsigned long)u2rx_available(&g_u2rx),
                 (unsigned long)HAL_UART_GetError(g_u2rx.huart));
    AT_TRACE_LOG("================ AT END (TIMEOUT) =============\r\n");

    at_rx_active = 0;
    if (g_at_mutex) osMutexRelease(g_at_mutex);
    return 0;
}

/* ================= RETRY WRAPPER ================= */
static bool at_should_flush(const char *cmd)
{
    if (!cmd) return false;

    // Only safe for basic commands (no late OK problems)
    if (!strcmp(cmd, "AT"))   return true;
    if (!strcmp(cmd, "ATE0")) return true;
    if (!strcmp(cmd, "ATI"))  return true;
    if (!strncmp(cmd, "AT+CMQTTTOPIC=", 14))   return true;
    if (!strncmp(cmd, "AT+CMQTTPAYLOAD=", 16)) return true;
    if (!strncmp(cmd, "AT+CMQTTSUBTOPIC=", 17)) return true;

    return false;
}

uint8_t SendCommandWithRetry(const char *cmd,
                             const char *expected,
                             uint16_t timeout_ms,
                             uint8_t retries)
{
    for (uint8_t i = 0; i < retries; i++) {

        bool flush = at_should_flush(cmd);

        AT_TRACE_LOG("\r\n[TRY %d/%d] flush=%d\r\n", i + 1, retries, flush ? 1 : 0);

        if (SIM800_SendATCommand2(cmd, expected, timeout_ms, flush)) {
            AT_TRACE_LOG("[AT] command success\r\n");
            return 1;
        }

        AT_TRACE_LOG("[AT] command failed, retrying\r\n");
        app_watchdog_mark_task(APP_WATCHDOG_TASK_RUNTIME);
        app_debug_led_run_once();
        app_watchdog_refresh();
        osDelay(1000);
    }
    return 0;
}


/* ================= HIGH LEVEL ================= */
uint8_t GPRS_Init(const char *apn)
{
    char cmd[128];
    const char *apn_u = (apn && apn[0]) ? apn : APN;

    print("[GPRS] starting PDP setup apn=%s\r\n", apn_u);

    if (!SendCommandWithRetry("ATE0", NULL, 2000, 2)) return 0;

    // Show verbose modem error causes instead of plain "ERROR".
    (void)SendCommandWithRetry("AT+CMEE=2", "OK", 2000, 1);

    snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"IP\",\"%s\"", apn_u);

    if (!SendCommandWithRetry(cmd, NULL, 8000, 2)) return 0;

    if (!SendCommandWithRetry("AT+CGATT=1", NULL, 10000, 2)) return 0;

    /*
     * CGATT only attaches packet service; it does not guarantee that PDP
     * context 1 is active with an IP address. MQTT can fail at CMQTTSTART
     * on some modem/network states unless the data context is active first.
     */
    if (!SendCommandWithRetry("AT+CGATT?", "+CGATT: 1", 5000, 1)) return 0;
    if (!SendCommandWithRetry("AT+CGACT=1,1", "OK", 15000, 2)) return 0;
    if (!SendCommandWithRetry("AT+CGACT?", "+CGACT: 1,1", 5000, 1)) return 0;
    if (!SendCommandWithRetry("AT+CGPADDR=1", "+CGPADDR: 1,", 5000, 2)) return 0;

    print("[GPRS] connected and IP context active\r\n");
    return 1;
}


void GetIMEI(void)
{
    char line[64];
    uint32_t t0;
    print("[MODEM] reading IMEI\r\n");
    memset(imei, 0, sizeof(imei));
    at_rx_active = 1;
    u2_flush_all();
    u2_send_cmd("AT+CGSN");
    t0 = HAL_GetTick();
    while ((HAL_GetTick() - t0) < 3000) {
        int n = u2rx_read_line(&g_u2rx, line, (int)sizeof(line), 250);
        if (n <= 0) { osDelay(1); continue; }
        trim_crlf(line);
        if (!line[0]) continue;
        if (!strcmp(line, "OK") || !strcmp(line, "ERROR") || !strcmp(line, "AT+CGSN")) continue;
        int all_digits = 1;
        for (int i = 0; line[i]; i++) {
            if (!isdigit((unsigned char)line[i])) { all_digits = 0; break; }
        }
        if (all_digits && (int)strlen(line) >= 14) {
            strncpy(imei, line, 15);
            imei[15] = 0;
            print("[MODEM] IMEI ready imei=%s\r\n", imei);
            at_rx_active = 0;
            (void)SIM800_SendATCommand2("", NULL, 300, false);
            print("[MODEM] querying modem firmware revision\r\n");
            (void)SIM800_SendATCommand2("AT+CGMR", NULL, 5000, true);
            return;
        }
    }
    at_rx_active = 0;
    print("[MODEM][ERR] IMEI read failed\r\n");
}

void WaitForNetworkAndBlinkLED(void)
{
    print("Waiting for GSM Network Registration...\r\n");
    while (1) {
        if (SIM800_SendATCommand2("AT+CGREG?", "0,1", 3000, false)) {
            print("Network registered\r\n");
            break;
        }
//        HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_7);
        osDelay(2000);
    }
//    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_7, GPIO_PIN_RESET);
}
void GSM_Network_Diagnostics(void)
{
    print("\r\n====================================\r\n");
    print(" GSM NETWORK DIAGNOSTICS\r\n");
    print("====================================\r\n");

    SIM800_SendATCommand2("AT", NULL, 3000, true);

    print("\r\n--- SIM Status ---\r\n");
    SIM800_SendATCommand2("AT+CPIN?", NULL, 3000, true);

    print("\r\n--- Signal Quality ---\r\n");
    SIM800_SendATCommand2("AT+CSQ", NULL, 3000, true);

    print("\r\n--- Network Registration ---\r\n");
    SIM800_SendATCommand2("AT+CREG?", NULL, 3000, true);

    print("\r\n--- GPRS Registration ---\r\n");
    SIM800_SendATCommand2("AT+CGREG?", NULL, 3000, true);

    print("\r\n--- Packet Attach Status ---\r\n");
    SIM800_SendATCommand2("AT+CGATT?", NULL, 3000, true);

    print("\r\n--- Operator ---\r\n");
    SIM800_SendATCommand2("AT+COPS?", NULL, 5000, true);

    print("\r\n--- PDP Context ---\r\n");
    SIM800_SendATCommand2("AT+CGDCONT?", NULL, 5000, true);

    print("\r\n--- PDP Activation Status ---\r\n");
    SIM800_SendATCommand2("AT+CGACT?", NULL, 5000, true);

    print("\r\n--- IP Address ---\r\n");
    SIM800_SendATCommand2("AT+CGPADDR=1", NULL, 5000, true);

    print("\r\n====================================\r\n");
    print(" END OF DIAGNOSTICS\r\n");
    print("====================================\r\n");
}
/* ================= HTTP CHECK (FIXED) ================= */
void Check_Internet_Service(void)
{
    print("\r\n========== INTERNET CHECK ==========\r\n");
    GSM_Network_Diagnostics();

    (void)SIM800_SendATCommand2("ATE0", NULL, 2000, true);

    /* Check SIM */
    (void)SIM800_SendATCommand2("AT+CPIN?", "READY", 3000, true);

    /* Check network registration */
    (void)SIM800_SendATCommand2("AT+CREG?", NULL, 3000, true);
    (void)SIM800_SendATCommand2("AT+CGREG?", NULL, 3000, true);

    /* Check signal quality */
    (void)SIM800_SendATCommand2("AT+CSQ", NULL, 3000, true);

    /* Check operator */
    (void)SIM800_SendATCommand2("AT+COPS?", NULL, 5000, true);

    /* Check IP status before attach */
    (void)SIM800_SendATCommand2("AT+CGATT?", NULL, 3000, true);

    print("[NET] Attaching GPRS...\r\n");

    if (!SIM800_SendATCommand2("AT+CGATT=1", NULL, 15000, true))
    {
        print("[NET] CGATT failed\r\n");
        return;
    }

    (void)SIM800_SendATCommand2("AT+CGATT?", NULL, 3000, true);

    print("[NET] Activating PDP...\r\n");

    if (!SIM800_SendATCommand2("AT+CGACT=1,1", NULL, 20000, false))
    {
        print("[NET] CGACT failed\r\n");
        return;
    }

    (void)SIM800_SendATCommand2("AT+CGACT?", NULL, 3000, true);

    /* Cleanup previous HTTP session */
    (void)SIM800_SendATCommand2("AT+HTTPTERM", NULL, 5000, false);

    if (!SIM800_SendATCommand2("AT+HTTPINIT", NULL, 8000, false))
    {
        print("[HTTP] HTTPINIT failed\r\n");
        return;
    }

    (void)SIM800_SendATCommand2(
        "AT+HTTPPARA=\"URL\",\"http://www.google.com/\"",
        NULL,
        8000,
        false);

    u2rx_drop_all(&g_u2rx);

    if (!SIM800_SendATCommand2("AT+HTTPACTION=0", NULL, 4000, false))
    {
        print("[HTTP] HTTPACTION command failed\r\n");
        (void)SIM800_SendATCommand2("AT+HTTPTERM", NULL, 5000, false);
        return;
    }

    if (!u2rx_wait_for(&g_u2rx, "+HTTPACTION:", 30000))
    {
        print("[HTTP] No HTTPACTION URC\r\n");
        (void)SIM800_SendATCommand2("AT+HTTPTERM", NULL, 5000, false);
        return;
    }

    if (u2rx_wait_for(&g_u2rx, "0,200", 3000))
    {
        print("[HTTP] Internet working (200 OK)\r\n");
    }
    else
    {
        print("[HTTP] Request failed\r\n");
    }

    (void)SIM800_SendATCommand2("AT+HTTPTERM", NULL, 8000, false);

    print("========== INTERNET CHECK DONE ==========\r\n");
}

/* ---------------- network clock -> RTC ----------------
 * The RTC is the time base for actuation expiry and for telemetry/event
 * timestamps. It used to be set only from the build time and was found
 * months off (2026-08-27 on 2026-11-16), so command expiry never triggered.
 * The modem carries operator network time (NITZ, enabled with AT+CTZU=1);
 * copy it into the RTC, which is kept in IST (UTC+05:30) like the rest of
 * the firmware assumes. */
static int64_t mh_days_from_civil(int y, int m, int d)
{
    y -= (m <= 2) ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = (int)(y - era * 400);
    const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void mh_civil_from_days(int64_t z, int *y, int *m, int *d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int doe = (int)(z - era * 146097);
    const int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yoe + era * 400) + ((*m <= 2) ? 1 : 0);
}

#define MH_IST_OFFSET_S   (5 * 3600 + 30 * 60)
#define MH_RTC_RESYNC_S   5

/* Make sure a CA file is on the modem (AT+CCERTDOWN once; the modem keeps
 * it across power cycles). Used for the MQTT broker roots. */
uint8_t modem_tls_ensure_ca(const char *file, const unsigned char *pem, uint16_t len)
{
    char cmd[96];
    uint8_t present = 0U;
    uint8_t ok = 0U;

    if (!file || !pem || len == 0U) return 0U;
    if (g_at_mutex) osMutexAcquire(g_at_mutex, osWaitForever);
    at_rx_active = 1;
    u2_flush_all();

    if (!modem_ca_file_exists_locked(file, &present)) goto done;
    if (!present) {
        int n = snprintf(cmd, sizeof(cmd), "AT+CCERTDOWN=\"%s\",%u", file, (unsigned)len);
        if (n < 0 || (size_t)n >= sizeof(cmd)) goto done;
        u2_flush_all();
        print("[TLS] installing CA file %s (%u bytes)\r\n", file, (unsigned)len);
        u2_send_cmd(cmd);
        if (!u2rx_wait_for(&g_u2rx, ">", 8000U)) goto done;
        if (HAL_UART_Transmit(&huart6, (uint8_t *)pem, len, 10000U) != HAL_OK ||
            !modem_http_wait_ok_after_data_locked(10000U)) goto done;
        if (!modem_ca_file_exists_locked(file, &present) || !present) goto done;
    }
    ok = 1U;

done:
    at_rx_active = 0;
    if (g_at_mutex) osMutexRelease(g_at_mutex);
    if (!ok) print("[TLS][ERR] CA file %s not available\r\n", file);
    return ok;
}

#ifndef CLOCK_TEST_IGNORE_NITZ
#define CLOCK_TEST_IGNORE_NITZ 0
#endif
#ifndef CLOCK_TEST_NO_NTP
#define CLOCK_TEST_NO_NTP 0
#endif
#define MH_NTP_RETRY_MS   (10UL * 60UL * 1000UL)
#define MH_NTP_SERVER     "pool.ntp.org"

/* NTP through the modem, at most every 10 min. The modem clock is set in
 * IST (+22 quarter hours) so the CCLK read that follows looks like network
 * time. Called with the AT mutex held. 1 when the modem reported success. */
static uint8_t modem_ntp_sync_due_locked(void)
{
    static uint32_t s_last_try;
    static uint8_t s_tried;
    char line[96];
    uint32_t now = HAL_GetTick();

#if CLOCK_TEST_NO_NTP
    return 0U;
#endif
    if (s_tried && (uint32_t)(now - s_last_try) < MH_NTP_RETRY_MS) return 0U;
    s_tried = 1U;
    s_last_try = now;

    print("[CLOCK] no network time from the operator, trying NTP (%s)\r\n", MH_NTP_SERVER);
    u2_drain_pending();
    u2_send_cmd("AT+CNTP=\"" MH_NTP_SERVER "\",22");
    uint32_t start = HAL_GetTick();
    while ((HAL_GetTick() - start) < 3000U) {
        int n = u2rx_read_line(&g_u2rx, line, sizeof(line), 500);
        if (n <= 0) continue;
        trim_crlf(line);
        if (!strcmp(line, "OK")) break;
        if (strstr(line, "ERROR")) return 0U;
    }
    u2_drain_pending();
    u2_send_cmd("AT+CNTP");
    start = HAL_GetTick();
    while ((HAL_GetTick() - start) < 20000U) {
        modem_http_kick_watchdog();
        int n = u2rx_read_line(&g_u2rx, line, sizeof(line), 500);
        if (n <= 0) continue;
        trim_crlf(line);
        const char *p = strstr(line, "+CNTP:");
        if (p) {
            int code = -1;
            (void)sscanf(p, "+CNTP: %d", &code);
            print("[CLOCK] NTP %s (+CNTP: %d)\r\n", code == 0 ? "ok" : "failed", code);
            return (code == 0) ? 1U : 0U;
        }
        if (strstr(line, "ERROR")) break;
    }
    print("[CLOCK] NTP failed (no answer)\r\n");
    return 0U;
}

uint8_t modem_sync_rtc_from_network(void)
{
    char line[96];
    uint8_t parsed = 0U;
    unsigned yy = 0U, mo = 0U, dd = 0U, hh = 0U, mi = 0U, ss = 0U, tzq = 0U;
    char tzsign = '+';

    if (g_at_mutex) osMutexAcquire(g_at_mutex, osWaitForever);
    at_rx_active = 1;

    /* Network time updates into the modem clock only with CTZU on. */
    u2_drain_pending();
    u2_send_cmd("AT+CTZU=1");
    uint32_t start = HAL_GetTick();
    while ((HAL_GetTick() - start) < 2000U) {
        int n = u2rx_read_line(&g_u2rx, line, sizeof(line), 500);
        if (n <= 0) continue;
        trim_crlf(line);
        if (!strcmp(line, "OK") || strstr(line, "ERROR")) break;
    }

    /* Network time (NITZ) first; if the operator sends none, NTP through the
     * modem (2026-11-19): before, a unit on such a network never got a
     * clock, stamped everything 1970 and refused every relay command. */
    for (uint8_t pass = 0U; pass < 2U; pass++) {
        parsed = 0U;
        u2_drain_pending();
        u2_send_cmd("AT+CCLK?");
        start = HAL_GetTick();
        while ((HAL_GetTick() - start) < 2000U) {
            int n = u2rx_read_line(&g_u2rx, line, sizeof(line), 500);
            if (n <= 0) continue;
            trim_crlf(line);
            const char *p = strstr(line, "+CCLK:");
            if (p) {
                int got = sscanf(p, "+CCLK: \"%2u/%2u/%2u,%2u:%2u:%2u%c%2u",
                                 &yy, &mo, &dd, &hh, &mi, &ss, &tzsign, &tzq);
                parsed = (got == 8 && (tzsign == '+' || tzsign == '-')) ? 1U : 0U;
                continue;
            }
            if (!strcmp(line, "OK") || strstr(line, "ERROR")) break;
        }
#if CLOCK_TEST_IGNORE_NITZ
        if (pass == 0U) parsed = 0U;
#endif
        uint8_t valid = (parsed && yy >= 25U && mo >= 1U && mo <= 12U && dd >= 1U && dd <= 31U &&
                         hh <= 23U && mi <= 59U && ss <= 59U && tzq <= 56U) ? 1U : 0U;
        if (valid || pass == 1U) break;
        if (!modem_ntp_sync_due_locked()) break;
    }

    at_rx_active = 0;
    if (g_at_mutex) osMutexRelease(g_at_mutex);

    /* An unsynced modem reports its default epoch (70/01/01, 80/01/06...). */
    if (!parsed || yy < 25U || mo < 1U || mo > 12U || dd < 1U || dd > 31U ||
        hh > 23U || mi > 59U || ss > 59U || tzq > 56U) {
        print("[CLOCK] network time not available yet\r\n");
        return 0U;
    }

    int64_t local_s = mh_days_from_civil((int)(2000U + yy), (int)mo, (int)dd) * 86400 +
                      (int64_t)hh * 3600 + (int64_t)mi * 60 + (int64_t)ss;
    int64_t tz_s = (int64_t)tzq * 15 * 60;
    int64_t utc_s = (tzsign == '-') ? (local_s + tz_s) : (local_s - tz_s);
    int64_t ist_s = utc_s + MH_IST_OFFSET_S;

    int64_t rtc_s = 0;
    rtc_datetime_t now;
    uint8_t rtc_ok = (rtc_is_datetime_valid() && rtc_get_datetime(&now)) ? 1U : 0U;
    if (rtc_ok) {
        rtc_s = mh_days_from_civil(now.year, now.month, now.day) * 86400 +
                (int64_t)now.hour * 3600 + (int64_t)now.minute * 60 + now.second;
    }
    int64_t drift = rtc_ok ? (rtc_s - ist_s) : 0;
    if (rtc_ok && drift > -MH_RTC_RESYNC_S && drift < MH_RTC_RESYNC_S) {
        print("[CLOCK] RTC in sync with network (drift=%ld s)\r\n", (long)drift);
        return 1U;
    }

    int y, m, d;
    int64_t days = ist_s / 86400;
    int64_t secs = ist_s % 86400;
    mh_civil_from_days(days, &y, &m, &d);
    uint8_t set_ok = rtc_set_datetime_ist((uint16_t)y, (uint8_t)m, (uint8_t)d,
                                          (uint8_t)(secs / 3600), (uint8_t)((secs % 3600) / 60),
                                          (uint8_t)(secs % 60));
    print("[CLOCK] RTC %s from network time %04d-%02d-%02dT%02ld:%02ld:%02ld+05:30 (drift was %ld s%s)\r\n",
          set_ok ? "set" : "set FAILED",
          y, m, d, (long)(secs / 3600), (long)((secs % 3600) / 60), (long)(secs % 60),
          (long)drift, rtc_ok ? "" : ", RTC was invalid");
    app_log_push(set_ok ? APP_LOG_INFO : APP_LOG_WARN, "CLOCK", "RTC_SYNCED_FROM_NETWORK",
                 set_ok ? "rtc set from network time" : "rtc set failed",
                 (int32_t)(drift > 2147483647 ? 2147483647 : (drift < -2147483647 ? -2147483647 : drift)), rtc_ok);
    return set_ok;
}

uint8_t modem_get_signal_quality(int32_t *rssi, int32_t *ber)
{
    char line[96];
    uint8_t ok = 0U;
    uint8_t done = 0U;

    if (!rssi || !ber) {
        return 0;
    }

    *rssi = 99;
    *ber  = 99;

    /*
     * Important:
     * Keep this function short and only call it occasionally,
     * for example once every 60 seconds from health telemetry.
     */

    if (g_at_mutex) osMutexAcquire(g_at_mutex, osWaitForever);

    at_rx_active = 1;
    u2_drain_pending();
    print("[MODEM] CSQ query\r\n");
    u2_send_cmd("AT+CSQ");

    uint32_t start = HAL_GetTick();

    while ((HAL_GetTick() - start) < 2000U)
    {
        int n = u2rx_read_line(&g_u2rx, line, sizeof(line), 500);

        if (n <= 0) {
            continue;
        }

        trim_crlf(line);
        if (!line[0]) {
            continue;
        }

        print("[MODEM][CSQ] %s\r\n", line);

        /*
         * Expected line:
         * +CSQ: 18,0
         */
        char *p = strstr(line, "+CSQ:");
        if (p)
        {
            int r = 99;
            int b = 99;

            if (sscanf(p, "+CSQ: %d,%d", &r, &b) == 2)
            {
                *rssi = r;
                *ber  = b;

                print("[MODEM] CSQ rssi=%ld ber=%ld\r\n",
                      (long)*rssi,
                      (long)*ber);

                ok = 1U;
                continue;
            }
        }

        if (!strcmp(line, "OK")) {
            done = 1U;
            break;
        }

        if (strstr(line, "ERROR") || strstr(line, "+CME ERROR")) {
            done = 1U;
            break;
        }

        /*
         * Ignore echo, blank lines, OK, and other URCs.
         */
    }

    at_rx_active = 0;
    if (g_at_mutex) osMutexRelease(g_at_mutex);

    if (ok && !done) {
        print("[MODEM] CSQ parsed without final OK\r\n");
    }

    if (!ok) {
        app_log_push(APP_LOG_WARN,
                     "MODEM",
                     "CSQ_FAILED",
                     "failed to read signal quality",
                     0,
                     0);

        print("[MODEM] CSQ read failed\r\n");
    }

    return ok;
}

uint8_t Modem_SoftReset(void)
{
    print("[MODEM] soft reset start\r\n");

    /*
     * First preferred reset command.
     * It may not return OK reliably before modem restarts,
     * so do not treat missing OK as immediate fatal.
     */
    if (SIM800_SendATCommand2("AT+CFUN=1,1", "OK", 3000, true))
    {
        print("[MODEM] AT+CFUN=1,1 accepted\r\n");
    }
    else
    {
        print("[MODEM] AT+CFUN=1,1 no OK, trying AT+CRESET\r\n");

        if (SIM800_SendATCommand2("AT+CRESET", "OK", 3000, true))
        {
            print("[MODEM] AT+CRESET accepted\r\n");
        }
        else
        {
            print("[MODEM] soft reset command not accepted\r\n");
        }
    }

    /*
     * Give modem time to reboot. Some modules can take much longer after
     * CFUN/CRESET, especially on weak supply or slow network initialization.
     */
    for (uint8_t i = 0; i < 30; i++)
    {
        app_watchdog_mark_task(APP_WATCHDOG_TASK_RUNTIME);
        app_debug_led_run_once();
        app_watchdog_refresh();
        osDelay(1000);
    }

    /*
     * Re-sync AT after reboot. ATE0 is the real sync command because some
     * modems echo plain AT without returning OK until echo is disabled.
     */
    for (uint8_t i = 0; i < 8; i++)
    {
        app_watchdog_mark_task(APP_WATCHDOG_TASK_RUNTIME);
        app_debug_led_run_once();
        app_watchdog_refresh();
        (void)SIM800_SendATCommand2("AT", "OK", 500, true);

        if (SIM800_SendATCommand2("ATE0", "OK", 3000, true))
        {
            print("[MODEM] soft reset recovery OK\r\n");
            return 1;
        }

        osDelay(2000);
    }

    print("[MODEM] soft reset recovery failed\r\n");
    return 0;
}
