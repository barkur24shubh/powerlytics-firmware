#include "modem_handler.h"
#include "main.h"
#include "config.h"
#include "debug_uart.h"
#include "uart2_rx_engine.h"
#include "cmsis_os.h" // Added for osDelay
#include "app_debug_led.h"
#include "app_log.h"
#include "app_watchdog.h"

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

static void trim_crlf(char *s)
{
    if (!s) return;
    int n = (int)strlen(s);
    while (n > 0 && (s[n-1] == '\r' || s[n-1] == '\n')) {
        s[n-1] = 0;
        n--;
    }
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
        print("[AT] IGNORE: NULL/EMPTY cmd\r\n");
        return 0;
    }

    /* Optional debug delay (keep 0 in production) */
    /* osDelay(6000); */

    if (g_at_mutex) osMutexAcquire(g_at_mutex, osWaitForever);

    at_rx_active = 1;

    if (flush_rx) {
        u2_flush_all();
        print("[AT] RX flushed\r\n");
    }

    print("[AT] task=%s\r\n", osThreadGetName(osThreadGetId()));

    print("\r\n================ AT START ================\r\n");
    print("[AT] timeout=%lu ms\r\n", (unsigned long)timeout_ms);
    print("[AT] cmd     = %s\r\n", (cmd && cmd[0]) ? cmd : "(NULL/EMPTY)");
    print("[AT] expect  = %s\r\n", (expected) ? expected : "(none)");
    print("==========================================\r\n");

    /* Send command */
    if (cmd && cmd[0] != '\0') {
        print("\r\n[AT->GSM] %s\r\n", cmd);
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
            print("[GSM->AT][%lums][RAW][n=%d] %s\r\n", (unsigned long)dt, n, line);
        }

        trim_crlf(line);
        if (!line[0]) continue;

        /* Trimmed print */
        {
            uint32_t dt = HAL_GetTick() - t0;
            print("[GSM->AT][%lums] %s\r\n", (unsigned long)dt, line);
        }

        /* Ignore echo */
        if (cmd && cmd[0] && !strcmp(line, cmd)) {
            print("[AT] (ignored echo)\r\n");
            continue;
        }

        /* ERROR terminators */
        if (strstr(line, "ERROR") || strstr(line, "+CME ERROR")) {
            print("[AT] FAIL: %s\r\n", line);
            print("================ AT END (FAIL) ================\r\n");
            at_rx_active = 0;
            if (g_at_mutex) osMutexRelease(g_at_mutex);
            return 0;
        }

        /* Prompt terminator */
        if (strchr(line, '>')) {
            print("[AT] PROMPT received: %s\r\n", line);
            print("================ AT END (PROMPT) ==============\r\n");
            at_rx_active = 0;
            if (g_at_mutex) osMutexRelease(g_at_mutex);
            return 1;
        }

        /* Mark expected seen (DO NOT return here) */
        if (!expected_seen && expected && strstr(line, expected)) {
            expected_seen = 1;
            print("[AT] expected matched: %s\r\n", expected);
            /* keep waiting for OK/ERROR */
        }

        /* OK terminator */
        if (!strcmp(line, "OK")) {
            print("[AT] OK\r\n");
            print("[AT] expected_seen=%d\r\n", expected_seen);
            print("================ AT END (OK) ==================\r\n");
            at_rx_active = 0;
            if (g_at_mutex) osMutexRelease(g_at_mutex);
            return expected_seen ? 1 : 0;
        }
    }

    /* Timeout */
    print("[AT] TIMEOUT after %lu ms\r\n", (unsigned long)timeout_ms);
    print("[AT] Cmd=%s\r\n", (cmd && cmd[0]) ? cmd : "NULL/EMPTY");
    print("[AT] expected_seen=%d\r\n", expected_seen);
    print("================ AT END (TIMEOUT) =============\r\n");

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

        print("\r\n[TRY %d/%d] flush=%d\r\n", i + 1, retries, flush ? 1 : 0);

        if (SIM800_SendATCommand2(cmd, expected, timeout_ms, flush)) {
            print("✔ COMMAND SUCCESS\r\n");
            return 1;
        }

        print("✖ COMMAND FAILED, retrying...\r\n");
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

    if (!SendCommandWithRetry("ATE0", NULL, 2000, 2)) return 0;

    // Show verbose modem error causes instead of plain "ERROR".
    (void)SendCommandWithRetry("AT+CMEE=2", "OK", 2000, 1);

    snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"IP\",\"%s\"",
             (apn && apn[0]) ? apn : APN);

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

    return 1;
}


void GetIMEI(void)
{
    char line[64];
    uint32_t t0;
    print("inside IMEI READ\r\n");
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
            print("IMEI: %s\r\n", imei);
            at_rx_active = 0;
            (void)SIM800_SendATCommand2("", NULL, 300, false);
            return;
        }
    }
    at_rx_active = 0;
    print("IMEI read FAIL\r\n");
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
    u2_flush_all();
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
