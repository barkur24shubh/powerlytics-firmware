#include "app_powerlytics_engine.h"
#include "app_rule_eval.h"

#include "app_board_io.h"
#include "app_debug_led.h"
#include "app_log.h"
#include "app_watchdog.h"
#include "board_profile.h"
#include "debug_uart.h"
#include "rtc_manager.h"

#include "cmsis_os.h"
#include "main.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define ENGINE_JSON_MAX        8192U
#define ENGINE_EVENT_MAX       512U
#define ENGINE_EVENT_QUEUE     (POWERLYTICS_DEFAULT_EVENT_CAP + 1U)
#define ENGINE_MB_REG_MAX      125U
#define ENGINE_SEQ_W25Q_ADDR   0x050000UL
#define ENGINE_SEQ_MAGIC       0x50515331UL /* PQS1 */
#define ENGINE_SEQ_SLOT_SIZE   16U
#define ENGINE_SEQ_SLOT_COUNT  (W25Q_SECTOR_SIZE / ENGINE_SEQ_SLOT_SIZE)
#define ENGINE_MODBUS_DEFAULT_INTERVAL_MS 1000UL
#define ENGINE_MODBUS_DEFAULT_TIMEOUT_MS  2000UL
#define ENGINE_DI_DEBOUNCE_MS              POWERLYTICS_DEFAULT_DI_DEBOUNCE_MS

#ifndef BOARD_DI_COUNT
#define BOARD_DI_COUNT 8U
#endif

#ifndef BOARD_AI_COUNT
#define BOARD_AI_COUNT 10U
#endif

#define ENGINE_MAX_DI_CHANNELS 8U
#define ENGINE_MAX_AI_CHANNELS 10U
#define ENGINE_DI_CHANNELS ((BOARD_DI_COUNT < ENGINE_MAX_DI_CHANNELS) ? BOARD_DI_COUNT : ENGINE_MAX_DI_CHANNELS)
#define ENGINE_AI_CHANNELS ((BOARD_AI_COUNT < ENGINE_MAX_AI_CHANNELS) ? BOARD_AI_COUNT : ENGINE_MAX_AI_CHANNELS)

/* Analog alarm state machine lives in app_rule_eval.c (host-tested). */
typedef app_rule_runtime_t rule_runtime_t;

typedef struct {
  uint8_t present;
  uint8_t last;
  uint8_t seeded;
  uint32_t last_edge_ms;
  uint32_t transitions;
  uint32_t on_duration_ms;
  rule_runtime_t rules[POWERLYTICS_MAX_RULES_PER_POINT];
} digital_runtime_t;

typedef struct {
  uint8_t present;
  uint8_t seeded;
  float last;
  float min;
  float max;
  double sum;
  uint32_t samples;
  char min_ts[40];
  char max_ts[40];
  rule_runtime_t rules[POWERLYTICS_MAX_RULES_PER_POINT];
} analog_runtime_t;

typedef struct {
  uint8_t present;
  uint8_t seeded;
  uint8_t retry_attempt;
  float last;
  float previous;
  uint32_t transitions;
  uint32_t on_duration_ms;
  uint32_t last_edge_ms;
  uint32_t counter_baseline;
  uint32_t counter_last;
  uint32_t counter_delta;
  uint32_t raw_last;
  analog_runtime_t analog;
  rule_runtime_t rules[POWERLYTICS_MAX_RULES_PER_POINT];
} modbus_read_runtime_t;

typedef struct {
  uint32_t magic;
  uint32_t telemetry_seq;
  uint32_t event_seq;
  uint32_t crc32;
} seq_record_t;

extern char app_boot_id[48];
static uint32_t s_last_built_seq;
static uint8_t  s_last_built_valid;
static modbus_cfg_t s_cfg;
/* Relay states as last put into a telemetry snapshot (bit i = RO_(i+1) ON). */
static uint8_t s_reported_relays;
static uint8_t s_reported_relays_valid;
static uint8_t s_cfg_valid;
static mbm_t *s_mbm;
static W25Q_Handle *s_flash;
static uint32_t s_next_sample_ms;
static uint32_t s_next_telemetry_seq = 1U;
static uint32_t s_next_event_seq = 1U;
/* Event seq last written to flash, and events dropped because the queue was
 * full (2026-11-18). */
static uint32_t s_saved_event_seq;
static uint32_t s_saved_seq_tick;
static volatile uint32_t s_events_dropped;
static uint8_t s_events_this_interval;

static digital_runtime_t s_di[8];
static analog_runtime_t s_ai[10];
static modbus_read_runtime_t s_mi[MODBUS_MAX_SLAVES][MODBUS_MAX_READS_PER_SLAVE];
static uint32_t s_modbus_next_poll[MODBUS_MAX_SLAVES];
static uint8_t s_modbus_next_read[MODBUS_MAX_SLAVES];

/* CPU-only JSON storage lives in CCM so normal SRAM remains available to DMA and FreeRTOS. */
static char s_telemetry_json[ENGINE_JSON_MAX] __attribute__((section(".ccmram"), aligned(4)));
static char s_event_queue[ENGINE_EVENT_QUEUE][ENGINE_EVENT_MAX] __attribute__((section(".ccmram"), aligned(4)));
static volatile uint8_t s_event_w;
static volatile uint8_t s_event_r;
static uint16_t s_mb_regs[ENGINE_MB_REG_MAX];
static volatile uint8_t s_di_event_pending[8][POWERLYTICS_MAX_RULES_PER_POINT];
static volatile uint8_t s_di_event_previous[8][POWERLYTICS_MAX_RULES_PER_POINT];
static volatile uint8_t s_di_event_current[8][POWERLYTICS_MAX_RULES_PER_POINT];
/* Edge rule chatter filter: rule debounceMs is the minimum time between two
 * events of the same rule; edges inside it are counted and the count goes
 * out with the next event as data.suppressed. */
static uint32_t s_di_rule_last_ms[8][POWERLYTICS_MAX_RULES_PER_POINT];
static uint8_t  s_di_rule_sent[8][POWERLYTICS_MAX_RULES_PER_POINT];
static uint32_t s_di_rule_suppressed[8][POWERLYTICS_MAX_RULES_PER_POINT];
static volatile uint16_t s_di_irq_pending_mask;
static volatile uint32_t s_di_irq_tick[ENGINE_MAX_DI_CHANNELS];

static void service_pending_di_edges(void);

static int jb_append(char *dst, int max, int *idx, const char *fmt, ...)
{
  if (!dst || !idx || *idx >= max) return -1;

  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(&dst[*idx], (size_t)(max - *idx), fmt, ap);
  va_end(ap);

  if (n < 0) return -1;
  if (n >= (max - *idx)) {
    *idx = max;
    return -1;
  }

  *idx += n;
  return 0;
}

static uint32_t crc32_sw(const void *data, uint32_t len)
{
  const uint8_t *p = (const uint8_t *)data;
  uint32_t crc = 0xFFFFFFFFUL;

  for (uint32_t i = 0U; i < len; i++) {
    crc ^= p[i];
    for (uint8_t b = 0U; b < 8U; b++) {
      uint32_t mask = (uint32_t)(-(int32_t)(crc & 1UL));
      crc = (crc >> 1) ^ (0xEDB88320UL & mask);
    }
  }

  return ~crc;
}

static uint8_t seq_record_valid(const seq_record_t *r)
{
  if (!r || r->magic != ENGINE_SEQ_MAGIC) return 0U;
  seq_record_t tmp = *r;
  uint32_t stored = tmp.crc32;
  tmp.crc32 = 0U;
  return (crc32_sw(&tmp, sizeof(tmp)) == stored) ? 1U : 0U;
}

/* Seq records ring over ENGINE_SEQ_SECTORS 4 KB sectors (0x050000-0x05FFFF,
 * free until 2026-11-19). Before, one sector took a 16-byte record every
 * telemetry and was erased every 256 saves: ~4 years of W25Q endurance at a
 * 5 s interval, ~1 year at 1 s. Sixteen sectors give 16x that; the next free
 * slot is kept in RAM instead of scanning up to 256 slots on every save. */
#define ENGINE_SEQ_SECTORS      16U
#define ENGINE_SEQ_TOTAL_SLOTS  (ENGINE_SEQ_SLOT_COUNT * ENGINE_SEQ_SECTORS)
static uint16_t s_seq_next_slot;

static uint32_t seq_slot_addr(uint16_t slot)
{
  return ENGINE_SEQ_W25Q_ADDR + ((uint32_t)slot * ENGINE_SEQ_SLOT_SIZE);
}

static void seq_store(void)
{
  if (!s_flash) return;

  seq_record_t rec;
  memset(&rec, 0, sizeof(rec));
  rec.magic = ENGINE_SEQ_MAGIC;
  rec.telemetry_seq = s_next_telemetry_seq;
  rec.event_seq = s_next_event_seq;
  rec.crc32 = crc32_sw(&rec, sizeof(rec));
  s_saved_event_seq = rec.event_seq;
  s_saved_seq_tick = HAL_GetTick();

  uint16_t slot = (uint16_t)(s_seq_next_slot % ENGINE_SEQ_TOTAL_SLOTS);
  uint32_t probe = 0U;
  if (!w25q_read(s_flash, seq_slot_addr(slot), (uint8_t *)&probe, sizeof(probe))) return;
  if (probe != 0xFFFFFFFFUL) {
    /* Entering a used sector (the oldest records): erase it first. */
    uint16_t first = (uint16_t)(slot - (slot % ENGINE_SEQ_SLOT_COUNT));
    if (!w25q_erase_sector_4k(s_flash, seq_slot_addr(first))) return;
  }
  if (w25q_write(s_flash, seq_slot_addr(slot), (const uint8_t *)&rec, sizeof(rec))) {
    s_seq_next_slot = (uint16_t)((slot + 1U) % ENGINE_SEQ_TOTAL_SLOTS);
  }
}

static void seq_load(void)
{
  if (!s_flash) return;

  seq_record_t rec;
  uint8_t found = 0U;
  uint16_t newest = 0U;
  uint32_t best_t = 0U, best_e = 0U;

  for (uint16_t slot = 0U; slot < ENGINE_SEQ_TOTAL_SLOTS; slot++) {
    if ((slot & 31U) == 0U) app_watchdog_force_refresh();
    memset(&rec, 0, sizeof(rec));
    if (!w25q_read(s_flash, seq_slot_addr(slot), (uint8_t *)&rec, sizeof(rec))) return;
    if (rec.magic == 0xFFFFFFFFUL) continue;
    if (!seq_record_valid(&rec)) continue;
    if (!found || rec.telemetry_seq > best_t || (rec.telemetry_seq == best_t && rec.event_seq >= best_e)) {
      best_t = rec.telemetry_seq;
      best_e = rec.event_seq;
      newest = slot;
      found = 1U;
    }
  }
  if (found) {
    s_next_telemetry_seq = best_t ? best_t : 1U;
    s_next_event_seq = best_e ? best_e : 1U;
    s_seq_next_slot = (uint16_t)((newest + 1U) % ENGINE_SEQ_TOTAL_SLOTS);
  } else {
    s_seq_next_slot = 0U;
  }

  /* Event seq is saved within ~1 s of each event (app_powerlytics_engine_seq_service),
   * so skip ahead at boot: an event raised after the last save would otherwise
   * reuse an eventId, and the platform drops a repeated eventId as a duplicate. */
  if (found) s_next_event_seq += 64U;
  seq_store();
}

void app_powerlytics_engine_seq_service(void)
{
  if (s_next_event_seq != s_saved_event_seq && (HAL_GetTick() - s_saved_seq_tick) >= 1000U) {
    seq_store();
  }
}

uint32_t app_powerlytics_engine_events_dropped(void)
{
  return s_events_dropped;
}

static uint32_t critical_enter(void)
{
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  return primask;
}

static void critical_exit(uint32_t primask)
{
  if (!primask) __enable_irq();
}

static uint8_t cfg_string_valid(const char *s)
{
  if (!s || s[0] == '\0') return 0U;
  if (strcmp(s, "unknown_device") == 0 || strcmp(s, "unknown_config") == 0 || strcmp(s, "missing") == 0) return 0U;
  return 1U;
}

static const powerlytics_point_cfg_t *find_point_any(const char *key)
{
  if (!key || !s_cfg_valid) return NULL;

  for (uint8_t i = 0U; i < s_cfg.point_count; i++) {
    if (strcmp(s_cfg.points[i].key, key) == 0) return &s_cfg.points[i];
  }

  return NULL;
}

static const powerlytics_point_cfg_t *find_channel_point_any(const char *prefix,
                                                             int index)
{
  char key[16];
  const powerlytics_point_cfg_t *p;

  if (!prefix) return NULL;

  snprintf(key, sizeof(key), "%s%d", prefix, index + 1);
  p = find_point_any(key);
  if (p) return p;

  if (strcmp(prefix, "DI_") == 0) {
    snprintf(key, sizeof(key), "DI%d", index + 1);
    p = find_point_any(key);
    if (p) return p;

    snprintf(key, sizeof(key), "di%d", index + 1);
    p = find_point_any(key);
    if (p) return p;

    snprintf(key, sizeof(key), "di_%d", index + 1);
    p = find_point_any(key);
    if (p) return p;
  } else if (strcmp(prefix, "AI_") == 0) {
    snprintf(key, sizeof(key), "AI%d", index + 1);
    p = find_point_any(key);
    if (p) return p;

    snprintf(key, sizeof(key), "ai%d", index + 1);
    p = find_point_any(key);
    if (p) return p;

    snprintf(key, sizeof(key), "ai_%d", index + 1);
    p = find_point_any(key);
    if (p) return p;
  }

  return NULL;
}

static void make_default_point(powerlytics_point_cfg_t *p, const char *prefix, int index, uint8_t kind)
{
  if (!p) return;
  memset(p, 0, sizeof(*p));
  snprintf(p->key, sizeof(p->key), "%s%d", prefix, index + 1);
  snprintf(p->name, sizeof(p->name), "%s%d", prefix, index + 1);
  p->kind = kind;
  p->enabled = 1U;
  p->edge = POWERLYTICS_EDGE_BOTH;
  p->active_high = 1U;
  p->scale = 1.0f;
  p->offset = 0.0f;
  p->counter_width = 32U;
  p->rollover_at = 0xFFFFFFFFUL;
  p->debounce_ms = ENGINE_DI_DEBOUNCE_MS;
}

static const powerlytics_point_cfg_t *point_or_default(const char *prefix,
                                                       int index,
                                                       uint8_t kind,
                                                       powerlytics_point_cfg_t *tmp)
{
  const powerlytics_point_cfg_t *p = find_channel_point_any(prefix, index);
  if (p) {
    if (!p->enabled || p->kind != kind) return NULL;
    return p;
  }

  make_default_point(tmp, prefix, index, kind);
  return tmp;
}

static uint8_t di_event_rule_count(const powerlytics_point_cfg_t *p)
{
  if (!p || p->kind != POWERLYTICS_POINT_DIGITAL) return 0U;
  if (p->rule_count > 0U) {
    return (p->rule_count > POWERLYTICS_MAX_RULES_PER_POINT) ?
           POWERLYTICS_MAX_RULES_PER_POINT :
           p->rule_count;
  }
  return 2U;
}

static uint8_t di_event_rule_at(const powerlytics_point_cfg_t *p,
                                uint8_t index,
                                powerlytics_rule_cfg_t *out)
{
  if (!p || !out || p->kind != POWERLYTICS_POINT_DIGITAL ||
      index >= POWERLYTICS_MAX_RULES_PER_POINT) {
    return 0U;
  }

  if (p->rule_count > 0U) {
    if (index >= p->rule_count) return 0U;
    *out = p->rules[index];
    return out->enabled ? 1U : 0U;
  }

  memset(out, 0, sizeof(*out));
  out->enabled = 1U;
  out->severity = POWERLYTICS_SEVERITY_INFO;
  out->type = (index == 0U) ? POWERLYTICS_RULE_RISING_EDGE : POWERLYTICS_RULE_FALLING_EDGE;
  return 1U;
}

float app_powerlytics_calibrate_u32(uint32_t raw, float scale, float offset)
{
  return ((float)raw * scale) + offset;
}

uint32_t app_powerlytics_counter_delta(uint32_t previous, uint32_t current, uint32_t rollover_at)
{
  if (rollover_at == 0U) rollover_at = 0xFFFFFFFFUL;
  if (current >= previous) return current - previous;
  return (rollover_at - previous) + current + 1UL;
}

static void analog_reset_interval(analog_runtime_t *a)
{
  if (!a) return;
  a->samples = 0U;
  a->sum = 0.0;
  a->min = 0.0f;
  a->max = 0.0f;
  a->min_ts[0] = '\0';
  a->max_ts[0] = '\0';
}

static void analog_sample(analog_runtime_t *a, float value)
{
  if (!a) return;

  char ts[40];
  rtc_get_datetime_strings(ts, sizeof(ts));

  if (a->samples == 0U) {
    a->min = value;
    a->max = value;
    strncpy(a->min_ts, ts, sizeof(a->min_ts) - 1U);
    strncpy(a->max_ts, ts, sizeof(a->max_ts) - 1U);
  } else {
    if (value < a->min) {
      a->min = value;
      strncpy(a->min_ts, ts, sizeof(a->min_ts) - 1U);
    }
    if (value > a->max) {
      a->max = value;
      strncpy(a->max_ts, ts, sizeof(a->max_ts) - 1U);
    }
  }

  a->last = value;
  a->sum += (double)value;
  a->samples++;
  a->seeded = 1U;
}

static const char *severity_name(uint8_t severity)
{
  switch (severity) {
    case POWERLYTICS_SEVERITY_CRITICAL: return "critical";
    case POWERLYTICS_SEVERITY_WARNING: return "warning";
    default: return "info";
  }
}

static void append_float(char *json, int max, int *idx, float value)
{
  int32_t milli = (int32_t)(value * 1000.0f);
  if (value < 0.0f && milli == 0) jb_append(json, max, idx, "-");
  if (milli < 0) {
    jb_append(json, max, idx, "-%ld.%03ld", (long)((-milli) / 1000), (long)((-milli) % 1000));
  } else {
    jb_append(json, max, idx, "%ld.%03ld", (long)(milli / 1000), (long)(milli % 1000));
  }
}

static uint8_t event_queue_has_space(void)
{
  return (uint8_t)(((s_event_w + 1U) % ENGINE_EVENT_QUEUE) != s_event_r);
}

/* extra: optional JSON members (no leading comma) added inside data, e.g.
 * "context":{...} or "reason":"rule_changed". */
static uint8_t event_push_json_x(const char *source,
                                 const char *event_type,
                                 uint8_t severity,
                                 const char *slave_id,
                                 const char *read_id,
                                 float value,
                                 float previous,
                                 float threshold,
                                 const char *extra)
{

  char ts[40];
  rtc_get_datetime_strings(ts, sizeof(ts));

  app_watchdog_force_refresh();
  /* No per-interval event cap: every edge is queued and sent on its own,
   * ahead of telemetry. Only a full queue holds an event back (DI edges
   * stay pending and are retried). eventCapPerInterval is ignored. */
  uint32_t primask = critical_enter();

  uint8_t next = (uint8_t)((s_event_w + 1U) % ENGINE_EVENT_QUEUE);
  if (next == s_event_r) {
    s_events_dropped++;
    critical_exit(primask);
    return 0U;
  }

  char *json = s_event_queue[s_event_w];
  int idx = 0;
  uint32_t event_seq = s_next_event_seq++;

  jb_append(json, ENGINE_EVENT_MAX, &idx,
            "{\"v\":1,\"type\":\"event\",\"eventId\":\"%s-%lu\","
            "\"eventSeq\":%lu,\"telemetrySeq\":%lu,\"imei\":\"%s\","
            "\"configId\":\"%s\",\"ts\":\"%s\",\"source\":\"%s\","
            "\"eventType\":\"%s\",\"severity\":\"%s\",\"data\":{",
            source ? source : "event",
            (unsigned long)event_seq,
            (unsigned long)event_seq,
            (unsigned long)s_next_telemetry_seq,
            s_cfg.imei,
            s_cfg.config_id,
            ts,
            source ? source : "unknown",
            event_type ? event_type : "EVENT",
            severity_name(severity));

  if (slave_id && slave_id[0]) jb_append(json, ENGINE_EVENT_MAX, &idx, "\"slaveId\":\"%s\",", slave_id);
  if (read_id && read_id[0]) jb_append(json, ENGINE_EVENT_MAX, &idx, "\"readId\":\"%s\",", read_id);

  jb_append(json, ENGINE_EVENT_MAX, &idx, "\"value\":");
  append_float(json, ENGINE_EVENT_MAX, &idx, value);
  jb_append(json, ENGINE_EVENT_MAX, &idx, ",\"previousValue\":");
  append_float(json, ENGINE_EVENT_MAX, &idx, previous);
  jb_append(json, ENGINE_EVENT_MAX, &idx, ",\"threshold\":");
  append_float(json, ENGINE_EVENT_MAX, &idx, threshold);
  if (extra && extra[0]) jb_append(json, ENGINE_EVENT_MAX, &idx, ",%s", extra);
  jb_append(json, ENGINE_EVENT_MAX, &idx, "}}");

  if (idx >= (int)ENGINE_EVENT_MAX) {
    json[ENGINE_EVENT_MAX - 1U] = '\0';
    critical_exit(primask);
    return 0U;
  }

  json[idx] = '\0';
  s_events_this_interval++;
  s_event_w = next;
  critical_exit(primask);
  return 1U;
}

/* ---------- Rule context (contextKeys) ---------- */

static int point_index_of_key(const char *key, const char *prefix)
{
  /* "AI_3", "AI3", "ai3", "ai_3" -> 2 */
  if (!key || !prefix) return -1;
  size_t n = strlen(prefix);
  if (strncasecmp(key, prefix, n - 1U) != 0) return -1;
  const char *d = key + (n - 1U);
  if (*d == '_') d++;
  if (*d < '1' || *d > '9') return -1;
  int v = atoi(d);
  return (v >= 1) ? (v - 1) : -1;
}

static uint8_t context_value(const char *key, float *out)
{
  int i = point_index_of_key(key, "AI_");
  if (i >= 0 && i < (int)ENGINE_AI_CHANNELS && s_ai[i].present && s_ai[i].seeded) {
    *out = s_ai[i].last;
    return 1U;
  }
  i = point_index_of_key(key, "DI_");
  if (i >= 0 && i < (int)ENGINE_DI_CHANNELS && s_di[i].present && s_di[i].seeded) {
    *out = (float)s_di[i].last;
    return 1U;
  }
  for (uint8_t si = 0U; si < s_cfg.slave_count && si < MODBUS_MAX_SLAVES; si++) {
    for (uint8_t ri = 0U; ri < s_cfg.slaves[si].read_count && ri < MODBUS_MAX_READS_PER_SLAVE; ri++) {
      const mb_read_item_t *rc = &s_cfg.slaves[si].reads[ri];
      if (strcmp(rc->read_id, key) != 0 && strcmp(rc->tag, key) != 0) continue;
      const modbus_read_runtime_t *rt = &s_mi[si][ri];
      if (!rt->present) return 0U;
      *out = rt->last;
      return 1U;
    }
  }
  return 0U;
}

/* "context":{"AI_2":1.250,"DI_1":1.000} for the keys that have a value now.
 * Keys with no reading yet are sent as null so the cloud can tell. */
static void build_rule_extra(const powerlytics_rule_cfg_t *rule, uint32_t suppressed,
                             char *buf, int max)
{
  int idx = 0;
  buf[0] = '\0';
  if (!rule) return;
  if (rule->context_count > 0U) {
    jb_append(buf, max, &idx, "\"context\":{");
    for (uint8_t k = 0U; k < rule->context_count && k < POWERLYTICS_MAX_CONTEXT_KEYS; k++) {
      float v;
      jb_append(buf, max, &idx, "%s\"%s\":", k ? "," : "", rule->context_keys[k]);
      if (context_value(rule->context_keys[k], &v)) append_float(buf, max, &idx, v);
      else jb_append(buf, max, &idx, "null");
    }
    jb_append(buf, max, &idx, "}");
  }
  if (suppressed > 0U) {
    jb_append(buf, max, &idx, "%s\"suppressed\":%lu", idx ? "," : "", (unsigned long)suppressed);
  }
  if (idx >= max) buf[0] = '\0';
}

/* ---------- Active alarms kept across resets (backup SRAM) ---------- */
/* Each active analog rule is stored as a hash of where it is (point key or
 * slave/read id, rule index) and what it means (type, limits, hysteresis).
 * After a reset or a config redeploy a rule whose hash is in the set starts
 * active, so a condition that is still present is not raised a second time
 * and its recovery is still sent when it clears. Backup SRAM survives
 * resets (watchdog, OTA, modem recovery) and, with VBAT and the backup
 * regulator on, power loss. A bad magic/CRC means nothing is restored. */
#define ALARM_BKP_MAGIC 0x414C4D31UL /* ALM1 */
#define ALARM_BKP_MAX   (ENGINE_MAX_AI_CHANNELS * POWERLYTICS_MAX_RULES_PER_POINT + \
                         MODBUS_MAX_SLAVES * MODBUS_MAX_READS_PER_SLAVE * POWERLYTICS_MAX_RULES_PER_POINT)

typedef struct {
  uint32_t magic;
  uint32_t count;
  uint32_t hash[ALARM_BKP_MAX];
  uint32_t crc;
} alarm_bkp_t;

static alarm_bkp_t *alarm_bkp(void)
{
  static uint8_t ready;
  if (!ready) {
    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();
    __HAL_RCC_BKPSRAM_CLK_ENABLE();
    (void)HAL_PWREx_EnableBkUpReg();
    ready = 1U;
  }
  return (alarm_bkp_t *)BKPSRAM_BASE;
}

static uint32_t rule_hash(const char *where1, const char *where2, uint8_t index,
                          const powerlytics_rule_cfg_t *r)
{
  struct {
    char w1[40];
    char w2[40];
    uint8_t index;
    uint8_t type;
    float high;
    float low;
    float hysteresis;
  } k;
  memset(&k, 0, sizeof(k));
  if (where1) strncpy(k.w1, where1, sizeof(k.w1) - 1U);
  if (where2) strncpy(k.w2, where2, sizeof(k.w2) - 1U);
  k.index = index;
  k.type = r->type;
  k.high = r->high;
  k.low = r->low;
  k.hysteresis = r->hysteresis;
  return crc32_sw(&k, sizeof(k));
}

typedef void (*rule_visit_fn)(const char *source, const char *slave_id, const char *read_id,
                              uint8_t index, const powerlytics_rule_cfg_t *rule,
                              rule_runtime_t *rt, float last, void *ctx);

/* Every enabled analog rule of cfg, with the runtime slot it uses. */
static void visit_analog_rules(const modbus_cfg_t *cfg, rule_visit_fn fn, void *ctx)
{
  if (!cfg || !fn) return;
  for (uint8_t pi = 0U; pi < cfg->point_count && pi < POWERLYTICS_MAX_POINTS; pi++) {
    const powerlytics_point_cfg_t *p = &cfg->points[pi];
    if (!p->enabled || p->kind != POWERLYTICS_POINT_ANALOG) continue;
    int ai = point_index_of_key(p->key, "AI_");
    if (ai < 0 || ai >= (int)ENGINE_AI_CHANNELS) continue;
    for (uint8_t r = 0U; r < p->rule_count && r < POWERLYTICS_MAX_RULES_PER_POINT; r++) {
      if (!p->rules[r].enabled || !app_rule_is_analog(p->rules[r].type)) continue;
      fn(p->key, NULL, NULL, r, &p->rules[r], &s_ai[ai].rules[r], s_ai[ai].last, ctx);
    }
  }
  for (uint8_t si = 0U; si < cfg->slave_count && si < MODBUS_MAX_SLAVES; si++) {
    const mb_slave_cfg_t *sc = &cfg->slaves[si];
    for (uint8_t ri = 0U; ri < sc->read_count && ri < MODBUS_MAX_READS_PER_SLAVE; ri++) {
      const mb_read_item_t *rc = &sc->reads[ri];
      if (rc->kind != POWERLYTICS_MODBUS_KIND_ANALOG) continue;
      for (uint8_t r = 0U; r < rc->rule_count && r < POWERLYTICS_MAX_RULES_PER_POINT; r++) {
        if (!rc->rules[r].enabled || !app_rule_is_analog(rc->rules[r].type)) continue;
        fn("MI_1", sc->unique_slave_id, rc->read_id, r, &rc->rules[r],
           &s_mi[si][ri].analog.rules[r], s_mi[si][ri].last, ctx);
      }
    }
  }
}

typedef struct {
  uint32_t hash[ALARM_BKP_MAX];
  uint32_t count;
} hash_set_t;

static uint8_t hash_set_has(const hash_set_t *set, uint32_t h)
{
  for (uint32_t i = 0U; i < set->count; i++) if (set->hash[i] == h) return 1U;
  return 0U;
}

static void collect_active(const char *source, const char *slave_id, const char *read_id,
                           uint8_t index, const powerlytics_rule_cfg_t *rule,
                           rule_runtime_t *rt, float last, void *ctx)
{
  (void)last;
  hash_set_t *set = (hash_set_t *)ctx;
  if (!app_rule_is_active(rt) || set->count >= ALARM_BKP_MAX) return;
  set->hash[set->count++] = rule_hash(slave_id ? slave_id : source, read_id, index, rule);
}

static void collect_all(const char *source, const char *slave_id, const char *read_id,
                        uint8_t index, const powerlytics_rule_cfg_t *rule,
                        rule_runtime_t *rt, float last, void *ctx)
{
  (void)rt; (void)last;
  hash_set_t *set = (hash_set_t *)ctx;
  if (set->count >= ALARM_BKP_MAX) return;
  set->hash[set->count++] = rule_hash(slave_id ? slave_id : source, read_id, index, rule);
}

static void alarm_state_save(void)
{
  static hash_set_t set;
  vTaskSuspendAll();
  set.count = 0U;
  visit_analog_rules(&s_cfg, collect_active, &set);

  alarm_bkp_t *b = alarm_bkp();
  b->magic = 0U; /* invalid while being written */
  b->count = set.count;
  memcpy(b->hash, set.hash, sizeof(b->hash));
  b->crc = crc32_sw(b->hash, sizeof(b->hash)) ^ b->count;
  b->magic = ALARM_BKP_MAGIC;
  (void)xTaskResumeAll();
}

static uint8_t alarm_state_load(hash_set_t *set)
{
  alarm_bkp_t *b = alarm_bkp();
  set->count = 0U;
  if (b->magic != ALARM_BKP_MAGIC || b->count > ALARM_BKP_MAX) return 0U;
  if ((crc32_sw(b->hash, sizeof(b->hash)) ^ b->count) != b->crc) return 0U;
  set->count = b->count;
  memcpy(set->hash, b->hash, sizeof(set->hash));
  return 1U;
}

static void restore_active(const char *source, const char *slave_id, const char *read_id,
                           uint8_t index, const powerlytics_rule_cfg_t *rule,
                           rule_runtime_t *rt, float last, void *ctx)
{
  (void)last;
  const hash_set_t *set = (const hash_set_t *)ctx;
  if (hash_set_has(set, rule_hash(slave_id ? slave_id : source, read_id, index, rule))) {
    app_rule_restore_active(rt, HAL_GetTick());
  }
}

/* A rule that was active and is gone (deleted, disabled, new limits) after
 * a redeploy: tell the cloud the alarm it raised is over. */
static void recover_dropped(const char *source, const char *slave_id, const char *read_id,
                            uint8_t index, const powerlytics_rule_cfg_t *rule,
                            rule_runtime_t *rt, float last, void *ctx)
{
  const hash_set_t *next = (const hash_set_t *)ctx;
  if (!app_rule_is_active(rt)) return;
  if (hash_set_has(next, rule_hash(slave_id ? slave_id : source, read_id, index, rule))) return;
  float threshold = (rule->type == POWERLYTICS_RULE_THRESHOLD_LOW) ? rule->low : rule->high;
  if (rule->type == POWERLYTICS_RULE_OUT_OF_RANGE || rule->type == POWERLYTICS_RULE_IN_RANGE) {
    threshold = (last < rule->low) ? rule->low : rule->high;
  }
  (void)event_push_json_x(source, "AI_RECOVERY", rule->severity, slave_id, read_id,
                          last, last, threshold, "\"reason\":\"rule_changed\"");
}

static void evaluate_threshold_rule(const char *source,
                                    const char *slave_id,
                                    const char *read_id,
                                    const powerlytics_rule_cfg_t *cfg,
                                    rule_runtime_t *rt,
                                    float value)
{
  app_rule_out_t out;
  app_rule_ev_t ev = app_rule_step(cfg, rt, value, HAL_GetTick(), &out);
  if (ev == RULE_EV_NONE) return;

  /* Only a queued event changes the rule's state; a full queue leaves it
   * pending and the next sample tries again (not counted as a drop). */
  if (!event_queue_has_space()) return;

  char extra[128];
  build_rule_extra(cfg, 0U, extra, (int)sizeof(extra));
  uint32_t now = HAL_GetTick();
  if (event_push_json_x(source, out.event_type, cfg->severity, slave_id, read_id,
                        value, out.previous, out.threshold, extra[0] ? extra : NULL)) {
    if (app_rule_commit(rt, ev, now)) {
      print("[RULE] %s %s chattering: recovery held %lu s\r\n", source,
            out.event_type, (unsigned long)(RULE_CHATTER_HOLD_MS / 1000UL));
    }
    alarm_state_save();
  }
}

static void evaluate_analog_rules(const char *source,
                                  const char *slave_id,
                                  const char *read_id,
                                  const powerlytics_rule_cfg_t *rules,
                                  rule_runtime_t *rt,
                                  uint8_t count,
                                  float value)
{
  for (uint8_t i = 0U; i < count && i < POWERLYTICS_MAX_RULES_PER_POINT; i++) {
    evaluate_threshold_rule(source, slave_id, read_id, &rules[i], &rt[i], value);
  }
}

static void sample_ai_values(const uint32_t mv[10])
{
  if (!mv) return;

  for (uint8_t i = 0U; i < ENGINE_AI_CHANNELS; i++) {
    powerlytics_point_cfg_t tmp;
    const powerlytics_point_cfg_t *p = point_or_default("AI_", i, POWERLYTICS_POINT_ANALOG, &tmp);
    if (!p || p->kind != POWERLYTICS_POINT_ANALOG) continue;

    float calibrated = app_powerlytics_calibrate_u32(mv[i], p->scale, p->offset);
    uint32_t primask = critical_enter();
    s_ai[i].present = 1U;
    analog_sample(&s_ai[i], calibrated);
    critical_exit(primask);

    evaluate_analog_rules(p->key, NULL, NULL, p->rules, s_ai[i].rules, p->rule_count, calibrated);
  }
}

static uint8_t sample_ai_pending(void)
{
  uint32_t mv[10] = {0};
  uint8_t consumed = 0U;

  while (app_adc_pop_sample_mv(mv)) {
    sample_ai_values(mv);
    consumed = 1U;
  }

  return consumed;
}

static void sample_ai_once_blocking(void)
{
  uint32_t mv[10] = {0};
  app_adc_read_all_mv(mv);
  sample_ai_values(mv);
}

static void flush_di_events(void)
{
  for (uint8_t i = 0U; i < ENGINE_DI_CHANNELS; i++) {
    app_watchdog_force_refresh();
    powerlytics_point_cfg_t tmp;
    const powerlytics_point_cfg_t *p = point_or_default("DI_", i, POWERLYTICS_POINT_DIGITAL, &tmp);
    if (!p || p->kind != POWERLYTICS_POINT_DIGITAL) continue;

    uint8_t rule_count = di_event_rule_count(p);
    for (uint8_t r = 0U; r < rule_count; r++) {
      uint8_t pending;
      uint8_t previous;
      uint8_t current;
      powerlytics_rule_cfg_t rule;

      uint32_t primask = critical_enter();
      pending = s_di_event_pending[i][r];
      previous = s_di_event_previous[i][r];
      current = s_di_event_current[i][r];
      s_di_event_pending[i][r] = 0U;
      critical_exit(primask);

      if (!pending || !di_event_rule_at(p, r, &rule)) continue;

      uint32_t now = HAL_GetTick();
      if (rule.debounce_ms > 0U && s_di_rule_sent[i][r] &&
          (uint32_t)(now - s_di_rule_last_ms[i][r]) < rule.debounce_ms) {
        s_di_rule_suppressed[i][r]++;
        continue;
      }

      char extra[128];
      build_rule_extra(&rule, s_di_rule_suppressed[i][r], extra, (int)sizeof(extra));
      uint8_t queued = 0U;
      if (rule.type == POWERLYTICS_RULE_RISING_EDGE) {
        queued = event_push_json_x(p->key, "DI_RISING_EDGE", rule.severity,
                                   NULL, NULL, (float)current, (float)previous, 1.0f,
                                   extra[0] ? extra : NULL);
      } else if (rule.type == POWERLYTICS_RULE_FALLING_EDGE) {
        queued = event_push_json_x(p->key, "DI_FALLING_EDGE", rule.severity,
                                   NULL, NULL, (float)current, (float)previous, 0.0f,
                                   extra[0] ? extra : NULL);
      }

      if (queued) {
        s_di_rule_last_ms[i][r] = now;
        s_di_rule_sent[i][r] = 1U;
        s_di_rule_suppressed[i][r] = 0U;
      } else {
        primask = critical_enter();
        if (!s_di_event_pending[i][r]) {
          s_di_event_previous[i][r] = previous;
          s_di_event_current[i][r] = current;
          s_di_event_pending[i][r] = 1U;
        }
        critical_exit(primask);
      }
    }
  }
}

static HAL_StatusTypeDef mb_read_by_func(uint8_t func,
                                         uint8_t slave_id,
                                         uint16_t start,
                                         uint16_t qty,
                                         uint32_t timeout_ms,
                                         uint16_t *out_regs)
{
  if (!s_mbm) return HAL_ERROR;

  if (func == 1U) return mbm_read_coils_01(s_mbm, slave_id, start, qty, timeout_ms, out_regs);
  if (func == 2U) return mbm_read_discrete_inputs_02(s_mbm, slave_id, start, qty, timeout_ms, out_regs);
  if (func == 3U) return mbm_read_holding_03(s_mbm, slave_id, start, qty, timeout_ms, out_regs);
  if (func == 4U) return mbm_read_input_04(s_mbm, slave_id, start, qty, timeout_ms, out_regs);
  return HAL_ERROR;
}

static uint32_t modbus_decode_u32(const mb_read_item_t *rc, const uint16_t *regs)
{
  if (!rc || !regs) return 0U;

  uint32_t v32 = ((uint32_t)regs[0] << 16);
  if (rc->qty > 1U) v32 |= regs[1];
  else v32 = regs[0];

  if (rc->qty >= 2U) {
    uint8_t a = (uint8_t)(regs[0] >> 8);
    uint8_t b = (uint8_t)(regs[0] & 0xFFU);
    uint8_t c = (uint8_t)(regs[1] >> 8);
    uint8_t d = (uint8_t)(regs[1] & 0xFFU);
    switch (rc->endianness) {
      case POWERLYTICS_ENDIAN_BADC: v32 = ((uint32_t)b << 24) | ((uint32_t)a << 16) | ((uint32_t)d << 8) | c; break;
      case POWERLYTICS_ENDIAN_CDAB: v32 = ((uint32_t)c << 24) | ((uint32_t)d << 16) | ((uint32_t)a << 8) | b; break;
      case POWERLYTICS_ENDIAN_DCBA: v32 = ((uint32_t)d << 24) | ((uint32_t)c << 16) | ((uint32_t)b << 8) | a; break;
      default: v32 = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | d; break;
    }
  }

  return v32;
}

static float modbus_decode_value(const mb_read_item_t *rc, const uint16_t *regs)
{
  if (!rc || !regs) return 0.0f;

  uint32_t v32 = modbus_decode_u32(rc, regs);
  float raw = 0.0f;
  switch (rc->data_type) {
    case POWERLYTICS_DTYPE_INT16: raw = (float)((int16_t)regs[0]); break;
    case POWERLYTICS_DTYPE_UINT32: raw = (float)v32; break;
    case POWERLYTICS_DTYPE_INT32: raw = (float)((int32_t)v32); break;
    case POWERLYTICS_DTYPE_FLOAT32: {
      union { uint32_t u; float f; } cvt;
      cvt.u = v32;
      raw = cvt.f;
      break;
    }
    case POWERLYTICS_DTYPE_BOOL:
    case POWERLYTICS_DTYPE_BIT:
      raw = ((regs[0] & (uint16_t)(1UL << (rc->bit_index & 0x0FU))) != 0U) ? 1.0f : 0.0f;
      break;
    default:
      raw = (float)regs[0];
      break;
  }

  return (raw * rc->scale) + rc->offset;
}

static HAL_StatusTypeDef mb_apply_slave_serial(const mb_slave_cfg_t *sc)
{
  if (!s_mbm || !sc) return HAL_ERROR;
  return mbm_configure_serial(s_mbm,
                              sc->baud ? sc->baud : 9600UL,
                              sc->data_bits ? sc->data_bits : 8U,
                              (sc->stop_bits == 2U) ? 2U : 1U,
                              (sc->parity <= 2U) ? sc->parity : 0U);
}

static uint8_t update_modbus_digital(modbus_read_runtime_t *rt,
                                     float value,
                                     uint8_t *previous_out,
                                     uint8_t *current_out)
{
  if (!rt) return 0U;
  uint32_t now = HAL_GetTick();
  uint8_t current = (value != 0.0f) ? 1U : 0U;

  if (!rt->seeded) {
    rt->seeded = 1U;
    rt->last = value;
    rt->previous = value;
    rt->last_edge_ms = now;
    return 0U;
  }

  if (((uint8_t)rt->last) != current) {
    uint8_t previous = ((uint8_t)rt->last) ? 1U : 0U;
    rt->transitions++;
    if (previous != 0U) rt->on_duration_ms += now - rt->last_edge_ms;
    rt->last_edge_ms = now;
    rt->previous = rt->last;
    rt->last = value;
    if (previous_out) *previous_out = previous;
    if (current_out) *current_out = current;
    return 1U;
  }

  return 0U;
}

static void evaluate_modbus_digital_rules(const mb_read_item_t *rc,
                                           const mb_slave_cfg_t *sc,
                                           uint8_t previous,
                                           uint8_t current)
{
  if (!rc || !sc) return;

  for (uint8_t i = 0U; i < rc->rule_count && i < POWERLYTICS_MAX_RULES_PER_POINT; i++) {
    const powerlytics_rule_cfg_t *rule = &rc->rules[i];
    if (!rule->enabled) continue;

    char extra[128];
    build_rule_extra(rule, 0U, extra, (int)sizeof(extra));
    if (rule->type == POWERLYTICS_RULE_RISING_EDGE && previous == 0U && current == 1U) {
      (void)event_push_json_x("MI_1", "DI_RISING_EDGE", rule->severity,
                              sc->unique_slave_id, rc->read_id,
                              1.0f, 0.0f, 1.0f, extra[0] ? extra : NULL);
    } else if (rule->type == POWERLYTICS_RULE_FALLING_EDGE && previous == 1U && current == 0U) {
      (void)event_push_json_x("MI_1", "DI_FALLING_EDGE", rule->severity,
                              sc->unique_slave_id, rc->read_id,
                              0.0f, 1.0f, 0.0f, extra[0] ? extra : NULL);
    }
  }
}

static void poll_one_modbus_due(uint32_t now)
{
  if (!s_cfg_valid || s_cfg.slave_count == 0U || !s_mbm) return;

  for (uint8_t si = 0U; si < s_cfg.slave_count; si++) {
    const mb_slave_cfg_t *sc = &s_cfg.slaves[si];
    uint32_t interval = sc->interval_ms ? sc->interval_ms : ENGINE_MODBUS_DEFAULT_INTERVAL_MS;
    if ((int32_t)(now - s_modbus_next_poll[si]) < 0) continue;

    if (sc->read_count == 0U) {
      s_modbus_next_poll[si] = now + interval;
      continue;
    }

    uint8_t ri = s_modbus_next_read[si];
    if (ri >= sc->read_count) ri = 0U;
    const mb_read_item_t *rc = &sc->reads[ri];
    uint16_t qty = rc->qty ? rc->qty : 1U;
    if (qty > ENGINE_MB_REG_MAX) qty = ENGINE_MB_REG_MAX;

    HAL_StatusTypeDef st = HAL_ERROR;
    if (mb_apply_slave_serial(sc) == HAL_OK) {
      st = mb_read_by_func(rc->func, sc->slave_id, rc->start_reg, qty,
                           sc->timeout_ms ? sc->timeout_ms : ENGINE_MODBUS_DEFAULT_TIMEOUT_MS,
                           s_mb_regs);
    }

    if (st == HAL_OK) {
      float value = modbus_decode_value(rc, s_mb_regs);
      modbus_read_runtime_t *rt = &s_mi[si][ri];
      rt->retry_attempt = 0U;
      uint8_t digital_changed = 0U;
      uint8_t digital_previous = 0U;
      uint8_t digital_current = 0U;
      uint32_t primask = critical_enter();
      rt->present = 1U;
      if (rc->kind == POWERLYTICS_MODBUS_KIND_ANALOG) {
        analog_sample(&rt->analog, value);
        rt->last = value;
      } else if (rc->kind == POWERLYTICS_MODBUS_KIND_COUNTER) {
        uint32_t current = modbus_decode_u32(rc, s_mb_regs);
        if (!rt->seeded) {
          rt->counter_baseline = current;
          rt->seeded = 1U;
        }
        rt->counter_last = current;
        rt->last = value;
      } else if (rc->kind == POWERLYTICS_MODBUS_KIND_DIGITAL) {
        digital_changed = update_modbus_digital(rt, value,
                                                 &digital_previous,
                                                 &digital_current);
      } else {
        rt->raw_last = modbus_decode_u32(rc, s_mb_regs);
        rt->last = value;
        rt->seeded = 1U;
      }
      critical_exit(primask);

      if (rc->kind == POWERLYTICS_MODBUS_KIND_ANALOG) {
        evaluate_analog_rules("MI_1", sc->unique_slave_id, rc->read_id,
                              rc->rules, rt->analog.rules, rc->rule_count, value);
      } else if (rc->kind == POWERLYTICS_MODBUS_KIND_DIGITAL && digital_changed) {
        evaluate_modbus_digital_rules(rc, sc, digital_previous, digital_current);
      }
    } else {
      modbus_read_runtime_t *rt = &s_mi[si][ri];
      if (rt->retry_attempt < sc->retries) {
        rt->retry_attempt++;
        s_modbus_next_poll[si] = now + 10U;
        return;
      }
      rt->retry_attempt = 0U;
    }

    s_modbus_next_read[si] = (uint8_t)((ri + 1U) % sc->read_count);
    s_modbus_next_poll[si] = now + interval;
    return;
  }
}

void app_powerlytics_engine_set_flash(W25Q_Handle *flash)
{
  s_flash = flash;
  seq_load();
}

void app_powerlytics_engine_init(mbm_t *mbm)
{
  s_mbm = mbm;
  s_next_sample_ms = HAL_GetTick();
}

/* The last telemetry snapshot built for the cloud (NULL before the first):
 * the SD log copies this instead of building its own, which used to take a
 * seq number and reset the transition counters and min/max windows. */
const char *app_powerlytics_engine_last_telemetry(uint32_t *seq)
{
  if (!s_last_built_valid) return NULL;
  if (seq) *seq = s_last_built_seq;
  return s_telemetry_json;
}

uint8_t app_powerlytics_engine_reported_relays(uint8_t *mask)
{
  if (mask) *mask = s_reported_relays;
  return s_reported_relays_valid;
}

uint16_t app_powerlytics_engine_relay_flags(void)
{
  return s_cfg_valid ? s_cfg.reserved_flags : 0U;
}

void app_powerlytics_engine_apply_config(const modbus_cfg_t *cfg)
{
  if (!cfg) return;

  /* Alarm state across the change: rules that still mean the same thing stay
   * active (no second raise); active rules that are gone send a recovery
   * with reason rule_changed. On the first apply after a reset the active
   * set comes from backup SRAM. */
  static hash_set_t next_rules;
  static hash_set_t active;
  next_rules.count = 0U;
  active.count = 0U;
  visit_analog_rules(cfg, collect_all, &next_rules);
  if (s_cfg_valid) {
    visit_analog_rules(&s_cfg, recover_dropped, &next_rules);
    visit_analog_rules(&s_cfg, collect_active, &active);
  } else if (alarm_state_load(&active) && active.count > 0U) {
    print("[ENGINE] %lu active alarm(s) restored after reset\r\n", (unsigned long)active.count);
  }

  uint32_t primask = critical_enter();
  s_cfg = *cfg;
  s_cfg_valid = 1U;
  memset(s_di, 0, sizeof(s_di));
  memset(s_ai, 0, sizeof(s_ai));
  memset(s_mi, 0, sizeof(s_mi));
  memset(s_di_rule_sent, 0, sizeof(s_di_rule_sent));
  memset(s_di_rule_suppressed, 0, sizeof(s_di_rule_suppressed));
  memset(s_modbus_next_poll, 0, sizeof(s_modbus_next_poll));
  memset(s_modbus_next_read, 0, sizeof(s_modbus_next_read));
  s_events_this_interval = 0U;
  s_next_sample_ms = HAL_GetTick();
  critical_exit(primask);

  visit_analog_rules(&s_cfg, restore_active, &active);
  alarm_state_save();

  uint8_t d[8] = {0};
  app_di_read_all(d);
  uint32_t now = HAL_GetTick();
  for (uint8_t i = 0U; i < ENGINE_DI_CHANNELS; i++) {
    powerlytics_point_cfg_t tmp;
    const powerlytics_point_cfg_t *p = point_or_default("DI_", i, POWERLYTICS_POINT_DIGITAL, &tmp);
    if (!p || p->kind != POWERLYTICS_POINT_DIGITAL) continue;
    s_di[i].present = 1U;
    s_di[i].last = p->active_high ? d[i] : !d[i];
    s_di[i].seeded = 1U;
    s_di[i].last_edge_ms = now;
  }

  print("[ENGINE] config applied points=%u slaves=%u telemetry=%lu ms sample=%lu ms\r\n",
        s_cfg.point_count,
        s_cfg.slave_count,
        (unsigned long)s_cfg.telemetry_interval_ms,
        (unsigned long)s_cfg.sample_interval_ms);
}

void app_powerlytics_engine_sample_due(void)
{
  service_pending_di_edges();

  if (!s_cfg_valid) return;

  uint32_t now = HAL_GetTick();

  if (app_adc_timed_sampler_is_running()) {
    (void)sample_ai_pending();
    flush_di_events();
    poll_one_modbus_due(now);
    return;
  }

  uint32_t interval = app_powerlytics_engine_get_sample_interval_ms();
  if ((int32_t)(now - s_next_sample_ms) < 0) return;
  s_next_sample_ms = now + interval;

  sample_ai_once_blocking();
  flush_di_events();
  poll_one_modbus_due(now);
}

void app_powerlytics_engine_flush_events(void)
{
  if (!s_cfg_valid) return;
  flush_di_events();
}

static void process_di_edge(uint16_t gpio_pin)
{
  uint8_t d[8] = {0};
  app_di_read_all(d);
  uint32_t now = HAL_GetTick();
  int8_t edge_index = app_di_index_from_gpio_pin(gpio_pin);

  if (edge_index < 0 || (uint8_t)edge_index >= ENGINE_DI_CHANNELS) return;

  for (uint8_t i = 0U; i < ENGINE_DI_CHANNELS; i++) {
    if (i != (uint8_t)edge_index) continue;

    powerlytics_point_cfg_t tmp;
    const powerlytics_point_cfg_t *p = point_or_default("DI_", i, POWERLYTICS_POINT_DIGITAL, &tmp);
    if (!p || p->kind != POWERLYTICS_POINT_DIGITAL) return;

    uint8_t current = p->active_high ? d[i] : !d[i];
    digital_runtime_t *rt = &s_di[i];
    if (!rt->seeded) {
      rt->present = 1U;
      rt->last = current;
      rt->seeded = 1U;
      rt->last_edge_ms = now;
      return;
    }

    if (current == rt->last) return;

    uint8_t previous = rt->last;
    rt->transitions++;
    if (previous) rt->on_duration_ms += now - rt->last_edge_ms;
    rt->last_edge_ms = now;
    rt->last = current;

    uint8_t rule_count = di_event_rule_count(p);
    for (uint8_t r = 0U; r < rule_count; r++) {
      powerlytics_rule_cfg_t rule;
      if (!di_event_rule_at(p, r, &rule)) continue;
      if (rule.type == POWERLYTICS_RULE_RISING_EDGE && previous == 0U && current == 1U) {
        s_di_event_previous[i][r] = previous;
        s_di_event_current[i][r] = current;
        s_di_event_pending[i][r] = 1U;
      } else if (rule.type == POWERLYTICS_RULE_FALLING_EDGE && previous == 1U && current == 0U) {
        s_di_event_previous[i][r] = previous;
        s_di_event_current[i][r] = current;
        s_di_event_pending[i][r] = 1U;
      }
    }
    return;
  }
}

void app_powerlytics_engine_di_edge_isr(uint16_t gpio_pin)
{
  int8_t edge_index = app_di_index_from_gpio_pin(gpio_pin);
  if (edge_index < 0 || (uint8_t)edge_index >= ENGINE_DI_CHANNELS) return;

  /* Keep EXTI work bounded. Debounce and rule processing run in ModbusTask. */
  EXTI->IMR &= ~(uint32_t)gpio_pin;
  s_di_irq_tick[(uint8_t)edge_index] = HAL_GetTick();
  s_di_irq_pending_mask |= gpio_pin;
}

static void service_pending_di_edges(void)
{
  uint32_t now = HAL_GetTick();

  for (uint8_t bit = 0U; bit < 16U; bit++) {
    uint16_t gpio_pin = (uint16_t)(1UL << bit);
    int8_t edge_index = app_di_index_from_gpio_pin(gpio_pin);
    if (edge_index < 0 || (uint8_t)edge_index >= ENGINE_DI_CHANNELS) continue;

    uint8_t i = (uint8_t)edge_index;
    uint8_t ready = 0U;
    powerlytics_point_cfg_t debounce_tmp;
    const powerlytics_point_cfg_t *point = point_or_default("DI_", i, POWERLYTICS_POINT_DIGITAL, &debounce_tmp);
    uint32_t debounce_ms = point ? point->debounce_ms : ENGINE_DI_DEBOUNCE_MS;

    uint32_t primask = critical_enter();
    if ((s_di_irq_pending_mask & gpio_pin) != 0U &&
        (uint32_t)(now - s_di_irq_tick[i]) >= debounce_ms) {
      s_di_irq_pending_mask &= (uint16_t)~gpio_pin;
      ready = 1U;
    }
    critical_exit(primask);

    if (!ready) continue;

    process_di_edge(gpio_pin);

    /* Discard bounce accumulated while masked, then accept the next edge. */
    EXTI->PR = gpio_pin;
    EXTI->IMR |= gpio_pin;
  }
}

static void append_analog_reading(char *json, int max, int *idx, const analog_runtime_t *a)
{
  float avg = (a && a->samples) ? (float)(a->sum / (double)a->samples) : 0.0f;
  jb_append(json, max, idx, "{\"last\":"); append_float(json, max, idx, a ? a->last : 0.0f);
  jb_append(json, max, idx, ",\"min\":"); append_float(json, max, idx, (a && a->samples) ? a->min : 0.0f);
  jb_append(json, max, idx, ",\"max\":"); append_float(json, max, idx, (a && a->samples) ? a->max : 0.0f);
  jb_append(json, max, idx, ",\"avg\":"); append_float(json, max, idx, avg);
  jb_append(json, max, idx, ",\"minTs\":\"%s\",\"maxTs\":\"%s\"}",
            (a && a->min_ts[0]) ? a->min_ts : "",
            (a && a->max_ts[0]) ? a->max_ts : "");
}

const char *app_powerlytics_engine_build_telemetry(void)
{
  if (!app_powerlytics_engine_has_valid_config()) return NULL;

  char ts[40];
  rtc_get_datetime_strings(ts, sizeof(ts));

  uint32_t primask = critical_enter();
  uint32_t seq = s_next_telemetry_seq++;
  s_last_built_seq = seq;
  s_last_built_valid = 1U;
  int idx = 0;
  uint8_t first = 1U;

  jb_append(s_telemetry_json, ENGINE_JSON_MAX, &idx,
            "{\"v\":1,\"type\":\"telemetry\",\"imei\":\"%s\",\"configId\":\"%s\","
            "\"configHash\":\"%.16s\",\"bootId\":\"%s\",\"seq\":%lu,\"ts\":\"%s\",\"values\":{",
            s_cfg.imei,
            s_cfg.config_id,
            s_cfg.hash,   /* first 16 hex of the applied config's hash: which config produced this */
            app_boot_id,  /* lets the platform drop an older redelivery from the same boot (FH-004) */
            (unsigned long)seq,
            ts);

  for (uint8_t i = 0U; i < ENGINE_DI_CHANNELS; i++) {
    powerlytics_point_cfg_t tmp;
    const powerlytics_point_cfg_t *p = point_or_default("DI_", i, POWERLYTICS_POINT_DIGITAL, &tmp);
    if (!p || p->kind != POWERLYTICS_POINT_DIGITAL) continue;
    if (!first) jb_append(s_telemetry_json, ENGINE_JSON_MAX, &idx, ",");
    first = 0U;
    jb_append(s_telemetry_json, ENGINE_JSON_MAX, &idx,
              "\"%s\":{\"last\":%u,\"transitions\":%lu,\"onDurationMs\":%lu}",
              p->key,
              s_di[i].last ? 1U : 0U,
              (unsigned long)s_di[i].transitions,
              (unsigned long)(s_di[i].on_duration_ms + (s_di[i].last ? (HAL_GetTick() - s_di[i].last_edge_ms) : 0U)));
    s_di[i].transitions = 0U;
    s_di[i].on_duration_ms = 0U;
    s_di[i].last_edge_ms = HAL_GetTick();
  }

  for (uint8_t i = 0U; i < ENGINE_AI_CHANNELS; i++) {
    app_watchdog_force_refresh();
    powerlytics_point_cfg_t tmp;
    const powerlytics_point_cfg_t *p = point_or_default("AI_", i, POWERLYTICS_POINT_ANALOG, &tmp);
    if (!p || p->kind != POWERLYTICS_POINT_ANALOG) continue;
    if (!first) jb_append(s_telemetry_json, ENGINE_JSON_MAX, &idx, ",");
    first = 0U;
    jb_append(s_telemetry_json, ENGINE_JSON_MAX, &idx, "\"%s\":", p->key);
    append_analog_reading(s_telemetry_json, ENGINE_JSON_MAX, &idx, &s_ai[i]);
    analog_reset_interval(&s_ai[i]);
  }

#ifndef BOARD_RELAY_COUNT
#define BOARD_RELAY_COUNT 5U
#endif
  for (uint8_t i = 0U; i < BOARD_RELAY_COUNT; i++) {
    /* Relay outputs are reported even when their point is disabled: a
     * relay can still be switched while "inactive", and this report is how
     * the platform learns the real output state (after a reboot the relays
     * come up OFF while the platform still holds the last acked state). The
     * platform never gates relay reports by port status. */
    powerlytics_point_cfg_t tmp;
    const powerlytics_point_cfg_t *p = find_channel_point_any("RO_", i);
    if (!p) {
      make_default_point(&tmp, "RO_", i, POWERLYTICS_POINT_RELAY);
      p = &tmp;
    }
    if (p->kind != POWERLYTICS_POINT_RELAY) continue;
    if (!first) jb_append(s_telemetry_json, ENGINE_JSON_MAX, &idx, ",");
    first = 0U;
    uint8_t relay_on = app_relay_get_by_key(p->key) ? 1U : 0U;
    if (i < 8U) {
      s_reported_relays = (uint8_t)((s_reported_relays & ~(1U << i)) | (relay_on << i));
      s_reported_relays_valid = 1U;
    }
    jb_append(s_telemetry_json, ENGINE_JSON_MAX, &idx, "\"%s\":{\"last\":%u}",
              p->key, relay_on);
  }

  if (s_cfg.slave_count > 0U) {
    if (!first) jb_append(s_telemetry_json, ENGINE_JSON_MAX, &idx, ",");
    first = 0U;
    jb_append(s_telemetry_json, ENGINE_JSON_MAX, &idx, "\"MI_1\":[");
    for (uint8_t si = 0U; si < s_cfg.slave_count; si++) {
      app_watchdog_force_refresh();
      const mb_slave_cfg_t *sc = &s_cfg.slaves[si];
      if (si > 0U) jb_append(s_telemetry_json, ENGINE_JSON_MAX, &idx, ",");
      jb_append(s_telemetry_json, ENGINE_JSON_MAX, &idx, "{\"slaveId\":\"%s\",\"registers\":[", sc->unique_slave_id);
      for (uint8_t ri = 0U; ri < sc->read_count; ri++) {
        app_watchdog_force_refresh();
        const mb_read_item_t *rc = &sc->reads[ri];
        modbus_read_runtime_t *rt = &s_mi[si][ri];
        if (ri > 0U) jb_append(s_telemetry_json, ENGINE_JSON_MAX, &idx, ",");
        jb_append(s_telemetry_json, ENGINE_JSON_MAX, &idx, "{\"readId\":\"%s\",", rc->read_id);
        if (rc->kind == POWERLYTICS_MODBUS_KIND_ANALOG) {
          jb_append(s_telemetry_json, ENGINE_JSON_MAX, &idx, "\"last\":"); append_float(s_telemetry_json, ENGINE_JSON_MAX, &idx, rt->last);
          jb_append(s_telemetry_json, ENGINE_JSON_MAX, &idx, ",\"min\":"); append_float(s_telemetry_json, ENGINE_JSON_MAX, &idx, rt->analog.samples ? rt->analog.min : rt->last);
          jb_append(s_telemetry_json, ENGINE_JSON_MAX, &idx, ",\"max\":"); append_float(s_telemetry_json, ENGINE_JSON_MAX, &idx, rt->analog.samples ? rt->analog.max : rt->last);
          jb_append(s_telemetry_json, ENGINE_JSON_MAX, &idx, ",\"avg\":"); append_float(s_telemetry_json, ENGINE_JSON_MAX, &idx, rt->analog.samples ? (float)(rt->analog.sum / (double)rt->analog.samples) : rt->last);
          jb_append(s_telemetry_json, ENGINE_JSON_MAX, &idx, ",\"minTs\":\"%s\",\"maxTs\":\"%s\"}", rt->analog.min_ts, rt->analog.max_ts);
          analog_reset_interval(&rt->analog);
        } else if (rc->kind == POWERLYTICS_MODBUS_KIND_COUNTER) {
          uint32_t current = rt->counter_last;
          uint32_t delta = app_powerlytics_counter_delta(rt->counter_baseline, current, 0xFFFFFFFFUL);
          rt->counter_baseline = current;
          jb_append(s_telemetry_json, ENGINE_JSON_MAX, &idx,
                    "\"last\":%lu,\"count\":%lu,\"delta\":%lu}",
                    (unsigned long)current, (unsigned long)delta, (unsigned long)delta);
        } else if (rc->kind == POWERLYTICS_MODBUS_KIND_DIGITAL) {
          jb_append(s_telemetry_json, ENGINE_JSON_MAX, &idx,
                    "\"last\":%u,\"transitions\":%lu,\"onDurationMs\":%lu}",
                    (rt->last != 0.0f) ? 1U : 0U,
                    (unsigned long)rt->transitions,
                    (unsigned long)rt->on_duration_ms);
          rt->transitions = 0U;
          rt->on_duration_ms = 0U;
        } else {
          jb_append(s_telemetry_json, ENGINE_JSON_MAX, &idx,
                    "\"last\":%lu}",
                    (unsigned long)rt->raw_last);
        }
      }
      jb_append(s_telemetry_json, ENGINE_JSON_MAX, &idx, "]}");
    }
    jb_append(s_telemetry_json, ENGINE_JSON_MAX, &idx, "]");
  }

  jb_append(s_telemetry_json, ENGINE_JSON_MAX, &idx, "}}");
  if (idx >= (int)ENGINE_JSON_MAX) {
    s_telemetry_json[ENGINE_JSON_MAX - 1U] = '\0';
    critical_exit(primask);
    app_log_critical(APP_LOG_ERROR, "PAYLOAD", "TELEMETRY_JSON_OVERFLOW",
                     "telemetry JSON exceeded engine buffer", ENGINE_JSON_MAX, 0, "CONFIG_REDUCE_POINTS");
    return NULL;
  }

  s_telemetry_json[idx] = '\0';
  s_events_this_interval = 0U;
  critical_exit(primask);
  app_watchdog_force_refresh();
  seq_store();

  print("[PAYLOAD] telemetry built len=%lu config=%s seq=%lu\r\n",
        (unsigned long)strlen(s_telemetry_json),
        s_cfg.config_id,
        (unsigned long)seq);
  return s_telemetry_json;
}

uint8_t app_powerlytics_engine_pop_event(char *payload, size_t payload_size)
{
  app_powerlytics_engine_flush_events();

  if (!payload || payload_size == 0U || s_event_r == s_event_w) return 0U;

  uint32_t primask = critical_enter();
  snprintf(payload, payload_size, "%s", s_event_queue[s_event_r]);
  s_event_r = (uint8_t)((s_event_r + 1U) % ENGINE_EVENT_QUEUE);
  critical_exit(primask);
  return 1U;
}

const char *app_powerlytics_engine_get_device_id(void)
{
  return (s_cfg_valid && s_cfg.device_id[0]) ? s_cfg.device_id : "unknown_device";
}

const char *app_powerlytics_engine_get_config_id(void)
{
  return (s_cfg_valid && s_cfg.config_id[0]) ? s_cfg.config_id : "unknown_config";
}

const char *app_powerlytics_engine_get_config_hash(void)
{
  return (s_cfg_valid && s_cfg.hash[0]) ? s_cfg.hash : "";
}

const char *app_powerlytics_engine_get_api_key(void)
{
  return (s_cfg_valid && s_cfg.api_key[0]) ? s_cfg.api_key : "";
}

uint8_t app_powerlytics_engine_has_valid_config(void)
{
  return (s_cfg_valid && cfg_string_valid(s_cfg.device_id) &&
          cfg_string_valid(s_cfg.config_id) && cfg_string_valid(s_cfg.api_key)) ? 1U : 0U;
}

uint32_t app_powerlytics_engine_get_telemetry_interval_ms(void)
{
  uint32_t interval = s_cfg_valid ? s_cfg.telemetry_interval_ms : MODBUS_DEFAULT_TELEMETRY_INTERVAL_MS;
  if (interval < MODBUS_MIN_TELEMETRY_INTERVAL_MS) return MODBUS_DEFAULT_TELEMETRY_INTERVAL_MS;
  if (interval > MODBUS_MAX_TELEMETRY_INTERVAL_MS) return MODBUS_MAX_TELEMETRY_INTERVAL_MS;
  return interval;
}

uint32_t app_powerlytics_engine_get_sample_interval_ms(void)
{
  uint32_t interval = s_cfg_valid ? s_cfg.sample_interval_ms : POWERLYTICS_DEFAULT_SAMPLE_INTERVAL_MS;
  if (interval < POWERLYTICS_MIN_SAMPLE_INTERVAL_MS) return POWERLYTICS_DEFAULT_SAMPLE_INTERVAL_MS;
  if (interval > POWERLYTICS_MAX_SAMPLE_INTERVAL_MS) return POWERLYTICS_MAX_SAMPLE_INTERVAL_MS;
  return interval;
}
