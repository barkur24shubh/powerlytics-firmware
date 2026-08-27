#include "app_sd_card.h"

#include "fatfs.h"
#include "debug_uart.h"
#include "app_modbus_payload.h"
#include "rtc_manager.h"
#include "cJSON.h"
#include "comm_manager.h"

#include "stm32f4xx_hal.h"

#include <stdio.h>
#include <string.h>

#define APP_SD_TEST_FILE "SDTEST.TXT"
#define APP_SD_TELEMETRY_INTERVAL_MS (5UL * 60UL * 1000UL)
#define APP_SD_TERMINAL_LOG_BUFFER_SZ 8192U
#define APP_SD_TERMINAL_LINE_SZ 512U
#define APP_SD_TERMINAL_FLUSH_MS 5000UL

static char s_terminal_log_buf[APP_SD_TERMINAL_LOG_BUFFER_SZ];
static char s_terminal_line_buf[APP_SD_TERMINAL_LINE_SZ];
static size_t s_terminal_log_len = 0U;
static size_t s_terminal_line_len = 0U;
static uint8_t s_terminal_log_enabled = 0U;
static uint8_t s_sd_busy = 0U;
static uint8_t s_terminal_log_flush_busy = 0U;
static uint32_t s_terminal_log_last_flush = 0U;
static uint32_t s_terminal_log_dropped = 0U;
static uint8_t s_sd_checked = 0U;
static uint8_t s_sd_available = 0U;

static FRESULT app_sd_mkdir_if_needed(const char *path)
{
    FRESULT fr = f_mkdir(path);

    if (fr == FR_EXIST)
    {
        return FR_OK;
    }

    return fr;
}

static void app_sd_join_path(char *out, size_t out_sz, const char *suffix)
{
    size_t base_len = strlen(SDPath);

    if (base_len > 0U && SDPath[base_len - 1U] == '/')
    {
        snprintf(out, out_sz, "%s%s", SDPath, suffix);
    }
    else
    {
        snprintf(out, out_sz, "%s/%s", SDPath, suffix);
    }
}

static uint8_t app_sd_make_month_paths(char *year_dir,
                                       size_t year_dir_sz,
                                       char *telemetry_path,
                                       size_t telemetry_path_sz,
                                       char *terminal_log_path,
                                       size_t terminal_log_path_sz)
{
    rtc_datetime_t dt = {0};
    char suffix[24];

    if (!rtc_get_datetime(&dt))
    {
        print("[SD_LOG][WARN] skip: RTC invalid\r\n");
        return 0U;
    }

    snprintf(suffix, sizeof(suffix), "%04u", dt.year);
    app_sd_join_path(year_dir, year_dir_sz, suffix);

    /*
     * Telemetry is kept as one valid JSON array per month.
     * Raw local diagnostics are kept as one filtered text log per month.
     */
    snprintf(suffix, sizeof(suffix), "%04u/%02u.json", dt.year, dt.month);
    app_sd_join_path(telemetry_path, telemetry_path_sz, suffix);

    snprintf(suffix, sizeof(suffix), "%04u/%02u.log", dt.year, dt.month);
    app_sd_join_path(terminal_log_path, terminal_log_path_sz, suffix);

    return 1U;
}

static uint8_t app_sd_write_all(FIL *file, const char *text, size_t len)
{
    UINT written = 0U;
    FRESULT fr = f_write(file, text, len, &written);

    return (fr == FR_OK && written == len) ? 1U : 0U;
}

static uint8_t app_sd_find_json_array_end(FIL *file, DWORD *close_pos)
{
    DWORD pos = f_size(file);
    char ch = 0;
    UINT read = 0U;

    while (pos > 0U)
    {
        pos--;

        if (f_lseek(file, pos) != FR_OK)
        {
            return 0U;
        }

        if (f_read(file, &ch, 1U, &read) != FR_OK || read != 1U)
        {
            return 0U;
        }

        if (ch == ']')
        {
            *close_pos = pos;
            return 1U;
        }

        if (ch != ' ' && ch != '\r' && ch != '\n' && ch != '\t')
        {
            return 0U;
        }
    }

    return 0U;
}

static uint8_t app_sd_find_last_object_end(FIL *file, DWORD *object_end_pos)
{
    DWORD pos = f_size(file);
    char ch = 0;
    UINT read = 0U;

    while (pos > 0U)
    {
        pos--;

        if (f_lseek(file, pos) != FR_OK)
        {
            return 0U;
        }

        if (f_read(file, &ch, 1U, &read) != FR_OK || read != 1U)
        {
            return 0U;
        }

        if (ch == '}')
        {
            *object_end_pos = pos;
            return 1U;
        }
    }

    return 0U;
}

static FRESULT app_sd_repair_json_array(FIL *file, const char *path)
{
    DWORD object_end_pos = 0U;
    FRESULT fr;

    if (f_size(file) == 0U)
    {
        return FR_OK;
    }

    if (!app_sd_find_last_object_end(file, &object_end_pos))
    {
        print("[SD_LOG][WARN] no valid JSON object found, recreating file=%s\r\n",
              path ? path : "");

        fr = f_lseek(file, 0U);
        if (fr == FR_OK)
        {
            fr = f_truncate(file);
        }
        return fr;
    }

    fr = f_lseek(file, object_end_pos + 1U);
    if (fr != FR_OK)
    {
        return fr;
    }

    fr = f_truncate(file);
    if (fr != FR_OK)
    {
        return fr;
    }

    if (!app_sd_write_all(file, "\r\n]\r\n", 4U))
    {
        return FR_DISK_ERR;
    }

    fr = f_sync(file);
    if (fr == FR_OK)
    {
        print("[SD_LOG][WARN] repaired monthly JSON file=%s\r\n",
              path ? path : "");
    }

    return fr;
}

static uint8_t app_sd_json_array_has_entries(FIL *file, DWORD close_pos)
{
    char ch = 0;
    UINT read = 0U;

    while (close_pos > 0U)
    {
        close_pos--;

        if (f_lseek(file, close_pos) != FR_OK)
        {
            return 0U;
        }

        if (f_read(file, &ch, 1U, &read) != FR_OK || read != 1U)
        {
            return 0U;
        }

        if (ch == '[')
        {
            return 0U;
        }

        if (ch != ' ' && ch != '\r' && ch != '\n' && ch != '\t')
        {
            return 1U;
        }
    }

    return 0U;
}

static char *app_sd_make_local_telemetry_json(const char *payload)
{
    cJSON *root = cJSON_Parse(payload);

    if (!root)
    {
        return NULL;
    }

    cJSON_DeleteItemFromObjectCaseSensitive(root, "apiKey");
    char *local_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    return local_json;
}

static char app_sd_ascii_upper(char ch)
{
    if (ch >= 'a' && ch <= 'z')
    {
        return (char)(ch - ('a' - 'A'));
    }

    return ch;
}

static uint8_t app_sd_contains_ci(const char *text, const char *needle)
{
    size_t needle_len = strlen(needle);

    if (needle_len == 0U)
    {
        return 1U;
    }

    for (size_t i = 0U; text[i] != '\0'; i++)
    {
        size_t j = 0U;

        while (needle[j] != '\0' &&
               text[i + j] != '\0' &&
               app_sd_ascii_upper(text[i + j]) == app_sd_ascii_upper(needle[j]))
        {
            j++;
        }

        if (j == needle_len)
        {
            return 1U;
        }
    }

    return 0U;
}

static const char *app_sd_terminal_line_severity(const char *line)
{
    if (!line)
    {
        return NULL;
    }

    if (strncmp(line, "CFG ", 4U) == 0 ||
        strncmp(line, "SLAVE[", 6U) == 0 ||
        strncmp(line, "  READ[", 7U) == 0 ||
        strncmp(line, "Load from FLASH:", 16U) == 0)
    {
        return NULL;
    }

    if (app_sd_contains_ci(line, "[ERR]") ||
        app_sd_contains_ci(line, "ERROR") ||
        app_sd_contains_ci(line, "FATAL") ||
        app_sd_contains_ci(line, "HARDFAULT") ||
        app_sd_contains_ci(line, "ASSERT"))
    {
        return "ERROR";
    }

    if (app_sd_contains_ci(line, "[WARN]") ||
        app_sd_contains_ci(line, "WARN"))
    {
        return "WARN";
    }

    if (app_sd_contains_ci(line, " failed") ||
        app_sd_contains_ci(line, " failed,") ||
        app_sd_contains_ci(line, " failed:") ||
        app_sd_contains_ci(line, " timeout") ||
        app_sd_contains_ci(line, "TIMEOUT after"))
    {
        return "ERROR";
    }

    return NULL;
}

static uint8_t app_sd_line_starts_with(const char *line, const char *prefix)
{
    if (!line || !prefix)
    {
        return 0U;
    }

    return (strncmp(line, prefix, strlen(prefix)) == 0) ? 1U : 0U;
}

static uint8_t app_sd_terminal_line_is_noisy_transport(const char *line)
{
    if (!line)
    {
        return 1U;
    }

    /*
     * These are low-level modem plumbing lines. They are useful on live UART,
     * but they make the SD error log hard to read and can leak full payloads.
     * Higher-level COMM/MQTT/FLOG lines describe the actual failure.
     */
    if (app_sd_line_starts_with(line, "[AT] timeout=") ||
        app_sd_line_starts_with(line, "[AT] cmd") ||
        app_sd_line_starts_with(line, "[AT] expect") ||
        app_sd_line_starts_with(line, "[AT] task=") ||
        app_sd_line_starts_with(line, "[AT] RX flushed") ||
        app_sd_line_starts_with(line, "[AT] expected_seen") ||
        app_sd_line_starts_with(line, "[AT] IGNORE") ||
        app_sd_line_starts_with(line, "[AT->GSM]") ||
        app_sd_line_starts_with(line, "[GSM->AT]") ||
        app_sd_line_starts_with(line, "[ADC]") ||
        app_sd_line_starts_with(line, "[TRY ") ||
        app_sd_line_starts_with(line, "====") ||
        app_sd_line_starts_with(line, "----") ||
        app_sd_line_starts_with(line, "{") ||
        app_sd_line_starts_with(line, "[FLOG_UPLOAD] json=") ||
        app_sd_contains_ci(line, "COMMAND FAILED, retrying") ||
        app_sd_contains_ci(line, "fail_count="))
    {
        return 1U;
    }

    if (app_sd_line_starts_with(line, "[FLOG_UPLOAD]") &&
        !app_sd_contains_ci(line, "[ERR]") &&
        !app_sd_contains_ci(line, "upload failed") &&
        !app_sd_contains_ci(line, "mark_uploaded failed"))
    {
        return 1U;
    }

    if (app_sd_line_starts_with(line, "[FLOG]") &&
        !app_sd_contains_ci(line, "[ERR]") &&
        !app_sd_contains_ci(line, "[WARN]"))
    {
        return 1U;
    }

    if (app_sd_line_starts_with(line, "[CRITICAL][DEFER]") ||
        app_sd_line_starts_with(line, "[CRITICAL][MERGE]") ||
        app_sd_line_starts_with(line, "[CRITICAL][FLASH]"))
    {
        return 1U;
    }

    return 0U;
}

static const char *app_sd_terminal_explain_line(const char *line)
{
    if (!line)
    {
        return "";
    }

    if (app_sd_contains_ci(line, "[MODBUS][READ_FAIL]"))
    {
        return "Modbus read failed. Check slave id, baud rate, register address, wiring, and sensor power.";
    }

    if (app_sd_contains_ci(line, "[MODBUS][ERR] UART config failed"))
    {
        return "Modbus serial port configuration failed. Check baud rate, parity, stop bits, and firmware config.";
    }

    if (app_sd_contains_ci(line, "[SD][ERR]") ||
        app_sd_contains_ci(line, "[SD_LOG][ERR]"))
    {
        return "SD card operation failed. Local SD backup logging may be disabled until the card mounts again.";
    }

    if (app_sd_contains_ci(line, "[FLOG_UPLOAD][ERR]") ||
        app_sd_contains_ci(line, "mark_uploaded failed"))
    {
        return "Cloud log upload reached MQTT but could not mark the local flash record as uploaded.";
    }

    if (app_sd_contains_ci(line, "upload failed/uncertain"))
    {
        return "Cloud log upload was not confirmed. The log remains pending and will be retried later.";
    }

    if (app_sd_contains_ci(line, "[FLOG][ERR]"))
    {
        return "External flash log storage failed. Some offline error logs may not be preserved.";
    }

    if (app_sd_contains_ci(line, "[FLOG][WARN]"))
    {
        return "External flash log storage warning. Log storage may be full or being cleaned.";
    }

    if (app_sd_contains_ci(line, "MQTT publish failed"))
    {
        return "MQTT publish failed. Device will keep the data/log pending and run network recovery.";
    }

    if (app_sd_contains_ci(line, "MQTT PUB failed"))
    {
        return "MQTT publish command failed inside the modem. Device will retry or reconnect MQTT.";
    }

    if (app_sd_contains_ci(line, "MQTT connect failed"))
    {
        return "MQTT connection failed. Device will retry MQTT, then rebuild GPRS/network if needed.";
    }

    if (app_sd_contains_ci(line, "MQTT Connection Failed") ||
        app_sd_contains_ci(line, "MQTT CONNECT command did not return OK"))
    {
        return "MQTT broker connection was not confirmed by the modem. Device will run communication recovery.";
    }

    if (app_sd_contains_ci(line, "Subscribe FAIL"))
    {
        return "MQTT subscribe failed. Device may be online but unable to receive downlink commands.";
    }

    if (app_sd_contains_ci(line, "MQTT disconnect URC") ||
        app_sd_contains_ci(line, "PDP deactivated") ||
        app_sd_contains_ci(line, "PDN deactivated"))
    {
        return "Modem reported MQTT/mobile data disconnection. Device will reconnect automatically.";
    }

    if (app_sd_contains_ci(line, "GPRS failed") ||
        app_sd_contains_ci(line, "GPRS/PDP failed"))
    {
        return "Mobile data session failed. Device will retry network attach and may reset the modem if it continues.";
    }

    if (app_sd_contains_ci(line, "network registration failed") ||
        app_sd_contains_ci(line, "network attach failed"))
    {
        return "Cellular network registration failed. Check SIM coverage, antenna, and operator availability.";
    }

    if (app_sd_contains_ci(line, "SIM not ready"))
    {
        return "SIM is not ready. Check SIM insertion, SIM activation, and modem SIM interface.";
    }

    if (app_sd_contains_ci(line, "AT sync failed") ||
        app_sd_contains_ci(line, "ATE0 failed"))
    {
        return "Modem did not respond to AT sync. Device will retry and then soft reset the modem.";
    }

    if (app_sd_contains_ci(line, "[MODEM] CSQ read failed") ||
        app_sd_contains_ci(line, "failed to read signal quality"))
    {
        return "Signal quality check failed. Modem did not return a valid CSQ response.";
    }

    if (app_sd_contains_ci(line, "SOFT_RESET"))
    {
        return "Recovery action started: device is resetting the modem/controller to restore communication.";
    }

    if (app_sd_contains_ci(line, "[PAYLOAD][ERR]") ||
        app_sd_contains_ci(line, "telemetry JSON exceeded"))
    {
        return "Telemetry payload became too large. Reduce configured Modbus reads or payload size.";
    }

    if (app_sd_contains_ci(line, "[CONFIG]") ||
        app_sd_contains_ci(line, "config JSON parse failed") ||
        app_sd_contains_ci(line, "modbus config parse failed") ||
        app_sd_contains_ci(line, "config object missing") ||
        app_sd_contains_ci(line, "configId missing"))
    {
        return "Configuration downlink failed validation. Check configId, apiKey, hash, and Modbus slave/register fields.";
    }

    if (app_sd_contains_ci(line, "[DOWNLINK][ERR]") ||
        app_sd_contains_ci(line, "unsupported downlink type"))
    {
        return "Downlink command type is unsupported. Device ignored the command.";
    }

    if (app_sd_contains_ci(line, "[OTA]") ||
        app_sd_contains_ci(line, "OTA_") ||
        app_sd_contains_ci(line, "ota_"))
    {
        return "OTA update failed or was rejected. Check firmware URL, model, version, size, and SHA256.";
    }

    if (app_sd_contains_ci(line, "[MEM]") ||
        app_sd_contains_ci(line, "LOW_STACK") ||
        app_sd_contains_ci(line, "heap"))
    {
        return "Memory warning detected. Heap or task stack is getting low.";
    }

    if (app_sd_contains_ci(line, "HARDFAULT"))
    {
        return "Firmware fault occurred. Device captured crash details and reset for recovery.";
    }

    return "";
}

static void app_sd_terminal_queue_record(const char *severity, const char *line)
{
    char ts[40];
    char record[APP_SD_TERMINAL_LINE_SZ + 160U];
    int n;
    size_t len;
    size_t space;
    const char *explain;

    if (!severity || !line || line[0] == '\0')
    {
        return;
    }

    if (app_sd_terminal_line_is_noisy_transport(line))
    {
        return;
    }

    rtc_get_datetime_strings(ts, sizeof(ts));
    explain = app_sd_terminal_explain_line(line);

    if (explain[0] != '\0')
    {
        n = snprintf(record,
                     sizeof(record),
                     "%s %s %s\r\n",
                     ts,
                     severity,
                     explain);
    }
    else
    {
        n = snprintf(record, sizeof(record), "%s %s %s\r\n", ts, severity, line);
    }

    if (n <= 0)
    {
        return;
    }

    len = (size_t)n;
    if (len >= sizeof(record))
    {
        len = sizeof(record) - 1U;
    }

    space = APP_SD_TERMINAL_LOG_BUFFER_SZ - s_terminal_log_len;
    if (len > space)
    {
        s_terminal_log_dropped += (uint32_t)(len - space);
        len = space;
    }

    if (len > 0U)
    {
        memcpy(&s_terminal_log_buf[s_terminal_log_len], record, len);
        s_terminal_log_len += len;
    }
}

static void app_sd_terminal_process_line(void)
{
    const char *severity;

    if (s_terminal_line_len == 0U)
    {
        return;
    }

    s_terminal_line_buf[s_terminal_line_len] = '\0';
    severity = app_sd_terminal_line_severity(s_terminal_line_buf);

    if (severity)
    {
        app_sd_terminal_queue_record(severity, s_terminal_line_buf);
    }

    s_terminal_line_len = 0U;
}

uint8_t app_sd_self_test_once(void)
{
    static uint8_t s_ran = 0U;
    FIL file;
    FRESULT fr;
    UINT written = 0U;
    char line[160];

    if (s_ran)
    {
        return s_sd_available;
    }
    s_ran = 1U;
    s_sd_checked = 1U;

    print("[SD] self-test start\r\n");

    s_sd_busy = 1U;
    fr = f_mount(&SDFatFS, SDPath, 1);
    if (fr != FR_OK)
    {
        s_sd_busy = 0U;
        print("[SD][ERR] mount failed code=%u\r\n", (unsigned int)fr);
        print("[SD][ERR] card not mounted, SD logging disabled\r\n");
        s_sd_available = 0U;
        return 0U;
    }

    snprintf(line,
             sizeof(line),
             "{\"type\":\"sd_test\",\"uptimeMs\":%lu,\"msg\":\"sd write ok\"}\r\n",
             (unsigned long)HAL_GetTick());

    fr = f_open(&file, APP_SD_TEST_FILE, FA_OPEN_APPEND | FA_WRITE);
    if (fr != FR_OK)
    {
        (void)f_mount(NULL, SDPath, 1);
        s_sd_busy = 0U;
        print("[SD][ERR] open %s failed code=%u\r\n",
              APP_SD_TEST_FILE,
              (unsigned int)fr);
        print("[SD][ERR] card not mounted, SD logging disabled\r\n");
        s_sd_available = 0U;
        return 0U;
    }

    fr = f_write(&file, line, strlen(line), &written);
    if ((fr == FR_OK) && (written == strlen(line)))
    {
        fr = f_sync(&file);
    }

    (void)f_close(&file);
    (void)f_mount(NULL, SDPath, 1);
    s_sd_busy = 0U;

    if (fr != FR_OK || written != strlen(line))
    {
        print("[SD][ERR] write failed code=%u bytes=%u expected=%u\r\n",
              (unsigned int)fr,
              (unsigned int)written,
              (unsigned int)strlen(line));
        print("[SD][ERR] card not mounted, SD logging disabled\r\n");
        s_sd_available = 0U;
        return 0U;
    }

    print("[SD] write ok file=%s bytes=%u\r\n",
          APP_SD_TEST_FILE,
          (unsigned int)written);
    print("[SD] self-test done\r\n");
    s_sd_available = 1U;
    return 1U;
}

uint8_t app_sd_is_available(void)
{
    return (s_sd_checked && s_sd_available) ? 1U : 0U;
}

uint8_t app_sd_append_telemetry_payload(const char *payload)
{
    FIL file;
    FRESULT fr;
    char year_dir[16];
    char telemetry_path[32];
    char terminal_log_path[32];
    char *local_payload;
    DWORD close_pos = 0U;
    uint8_t has_entries = 0U;

    if (!app_sd_is_available() || !payload || payload[0] == '\0')
    {
        return 0U;
    }

    if (!app_sd_make_month_paths(year_dir,
                                 sizeof(year_dir),
                                 telemetry_path,
                                 sizeof(telemetry_path),
                                 terminal_log_path,
                                 sizeof(terminal_log_path)))
    {
        return 0U;
    }

    (void)terminal_log_path;

    local_payload = app_sd_make_local_telemetry_json(payload);
    if (!local_payload)
    {
        print("[SD_LOG][ERR] telemetry JSON sanitize failed\r\n");
        return 0U;
    }

    s_sd_busy = 1U;
    fr = f_mount(&SDFatFS, SDPath, 1);
    if (fr != FR_OK)
    {
        s_sd_busy = 0U;
        s_sd_available = 0U;
        cJSON_free(local_payload);
        print("[SD_LOG][ERR] mount failed code=%u\r\n", (unsigned int)fr);
        return 0U;
    }

    fr = app_sd_mkdir_if_needed(year_dir);
    if (fr != FR_OK)
    {
        (void)f_mount(NULL, SDPath, 1);
        s_sd_busy = 0U;
        s_sd_available = 0U;
        cJSON_free(local_payload);
        print("[SD_LOG][ERR] mkdir failed code=%u path=%s\r\n",
              (unsigned int)fr,
              year_dir);
        return 0U;
    }

    fr = f_open(&file, telemetry_path, FA_OPEN_ALWAYS | FA_READ | FA_WRITE);
    if (fr != FR_OK)
    {
        (void)f_mount(NULL, SDPath, 1);
        s_sd_busy = 0U;
        s_sd_available = 0U;
        cJSON_free(local_payload);
        print("[SD_LOG][ERR] open failed code=%u path=%s\r\n",
              (unsigned int)fr,
              telemetry_path);
        return 0U;
    }

    if (app_sd_find_json_array_end(&file, &close_pos))
    {
        has_entries = app_sd_json_array_has_entries(&file, close_pos);
        fr = f_lseek(&file, close_pos);
    }
    else
    {
        fr = app_sd_repair_json_array(&file, telemetry_path);

        if (fr == FR_OK && app_sd_find_json_array_end(&file, &close_pos))
        {
            has_entries = app_sd_json_array_has_entries(&file, close_pos);
            fr = f_lseek(&file, close_pos);
        }
        else if (fr == FR_OK)
        {
            fr = f_lseek(&file, 0U);
            if (fr == FR_OK)
            {
                fr = f_truncate(&file);
            }
            has_entries = 0U;
        }
    }

    if (fr == FR_OK)
    {
        uint8_t ok;

        if (has_entries)
        {
            ok = app_sd_write_all(&file, ",\r\n", 3U);
        }
        else
        {
            ok = app_sd_write_all(&file, "[\r\n", 3U);
        }

        if (ok)
        {
            ok = app_sd_write_all(&file, local_payload, strlen(local_payload));
        }

        if (ok)
        {
            ok = app_sd_write_all(&file, "\r\n]\r\n", 4U);
        }

        fr = ok ? f_sync(&file) : FR_DISK_ERR;
    }

    (void)f_close(&file);
    (void)f_mount(NULL, SDPath, 1);
    s_sd_busy = 0U;
    cJSON_free(local_payload);

    if (fr != FR_OK)
    {
        s_sd_available = 0U;
        print("[SD_LOG][ERR] append failed code=%u path=%s\r\n",
              (unsigned int)fr,
              telemetry_path);
        return 0U;
    }

    print("[SD_LOG] append ok file=%s\r\n", telemetry_path);
    return 1U;
}

void app_sd_telemetry_service(void)
{
    static uint32_t s_next_due_tick = 0U;
    uint32_t now = HAL_GetTick();

    if (!app_sd_is_available())
    {
        return;
    }

    if (s_next_due_tick != 0U && (int32_t)(now - s_next_due_tick) < 0)
    {
        return;
    }

    if (!app_modbus_payload_has_valid_config())
    {
        return;
    }

    const char *payload = app_build_payload_json_once();
    uint8_t ok = app_sd_append_telemetry_payload(payload);

    print("[SD_LOG] telemetry append %s mode=%s\r\n",
          ok ? "ok" : "failed",
          comm_manager_is_online() ? "online" : "offline");

    s_next_due_tick = now + APP_SD_TELEMETRY_INTERVAL_MS;
    print("[SD_LOG] next telemetry log in %lu ms\r\n",
          (unsigned long)APP_SD_TELEMETRY_INTERVAL_MS);
}

void app_sd_terminal_log_sink(const char *text)
{
    if (!text || !s_terminal_log_enabled || !app_sd_is_available() || s_terminal_log_flush_busy)
    {
        return;
    }

    for (size_t i = 0U; text[i] != '\0'; i++)
    {
        char ch = text[i];

        if (ch == '\r')
        {
            continue;
        }

        if (ch == '\n')
        {
            app_sd_terminal_process_line();
            continue;
        }

        if (s_terminal_line_len < (APP_SD_TERMINAL_LINE_SZ - 1U))
        {
            s_terminal_line_buf[s_terminal_line_len++] = ch;
        }
    }
}

void app_sd_terminal_log_enable(uint8_t enabled)
{
    s_terminal_log_enabled = enabled ? 1U : 0U;
    s_terminal_log_last_flush = 0U;
}

void app_sd_terminal_log_service(void)
{
    FIL file;
    FRESULT fr;
    char year_dir[16];
    char telemetry_path[32];
    char terminal_log_path[32];
    UINT written = 0U;
    uint32_t now = HAL_GetTick();
    size_t len;

    if (!s_terminal_log_enabled || !app_sd_is_available() || s_terminal_log_flush_busy || s_sd_busy || s_terminal_log_len == 0U)
    {
        return;
    }

    if (s_terminal_log_last_flush != 0U &&
        s_terminal_log_len < (APP_SD_TERMINAL_LOG_BUFFER_SZ / 2U) &&
        (now - s_terminal_log_last_flush) < APP_SD_TERMINAL_FLUSH_MS)
    {
        return;
    }

    if (!app_sd_make_month_paths(year_dir,
                                 sizeof(year_dir),
                                 telemetry_path,
                                 sizeof(telemetry_path),
                                 terminal_log_path,
                                 sizeof(terminal_log_path)))
    {
        return;
    }

    (void)telemetry_path;

    s_terminal_log_flush_busy = 1U;
    s_sd_busy = 1U;
    len = s_terminal_log_len;

    fr = f_mount(&SDFatFS, SDPath, 1);
    if (fr != FR_OK)
    {
        s_sd_available = 0U;
    }

    if (fr == FR_OK)
    {
        fr = app_sd_mkdir_if_needed(year_dir);
    }

    if (fr == FR_OK)
    {
        fr = f_open(&file, terminal_log_path, FA_OPEN_APPEND | FA_WRITE);
    }

    if (fr == FR_OK)
    {
        if (s_terminal_log_dropped > 0U)
        {
            char dropped_msg[80];
            int n = snprintf(dropped_msg,
                             sizeof(dropped_msg),
                             "\r\n[SD_LOG][WARN] terminal log dropped bytes=%lu\r\n",
                             (unsigned long)s_terminal_log_dropped);

            if (n > 0)
            {
                UINT dropped_written = 0U;
                (void)f_write(&file, dropped_msg, (UINT)n, &dropped_written);
            }

            s_terminal_log_dropped = 0U;
        }

        fr = f_write(&file, s_terminal_log_buf, len, &written);
        if (fr == FR_OK && written == len)
        {
            fr = f_sync(&file);
        }

        (void)f_close(&file);
    }

    (void)f_mount(NULL, SDPath, 1);

    if (fr == FR_OK && written == len)
    {
        if (s_terminal_log_len > len)
        {
            memmove(s_terminal_log_buf,
                    &s_terminal_log_buf[len],
                    s_terminal_log_len - len);
            s_terminal_log_len -= len;
        }
        else
        {
            s_terminal_log_len = 0U;
        }

        s_terminal_log_last_flush = now;
    }
    else
    {
        s_terminal_log_last_flush = now;
    }

    s_sd_busy = 0U;
    s_terminal_log_flush_busy = 0U;
}
