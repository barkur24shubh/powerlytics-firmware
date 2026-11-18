#include "app_log.h"

#include "cmsis_os.h"
#include "stm32f4xx_hal.h"

#include "debug_uart.h"
#include "connectivity.h"
#include "app_flash_log.h"
#include "app_modbus_payload.h"

#include <stdio.h>
#include <string.h>

#define APP_LOG_QUEUE_SIZE       16U
#define APP_LOG_MODULE_LEN       12U
#define APP_LOG_EVENT_LEN        32U
#define APP_LOG_MESSAGE_LEN      96U
#define APP_LOG_JSON_LEN         384U
#define APP_CRITICAL_DEFER_Q_SIZE 8
#define APP_INCIDENT_CACHE_SIZE  8U
#define APP_INCIDENT_UPDATE_INTERVAL_MS (30UL * 60UL * 1000UL)

typedef struct {
    uint8_t level;
    char module[12];
    char event[28];
    char message[96];
    char recovery[32];
    int32_t value1;
    int32_t value2;
    uint32_t first_seen_tick;
    uint8_t used;
} app_deferred_critical_t;

static app_deferred_critical_t s_crit_q[APP_CRITICAL_DEFER_Q_SIZE];
static uint8_t s_crit_wr = 0;
static uint8_t s_crit_rd = 0;
static uint8_t s_crit_count = 0;
static uint32_t s_crit_drop = 0;
static uint32_t s_last_flash_write_tick = 0;

typedef struct {
    uint8_t used;
    app_log_level_t level;
    uint32_t count;
    uint32_t first_seen_tick;
    uint32_t last_emit_tick;
    char module[12];
    char event[32];
    char recovery[32];
} app_incident_cache_t;

static app_incident_cache_t s_incidents[APP_INCIDENT_CACHE_SIZE];

#define APP_CRITICAL_FLASH_WRITE_INTERVAL_MS 30000UL

typedef struct {
    uint32_t seq;
    uint32_t tick_ms;
    app_log_level_t level;
    char module[APP_LOG_MODULE_LEN];
    char event[APP_LOG_EVENT_LEN];
    char message[APP_LOG_MESSAGE_LEN];
    int32_t value1;
    int32_t value2;
} app_log_event_t;

static app_log_event_t s_log_q[APP_LOG_QUEUE_SIZE];

static uint8_t  s_head = 0;
static uint8_t  s_tail = 0;
static uint8_t  s_count = 0;

static uint32_t s_seq = 0;
static uint32_t s_dropped = 0;

static osMutexId_t s_log_mutex = NULL;

extern char topic_d2c_logs[64];

static void safe_copy(char *dst, uint32_t dst_sz, const char *src)
{
    if (!dst || dst_sz == 0U) return;

    if (!src) {
        dst[0] = '\0';
        return;
    }

    snprintf(dst, dst_sz, "%s", src);
}

static const char *normalize_incident_event(const char *module, const char *event)
{
    if (!event || event[0] == '\0') {
        return "UNKNOWN";
    }

    if (module && strcmp(module, "MQTT") == 0) {
        if (strstr(event, "PUBLISH")) {
            return "MQTT_PUBLISH_FAILED";
        }

        if (strstr(event, "SUBSCRIBE")) {
            return "MQTT_SUBSCRIBE_FAILED";
        }

        if (strstr(event, "CONNECT")) {
            return "MQTT_CONNECT_FAILED";
        }

        if (strstr(event, "DISCONNECT") ||
            strstr(event, "CONN_LOST") ||
            strstr(event, "PDP_DEACT") ||
            strstr(event, "PDN_DEACT")) {
            return "MQTT_DISCONNECTED";
        }
    }

    if (strstr(event, "SIM")) {
        return "SIM_NOT_READY";
    }

    if (strstr(event, "NETWORK")) {
        return "NETWORK_NOT_REGISTERED";
    }

    if (strstr(event, "GPRS")) {
        return "GPRS_FAILED";
    }

    if (strstr(event, "AT_TIMEOUT") || strstr(event, "AT_STILL")) {
        return "AT_TIMEOUT";
    }

    return event;
}

static app_incident_cache_t *find_or_alloc_incident(const char *module, const char *event)
{
    app_incident_cache_t *free_slot = NULL;

    for (uint8_t i = 0; i < APP_INCIDENT_CACHE_SIZE; i++) {
        if (!s_incidents[i].used) {
            if (!free_slot) {
                free_slot = &s_incidents[i];
            }
            continue;
        }

        if ((strcmp(s_incidents[i].module, module ? module : "") == 0) &&
            (strcmp(s_incidents[i].event, event ? event : "") == 0)) {
            return &s_incidents[i];
        }
    }

    if (!free_slot) {
        free_slot = &s_incidents[0];
    }

    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->used = 1U;
    safe_copy(free_slot->module, sizeof(free_slot->module), module);
    safe_copy(free_slot->event, sizeof(free_slot->event), event);

    return free_slot;
}

const char *app_log_level_name(app_log_level_t level)
{
    switch (level) {
    case APP_LOG_INFO:  return "INFO";
    case APP_LOG_WARN:  return "WARN";
    case APP_LOG_ERROR: return "ERROR";
    case APP_LOG_FATAL: return "FATAL";
    default:            return "UNKNOWN";
    }
}

void app_log_init(void)
{
    memset(s_log_q, 0, sizeof(s_log_q));
    memset(s_incidents, 0, sizeof(s_incidents));

    s_head = 0;
    s_tail = 0;
    s_count = 0;
    s_seq = 0;
    s_dropped = 0;

    if (s_log_mutex == NULL) {
        s_log_mutex = osMutexNew(NULL);
    }

    print("[LOG] init ok\r\n");
}

void app_log_push(app_log_level_t level,
                  const char *module,
                  const char *event,
                  const char *message,
                  int32_t value1,
                  int32_t value2)
{
    if (s_log_mutex) {
        osMutexAcquire(s_log_mutex, osWaitForever);
    }

    /*
     * If full, drop oldest.
     * Latest error is usually more useful during field debugging.
     */
    if (s_count >= APP_LOG_QUEUE_SIZE) {
        s_tail = (uint8_t)((s_tail + 1U) % APP_LOG_QUEUE_SIZE);
        s_count--;
        s_dropped++;
    }

    app_log_event_t *e = &s_log_q[s_head];

    memset(e, 0, sizeof(*e));

    e->seq = ++s_seq;
    e->tick_ms = HAL_GetTick();
    e->level = level;
    e->value1 = value1;
    e->value2 = value2;

    safe_copy(e->module, sizeof(e->module), module);
    safe_copy(e->event, sizeof(e->event), event);
    safe_copy(e->message, sizeof(e->message), message);

    s_head = (uint8_t)((s_head + 1U) % APP_LOG_QUEUE_SIZE);
    s_count++;

    if (s_log_mutex) {
        osMutexRelease(s_log_mutex);
    }

    print("[LOG][%s][%s] %s\r\n",
          app_log_level_name(level),
          event ? event : "",
          message ? message : "");
}

static uint8_t app_log_peek(app_log_event_t *out)
{
    if (!out) return 0;

    if (s_log_mutex) {
        osMutexAcquire(s_log_mutex, osWaitForever);
    }

    if (s_count == 0) {
        if (s_log_mutex) {
            osMutexRelease(s_log_mutex);
        }
        return 0;
    }

    *out = s_log_q[s_tail];

    if (s_log_mutex) {
        osMutexRelease(s_log_mutex);
    }

    return 1;
}

static void app_log_pop(void)
{
    if (s_log_mutex) {
        osMutexAcquire(s_log_mutex, osWaitForever);
    }

    if (s_count > 0) {
        s_tail = (uint8_t)((s_tail + 1U) % APP_LOG_QUEUE_SIZE);
        s_count--;
    }

    if (s_log_mutex) {
        osMutexRelease(s_log_mutex);
    }
}

uint8_t app_log_flush_one(void)
{
    app_log_event_t e;
    char json[APP_LOG_JSON_LEN];

    if (!app_modbus_payload_has_valid_config()) {
        return 0;
    }

    if (!app_log_peek(&e)) {
        return 0;
    }

    snprintf(json, sizeof(json),
             "{"
             "\"type\":\"log\","
             "\"seq\":%lu,"
             "\"tsMs\":%lu,"
             "\"level\":\"%s\","
             "\"module\":\"%s\","
             "\"event\":\"%s\","
             "\"message\":\"%s\","
             "\"value1\":%ld,"
             "\"value2\":%ld,"
             "\"dropped\":%lu"
             "}",
             (unsigned long)e.seq,
             (unsigned long)e.tick_ms,
             app_log_level_name(e.level),
             e.module,
             e.event,
             e.message,
             (long)e.value1,
             (long)e.value2,
             (unsigned long)s_dropped);

    /*
     * Logs are best-effort.
     *
     * Important:
     * Sometimes cloud receives the log but firmware misses the +CMQTTPUB ACK.
     * If we only pop on publish success, the same log is sent again and again.
     *
     * So for logs:
     * - attempt publish once
     * - always pop from RAM queue after attempt
     *
     * This prevents duplicate logs and prevents logs from starving telemetry.
     */
    uint8_t ok = connectivity_publish(topic_d2c_logs, json);

    app_log_pop();

    if (!ok) {
        print("[LOG] publish uncertain, popped to avoid duplicate\r\n");
        return 0;
    }

    return 1;
}

uint8_t app_log_flush_all(uint8_t max_to_send)
{
    uint8_t sent = 0;

    while (sent < max_to_send) {
        if (!app_log_flush_one()) {
            break;
        }

        sent++;

        /*
         * Small gap so we do not hammer modem with back-to-back log publishes.
         */
        osDelay(100);
    }

    return sent;
}

uint16_t app_log_count(void)
{
    uint16_t count;

    if (s_log_mutex) {
        osMutexAcquire(s_log_mutex, osWaitForever);
    }

    count = s_count;

    if (s_log_mutex) {
        osMutexRelease(s_log_mutex);
    }

    return count;
}

uint32_t app_log_dropped_count(void)
{
    return s_dropped;
}


/* =========================================================
   Persistent critical log wrappers
   ========================================================= */

void app_log_critical(app_log_level_t level,
                      const char *module,
                      const char *event,
                      const char *message,
                      int32_t value1,
                      int32_t value2,
                      const char *recovery)
{
    const char *root_event = normalize_incident_event(module, event);

    print("[CRITICAL][DEFER][%s][%s] %s v1=%ld v2=%ld recovery=%s\r\n",
          module ? module : "",
          root_event ? root_event : "",
          message ? message : "",
          (long)value1,
          (long)value2,
          recovery ? recovery : "");

    /* Kept even before the unit has a config (2026-11-18): failures during
     * onboarding were dropped here. Logs no longer carry the API key, so
     * they can be uploaded as soon as the unit is online. */
    app_incident_cache_t *incident = find_or_alloc_incident(module, root_event);
    uint32_t now = HAL_GetTick();
    uint8_t should_emit = 0U;

    incident->count++;

    if (incident->first_seen_tick == 0U) {
        incident->first_seen_tick = now;
    }

    if (incident->last_emit_tick == 0U) {
        should_emit = 1U;
    }

    if (level > incident->level) {
        should_emit = 1U;
    }

    if (strcmp(incident->recovery, recovery ? recovery : "") != 0) {
        should_emit = 1U;
    }

    if ((now - incident->last_emit_tick) >= APP_INCIDENT_UPDATE_INTERVAL_MS) {
        should_emit = 1U;
    }

    incident->level = level;
    safe_copy(incident->recovery, sizeof(incident->recovery), recovery);

    if (!should_emit) {
        print("[CRITICAL][MERGE][%s][%s] count=%lu recovery=%s\r\n",
              module ? module : "",
              root_event ? root_event : "",
              (unsigned long)incident->count,
              recovery ? recovery : "");
        return;
    }

    incident->last_emit_tick = now;

    /*
     * Keep lightweight RAM/debug log.
     */
    app_log_push(level,
                 module,
                 root_event,
                 message,
                 (int32_t)incident->count,
                 value2);

    /*
     * Deferred queue only.
     * No W25Q write here.
     */
    if (s_crit_count >= APP_CRITICAL_DEFER_Q_SIZE) {
        s_crit_drop++;
        print("[CRITICAL][DROP] deferred queue full drop=%lu\r\n",
              (unsigned long)s_crit_drop);
        return;
    }

    app_deferred_critical_t *r = &s_crit_q[s_crit_wr];
    memset(r, 0, sizeof(*r));

    r->level = (uint8_t)level;
    r->value1 = (int32_t)incident->count;
    r->value2 = value2;
    r->first_seen_tick = incident->first_seen_tick;
    r->used = 1U;

    safe_copy(r->module, sizeof(r->module), module);
    safe_copy(r->event, sizeof(r->event), root_event);
    safe_copy(r->message, sizeof(r->message), message);
    safe_copy(r->recovery, sizeof(r->recovery), recovery);

    s_crit_wr++;
    if (s_crit_wr >= APP_CRITICAL_DEFER_Q_SIZE) {
        s_crit_wr = 0;
    }

    s_crit_count++;
}
uint8_t app_log_critical_persist_deferred_once(void)
{
    uint32_t now = HAL_GetTick();

    if (s_crit_count == 0U) {
        return 0U;
    }

    /*
     * Never hammer flash.
     */
    if ((now - s_last_flash_write_tick) < APP_CRITICAL_FLASH_WRITE_INTERVAL_MS) {
        return 0U;
    }

    s_last_flash_write_tick = now;

    app_deferred_critical_t *r = &s_crit_q[s_crit_rd];

    if (!r->used) {
        return 0U;
    }

    print("[CRITICAL][FLASH] writing deferred module=%s event=%s count=%u\r\n",
          r->module,
          r->event,
          s_crit_count);

    uint8_t ok = app_flash_log_write_incident((app_log_level_t)r->level,
                                              r->module,
                                              r->event,
                                              r->message,
                                              r->value1,
                                              r->value2,
                                              r->recovery,
                                              r->first_seen_tick);

    print("[CRITICAL][FLASH] write ok=%u pending=%u total=%u dropped=%lu deduped=%lu\r\n",
          ok,
          app_flash_log_pending_count(),
          app_flash_log_total_count(),
          (unsigned long)app_flash_log_dropped_count(),
          (unsigned long)app_flash_log_deduped_count());

    if (!ok) {
        /*
         * Keep it in RAM and retry later.
         */
        return 0U;
    }

    memset(r, 0, sizeof(*r));

    s_crit_rd++;
    if (s_crit_rd >= APP_CRITICAL_DEFER_Q_SIZE) {
        s_crit_rd = 0;
    }

    if (s_crit_count > 0U) {
        s_crit_count--;
    }

    return 1U;
}
uint8_t app_log_critical_flush_cloud_once(void)
{
    return app_flash_log_upload_due();
}

uint16_t app_log_critical_pending_count(void)
{
    return app_flash_log_pending_count();
}

uint16_t app_log_critical_total_count(void)
{
    return app_flash_log_total_count();
}

uint32_t app_log_critical_dropped_count(void)
{
    return app_flash_log_dropped_count();
}
