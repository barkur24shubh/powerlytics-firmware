#include "app_flash_log.h"

#include "cmsis_os.h"
#include "stm32f4xx_hal.h"

#include "debug_uart.h"
#include "mqtt_handler.h"
#include "rtc_manager.h"
#include "comm_manager.h"
#include "app_modbus_payload.h"
#include "app_health.h"
#include "app_sd_card.h"
#include "app_firmware.h"

#include <stdio.h>
#include <string.h>
#include <stddef.h>

#define APP_FLASH_LOG_MAGIC           0x4C4F4731UL   /* LOG1 */
#define APP_FLASH_LOG_VERSION         1UL

/*
 * W25Q16 = 2MB.
 * Keep logs away from future low-address config/metadata space.
 *
 * 0x010000..0x04FFFF = 256KB log area
 * Slot size 512 bytes => 512 persistent log slots.
 */
#define APP_FLASH_LOG_BASE_ADDR       0x010000UL
#define APP_FLASH_LOG_AREA_SIZE       (256UL * 1024UL)
#define APP_FLASH_LOG_SLOT_SIZE       512UL
#define APP_FLASH_LOG_SLOT_COUNT      (APP_FLASH_LOG_AREA_SIZE / APP_FLASH_LOG_SLOT_SIZE)

#define APP_FLASH_LOG_UPLOADED_MARK_PENDING  0xFFFFFFFFUL
#define APP_FLASH_LOG_UPLOADED_MARK_DONE     0x00000000UL

#define APP_FLASH_LOG_JSON_LEN        1024U

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t record_size;
    uint32_t crc32;

    uint32_t seq;
    uint32_t tick_ms;
    uint32_t uploaded_mark;

    uint8_t  level;
    uint8_t  reserved[3];

    int32_t value1;
    int32_t value2;

    char ts[24];
    char module[12];
    char event[32];
    char message[96];

    char comm_state[20];
    char last_error[32];
    char recovery[24];
    char config_id[40];

    uint32_t first_seen_tick;

    uint8_t reserved_tail[512 - 4*7 - 4 - 4*2 - 24 - 12 - 32 - 96 - 20 - 32 - 24 - 40 - 4];
} app_flash_log_record_t;

typedef char app_flash_log_record_size_check[
    (sizeof(app_flash_log_record_t) == APP_FLASH_LOG_SLOT_SIZE) ? 1 : -1
];

static W25Q_Handle *s_flash = NULL;
static uint8_t  s_ready = 0;

static uint32_t s_seq = 0;
static uint16_t s_next_write_slot = 0;
static uint16_t s_pending_count = 0;
static uint16_t s_total_count = 0;
static uint32_t s_dropped_count = 0;
static uint32_t s_deduped_count = 0;
static uint32_t s_slot_merge_count[APP_FLASH_LOG_SLOT_COUNT];

static uint32_t s_last_upload_attempt_tick = 0;

static osMutexId_t s_flash_log_mutex = NULL;

extern char imei[20];
extern char topic_d2c_logs[64];

static uint8_t flash_log_rtos_running(void)
{
    return (osKernelGetState() == osKernelRunning) ? 1U : 0U;
}

static void flash_log_lock(void)
{
    if (flash_log_rtos_running()) {
        if (s_flash_log_mutex == NULL) {
            s_flash_log_mutex = osMutexNew(NULL);
        }

        if (s_flash_log_mutex) {
            osMutexAcquire(s_flash_log_mutex, osWaitForever);
        }
    }
}

static void flash_log_unlock(void)
{
    if (flash_log_rtos_running() && s_flash_log_mutex) {
        osMutexRelease(s_flash_log_mutex);
    }
}

static uint32_t crc32_sw(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFUL;

    for (uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (uint8_t b = 0; b < 8; b++) {
            uint32_t mask = (uint32_t)(-(int32_t)(crc & 1UL));
            crc = (crc >> 1) ^ (0xEDB88320UL & mask);
        }
    }

    return ~crc;
}

static void safe_copy(char *dst, uint32_t dst_len, const char *src)
{
    if (!dst || dst_len == 0U) return;

    if (!src) {
        dst[0] = '\0';
        return;
    }

    strncpy(dst, src, dst_len - 1U);
    dst[dst_len - 1U] = '\0';
}

/*
 * Minimal JSON string sanitizer.
 * Replaces quotes/backslash/control chars so log upload JSON stays valid.
 */
static void json_escape(char *dst, uint32_t dst_len, const char *src)
{
    uint32_t di = 0;

    if (!dst || dst_len == 0U) return;
    dst[0] = '\0';

    if (!src) return;

    for (uint32_t si = 0; src[si] != '\0' && di + 1U < dst_len; si++) {
        char c = src[si];

        if (c == '"' || c == '\\') {
            if (di + 2U >= dst_len) break;
            dst[di++] = '\\';
            dst[di++] = c;
        } else if ((uint8_t)c < 0x20U) {
            dst[di++] = ' ';
        } else {
            dst[di++] = c;
        }
    }

    dst[di] = '\0';
}

static void extract_stage(char *dst, uint32_t dst_len, const char *message)
{
    const char *p;
    uint32_t i = 0U;

    if (!dst || dst_len == 0U) return;
    dst[0] = '\0';

    if (!message) {
        safe_copy(dst, dst_len, "NONE");
        return;
    }

    p = strstr(message, "stage=");
    if (!p) {
        safe_copy(dst, dst_len, "NONE");
        return;
    }

    p += 6;
    while (*p != '\0' && *p != ' ' && *p != ',' && i + 1U < dst_len) {
        dst[i++] = *p++;
    }

    dst[i] = '\0';

    if (dst[0] == '\0') {
        safe_copy(dst, dst_len, "NONE");
    }
}

static const char *log_status_from_level(app_log_level_t level)
{
    switch (level) {
    case APP_LOG_INFO:
        return "event";
    case APP_LOG_WARN:
    case APP_LOG_ERROR:
    case APP_LOG_FATAL:
        return "open";
    default:
        return "open";
    }
}

static void get_signal_quality(char *dst, uint32_t dst_len)
{
    app_signal_snapshot_t sig;

    if (!dst || dst_len == 0U) return;

    if (app_health_get_signal_snapshot(&sig) && sig.quality) {
        safe_copy(dst, dst_len, sig.quality);
    } else {
        safe_copy(dst, dst_len, "unknown");
    }
}

static uint32_t record_first_seen_tick(const app_flash_log_record_t *r)
{
    if (!r) return 0U;

    if (r->first_seen_tick == 0U || r->first_seen_tick == 0xFFFFFFFFUL) {
        return r->tick_ms;
    }

    return r->first_seen_tick;
}

static void module_status_from_error(const char *last_error,
                                     const char **sim,
                                     const char **net,
                                     const char **gprs,
                                     const char **mqtt)
{
    *sim = "unknown";
    *net = "unknown";
    *gprs = "unknown";
    *mqtt = "unknown";

    if (!last_error || last_error[0] == '\0' || strcmp(last_error, "NONE") == 0) {
        *sim = "ok";
        *net = "ok";
        *gprs = "ok";
        *mqtt = "ok";
        return;
    }

    if (strcmp(last_error, "SIM_NOT_READY") == 0) {
        *sim = "fail";
        return;
    }

    if (strcmp(last_error, "NETWORK_NOT_REGISTERED") == 0) {
        *sim = "ok";
        *net = "fail";
        return;
    }

    if (strcmp(last_error, "GPRS_FAILED") == 0) {
        *sim = "ok";
        *net = "ok";
        *gprs = "fail";
        return;
    }

    if (strstr(last_error, "MQTT")) {
        *sim = "ok";
        *net = "ok";
        *gprs = "ok";
        *mqtt = "fail";
        return;
    }

    if (strcmp(last_error, "AT_TIMEOUT") == 0) {
        *sim = "unknown";
        *net = "unknown";
        *gprs = "unknown";
        *mqtt = "unknown";
    }
}


static uint32_t slot_addr(uint16_t slot)
{
    return APP_FLASH_LOG_BASE_ADDR + ((uint32_t)slot * APP_FLASH_LOG_SLOT_SIZE);
}

static uint8_t record_is_erased(const app_flash_log_record_t *r)
{
    return (r->magic == 0xFFFFFFFFUL) ? 1U : 0U;
}

static uint8_t record_crc_ok(const app_flash_log_record_t *r)
{
    if (r->magic != APP_FLASH_LOG_MAGIC) return 0U;
    if (r->version != APP_FLASH_LOG_VERSION) return 0U;
    if (r->record_size != APP_FLASH_LOG_SLOT_SIZE) return 0U;

    app_flash_log_record_t tmp;
    memcpy(&tmp, r, sizeof(tmp));
    uint32_t stored = tmp.crc32;
    tmp.crc32 = 0;

    return (crc32_sw(&tmp, sizeof(tmp)) == stored) ? 1U : 0U;
}

static uint8_t same_text(const char *a, const char *b)
{
    return (strcmp(a ? a : "", b ? b : "") == 0) ? 1U : 0U;
}

static uint8_t find_pending_duplicate_locked(const char *module,
                                             const char *event,
                                             const char *message,
                                             const char *recovery,
                                             const char *config_id,
                                             uint16_t *slot_out,
                                             app_flash_log_record_t *rec_out)
{
    app_flash_log_record_t r;

    for (uint16_t i = 0; i < APP_FLASH_LOG_SLOT_COUNT; i++) {
        memset(&r, 0, sizeof(r));

        if (!w25q_read(s_flash, slot_addr(i), (uint8_t *)&r, sizeof(r))) {
            continue;
        }

        if (!record_crc_ok(&r)) {
            continue;
        }

        if (r.uploaded_mark == APP_FLASH_LOG_UPLOADED_MARK_DONE) {
            continue;
        }

        if (same_text(r.module, module) &&
            same_text(r.event, event) &&
            same_text(r.message, message) &&
            same_text(r.recovery, recovery) &&
            same_text(r.config_id, config_id)) {
            if (slot_out) {
                *slot_out = i;
            }

            if (rec_out) {
                *rec_out = r;
            }

            return 1U;
        }
    }

    return 0U;
}

static int32_t effective_record_count(uint16_t slot, const app_flash_log_record_t *r)
{
    uint32_t stored_count = 1UL;

    if (r && r->value1 > 0) {
        stored_count = (uint32_t)r->value1;
    }

    if (slot < APP_FLASH_LOG_SLOT_COUNT && s_slot_merge_count[slot] > stored_count) {
        stored_count = s_slot_merge_count[slot];
    }

    if (stored_count > 0x7FFFFFFFUL) {
        stored_count = 0x7FFFFFFFUL;
    }

    return (int32_t)stored_count;
}

static void erase_log_region(void)
{
    if (!s_flash) return;

    for (uint32_t addr = APP_FLASH_LOG_BASE_ADDR;
         addr < (APP_FLASH_LOG_BASE_ADDR + APP_FLASH_LOG_AREA_SIZE);
         addr += W25Q_SECTOR_SIZE) {
        (void)w25q_erase_sector_4k(s_flash, addr);
        osDelay(5);
    }

    s_next_write_slot = 0;
    s_pending_count = 0;
    s_total_count = 0;
    memset(s_slot_merge_count, 0, sizeof(s_slot_merge_count));
}

static void scan_log_region(void)
{
    app_flash_log_record_t r;

    s_next_write_slot = APP_FLASH_LOG_SLOT_COUNT;
    s_pending_count = 0;
    s_total_count = 0;
    s_seq = 0;
    memset(s_slot_merge_count, 0, sizeof(s_slot_merge_count));

    for (uint16_t i = 0; i < APP_FLASH_LOG_SLOT_COUNT; i++) {
        memset(&r, 0, sizeof(r));

        if (!w25q_read(s_flash, slot_addr(i), (uint8_t *)&r, sizeof(r))) {
            continue;
        }

        if (record_is_erased(&r)) {
            if (s_next_write_slot == APP_FLASH_LOG_SLOT_COUNT) {
                s_next_write_slot = i;
            }
            continue;
        }

        if (record_crc_ok(&r)) {
            s_total_count++;

            if (r.uploaded_mark != APP_FLASH_LOG_UPLOADED_MARK_DONE) {
                s_pending_count++;
                s_slot_merge_count[i] = (r.value1 > 0) ? (uint32_t)r.value1 : 1UL;
            }

            if (r.seq > s_seq) {
                s_seq = r.seq;
            }
        }
    }

    if (s_next_write_slot == APP_FLASH_LOG_SLOT_COUNT) {
        print("[FLOG][WARN] log region full, erasing all uploaded/pending logs\r\n");
        s_dropped_count += s_pending_count;
        erase_log_region();
    }
}

uint8_t app_flash_log_init(W25Q_Handle *flash)
{
    s_flash = flash;
    s_ready = 0;

    /*
     * Do not create/acquire RTOS mutex before scheduler starts.
     * It can leave BASEPRI masked or hang before osKernelStart().
     */
    if (flash_log_rtos_running() && s_flash_log_mutex == NULL) {
        s_flash_log_mutex = osMutexNew(NULL);
    }

    if (!s_flash) {
        print("[FLOG][ERR] flash handle null\r\n");
        return 0U;
    }

    uint32_t id = w25q_read_jedec_id(s_flash);
    if (id == 0U || id == 0xFFFFFFUL) {
        print("[FLOG][ERR] W25Q not detected\r\n");
        return 0U;
    }

    scan_log_region();

    s_ready = 1U;

    print("[FLOG] init ok pending=%u total=%u nextSlot=%u\r\n",
          (unsigned)s_pending_count,
          (unsigned)s_total_count,
          (unsigned)s_next_write_slot);

    return 1U;
}

uint8_t app_flash_log_is_ready(void)
{
    return s_ready;
}

uint8_t app_flash_log_write_incident(app_log_level_t level,
                                     const char *module,
                                     const char *event,
                                     const char *message,
                                     int32_t value1,
                                     int32_t value2,
                                     const char *recovery,
                                     uint32_t first_seen_tick)
{
    if (!s_ready || !s_flash) return 0U;

    if (!app_modbus_payload_has_valid_config()) {
        print("[FLOG] write suppressed: waiting for valid config/apiKey module=%s event=%s\r\n",
              module ? module : "",
              event ? event : "");
        return 0U;
    }

    flash_log_lock();

    char cfg_id[40];
    safe_copy(cfg_id, sizeof(cfg_id), app_modbus_payload_get_config_id());

    uint16_t duplicate_slot = 0U;
    app_flash_log_record_t duplicate;
    memset(&duplicate, 0, sizeof(duplicate));

    if (find_pending_duplicate_locked(module,
                                      event,
                                      message,
                                      recovery ? recovery : "NONE",
                                      cfg_id,
                                      &duplicate_slot,
                                      &duplicate)) {
        uint32_t merged_count = (s_slot_merge_count[duplicate_slot] > 0U)
                                ? s_slot_merge_count[duplicate_slot]
                                : ((duplicate.value1 > 0) ? (uint32_t)duplicate.value1 : 1UL);
        uint32_t incoming_count = (value1 > 0) ? (uint32_t)value1 : (merged_count + 1UL);

        if (incoming_count > merged_count) {
            merged_count = incoming_count;
        } else if (merged_count < 0x7FFFFFFFUL) {
            merged_count++;
        }

        s_slot_merge_count[duplicate_slot] = merged_count;
        s_deduped_count++;

        print("[FLOG][DEDUP] merged module=%s event=%s slot=%u count=%lu deduped=%lu\r\n",
              module ? module : "",
              event ? event : "",
              duplicate_slot,
              (unsigned long)merged_count,
              (unsigned long)s_deduped_count);

        flash_log_unlock();
        return 1U;
    }

    if (s_next_write_slot >= APP_FLASH_LOG_SLOT_COUNT) {
        print("[FLOG][WARN] no free slot, erasing log region\r\n");
        s_dropped_count += s_pending_count;
        erase_log_region();
    }

    app_flash_log_record_t r;
    memset(&r, 0xFF, sizeof(r));

    r.magic = APP_FLASH_LOG_MAGIC;
    r.version = APP_FLASH_LOG_VERSION;
    r.record_size = APP_FLASH_LOG_SLOT_SIZE;
    r.crc32 = 0;

    r.seq = ++s_seq;
    r.tick_ms = HAL_GetTick();
    r.uploaded_mark = APP_FLASH_LOG_UPLOADED_MARK_PENDING;
    r.level = (uint8_t)level;
    r.value1 = value1;
    r.value2 = value2;
    r.first_seen_tick = (first_seen_tick != 0U) ? first_seen_tick : r.tick_ms;

    rtc_get_datetime_strings(r.ts, sizeof(r.ts));

    safe_copy(r.module, sizeof(r.module), module);
    safe_copy(r.event, sizeof(r.event), event);
    safe_copy(r.message, sizeof(r.message), message);
    safe_copy(r.recovery, sizeof(r.recovery), recovery ? recovery : "NONE");

    safe_copy(r.comm_state,
              sizeof(r.comm_state),
              comm_manager_state_name(comm_manager_get_state()));

    safe_copy(r.last_error,
              sizeof(r.last_error),
              comm_manager_error_name(comm_manager_get_last_error()));

    safe_copy(r.config_id,
              sizeof(r.config_id),
              cfg_id);

    r.crc32 = crc32_sw(&r, sizeof(r));

    uint16_t written_slot = s_next_write_slot;
    uint32_t addr = slot_addr(written_slot);
    uint8_t ok = w25q_write(s_flash, addr, (const uint8_t *)&r, sizeof(r)) ? 1U : 0U;

    if (ok) {
        s_slot_merge_count[written_slot] = (r.value1 > 0) ? (uint32_t)r.value1 : 1UL;
        s_next_write_slot++;
        s_pending_count++;
        s_total_count++;

        print("[FLOG] stored seq=%lu level=%s module=%s event=%s pending=%u\r\n",
              (unsigned long)r.seq,
              app_log_level_name(level),
              r.module,
              r.event,
              (unsigned)s_pending_count);
    } else {
        print("[FLOG][ERR] write failed\r\n");
    }

    flash_log_unlock();

    return ok;
}

uint8_t app_flash_log_write(app_log_level_t level,
                            const char *module,
                            const char *event,
                            const char *message,
                            int32_t value1,
                            int32_t value2,
                            const char *recovery)
{
    return app_flash_log_write_incident(level,
                                        module,
                                        event,
                                        message,
                                        value1,
                                        value2,
                                        recovery,
                                        0U);
}

static uint8_t read_oldest_pending(uint16_t *slot_out, app_flash_log_record_t *rec_out)
{
    app_flash_log_record_t r;

    if (!slot_out || !rec_out) return 0U;

    for (uint16_t i = 0; i < APP_FLASH_LOG_SLOT_COUNT; i++) {
        memset(&r, 0, sizeof(r));

        if (!w25q_read(s_flash, slot_addr(i), (uint8_t *)&r, sizeof(r))) {
            continue;
        }

        if (!record_crc_ok(&r)) {
            continue;
        }

        if (r.uploaded_mark == APP_FLASH_LOG_UPLOADED_MARK_DONE) {
            continue;
        }

        *slot_out = i;
        *rec_out = r;
        return 1U;
    }

    return 0U;
}

static uint8_t mark_uploaded(uint16_t slot)
{
    uint32_t done = APP_FLASH_LOG_UPLOADED_MARK_DONE;
    uint32_t addr = slot_addr(slot) + (uint32_t)offsetof(app_flash_log_record_t, uploaded_mark);

    /*
     * This changes uploaded_mark from 0xFFFFFFFF to 0x00000000.
     * Flash allows 1->0 programming without sector erase.
     */
    return w25q_write(s_flash, addr, (const uint8_t *)&done, sizeof(done)) ? 1U : 0U;
}

uint8_t app_flash_log_upload_one(void)
{
    if (!s_ready || !s_flash) {
        print("[FLOG_UPLOAD] not ready\r\n");
        return 0U;
    }

    if (!app_modbus_payload_has_valid_config()) {
        print("[FLOG_UPLOAD] skipped: waiting for valid config/apiKey\r\n");
        return 0U;
    }

    if (!comm_manager_is_online()) {
        print("[FLOG_UPLOAD] comm not online\r\n");
        return 0U;
    }

    if (topic_d2c_logs[0] == '\0') {
        print("[FLOG_UPLOAD][ERR] logs topic empty\r\n");
        return 0U;
    }

    app_flash_log_record_t r;
    uint16_t slot = 0;

    print("[FLOG_UPLOAD] enter pending=%u total=%u topic=%s\r\n",
          app_flash_log_pending_count(),
          app_flash_log_total_count(),
          topic_d2c_logs);

    flash_log_lock();

    uint8_t found = read_oldest_pending(&slot, &r);

    flash_log_unlock();

    if (!found) {
        print("[FLOG_UPLOAD] no pending record found\r\n");
        return 0U;
    }

    char msg[128];
    char module[24];
    char event[48];
    char recovery[40];
    char stage[24];
    char signal[16];
    char last_error[48];
    const char *sim_status;
    const char *net_status;
    const char *gprs_status;
    const char *mqtt_status;
    const char *sd_status = app_sd_is_available() ? "mounted" : "not_mounted";

    json_escape(msg, sizeof(msg), r.message);
    json_escape(module, sizeof(module), r.module);
    json_escape(event, sizeof(event), r.event);
    json_escape(recovery, sizeof(recovery), r.recovery);
    json_escape(last_error, sizeof(last_error), r.last_error);
    extract_stage(stage, sizeof(stage), r.message);
    get_signal_quality(signal, sizeof(signal));
    module_status_from_error(last_error,
                             &sim_status,
                             &net_status,
                             &gprs_status,
                             &mqtt_status);

    const char *api_key = app_modbus_payload_get_api_key();
    if (!api_key || api_key[0] == '\0') {
        api_key = "";
    }

    char api_key_esc[192];
    json_escape(api_key_esc, sizeof(api_key_esc), api_key);

    char json[APP_FLASH_LOG_JSON_LEN];

    snprintf(json, sizeof(json),
             "{"
             "\"type\":\"log\","
             "\"v\":2,"
             "\"apiKey\":\"%s\","
             "\"imei\":\"%s\","
             "\"fid\":\"%s-%lu\","
             "\"s\":\"%s\","
             "\"ts\":\"%s\","
             "\"up\":%lu,"
             "\"fw\":\"%s\","
             "\"lvl\":\"%s\","
             "\"module\":\"%s\","
             "\"event\":\"%s\","
             "\"msg\":\"%s\","
             "\"cnt\":%ld,"
             "\"stage\":\"%s\","
             "\"action\":\"%s\","
             "\"signal\":\"%s\","
             "\"mod\":{"
                "\"sim\":\"%s\","
                "\"net\":\"%s\","
                "\"gprs\":\"%s\","
                "\"mqtt\":\"%s\","
                "\"mb\":\"ok\","
                "\"flash\":\"ok\","
                "\"rtc\":\"ok\","
                "\"wdg\":\"ok\","
                "\"sd\":\"%s\""
             "}"
             "}",
             api_key_esc,
             imei,
             event,
             (unsigned long)record_first_seen_tick(&r),
             log_status_from_level((app_log_level_t)r.level),
             r.ts,
             (unsigned long)r.tick_ms,
             APP_FIRMWARE_VERSION,
             app_log_level_name((app_log_level_t)r.level),
             module,
             event,
             msg,
             (long)effective_record_count(slot, &r),
             stage,
             recovery,
             signal,
             sim_status,
             net_status,
             gprs_status,
             mqtt_status,
             sd_status);

    print("[FLOG_UPLOAD] uploading log slot=%u seq=%lu module=%s event=%s count=%ld jsonLen=%u\r\n",
          slot,
          (unsigned long)r.seq,
          module,
          event,
          (long)effective_record_count(slot, &r),
          (unsigned)strlen(json));

    print("[FLOG_UPLOAD] before MQTT publish\r\n");

    uint8_t ok = MQTT_Publish(topic_d2c_logs, json);

    print("[FLOG_UPLOAD] after MQTT publish ok=%u\r\n", ok);

    if (!ok) {
        print("[FLOG_UPLOAD] upload failed/uncertain, keeping pending seq=%lu\r\n",
              (unsigned long)r.seq);
        return 0U;
    }

    flash_log_lock();

    uint8_t marked = mark_uploaded(slot);

    if (marked) {
        if (s_pending_count > 0U) {
            s_pending_count--;
        }

        s_slot_merge_count[slot] = 0U;

        print("[FLOG_UPLOAD] uploaded+cleared seq=%lu pending=%u\r\n",
              (unsigned long)r.seq,
              (unsigned)s_pending_count);
    } else {
        print("[FLOG_UPLOAD][ERR] uploaded but mark_uploaded failed seq=%lu\r\n",
              (unsigned long)r.seq);
    }

    flash_log_unlock();

    return marked;
}
uint8_t app_flash_log_upload_due(void)
{
    uint32_t now = HAL_GetTick();

    if (app_flash_log_pending_count() == 0U) {
        return 0U;
    }

    if ((now - s_last_upload_attempt_tick) < APP_FLASH_LOG_UPLOAD_INTERVAL_MS) {
        return 0U;
    }

    s_last_upload_attempt_tick = now;

    return app_flash_log_upload_one();
}

uint16_t app_flash_log_pending_count(void)
{
    uint16_t c;

    if (s_flash_log_mutex) {
        osMutexAcquire(s_flash_log_mutex, osWaitForever);
    }

    c = s_pending_count;

    if (s_flash_log_mutex) {
        osMutexRelease(s_flash_log_mutex);
    }

    return c;
}

uint16_t app_flash_log_total_count(void)
{
    uint16_t c;

    if (s_flash_log_mutex) {
        osMutexAcquire(s_flash_log_mutex, osWaitForever);
    }

    c = s_total_count;

    if (s_flash_log_mutex) {
        osMutexRelease(s_flash_log_mutex);
    }

    return c;
}

uint32_t app_flash_log_dropped_count(void)
{
    return s_dropped_count;
}

uint32_t app_flash_log_deduped_count(void)
{
    return s_deduped_count;
}
