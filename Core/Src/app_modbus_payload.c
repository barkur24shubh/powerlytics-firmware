#include "app_modbus_payload.h"
#include "app_board_io.h"
#include "debug_uart.h"
#include "rtc_manager.h"
#include "cmsis_os.h"
#include "app_debug_led.h"
#include "app_watchdog.h"
#include "app_health.h"
#include "app_log.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#include "modbus_config.h"
#include "modbus_rtu_master.h"   // for mbm_read_* prototypes

static modbus_cfg_t s_cfg;
static uint8_t s_cfg_valid = 0;
static uint32_t s_modbus_payload_init_tick = 0U;
static uint8_t s_mb_read_ready[MODBUS_MAX_SLAVES][MODBUS_MAX_READS_PER_SLAVE];

/* Static refs */
static mbm_t *s_mbm = NULL;

/* Buffers (avoid stack overflow) */
#define MB_JSON_MAX   3072
#define MB_REG_MAX    125
#define MODBUS_INTER_RETRY_DELAY_MS        200U
#define MODBUS_STARTUP_ZERO_FILTER_MS      180000UL
#define MODBUS_STARTUP_ZERO_RETRY_DELAY_MS 1000U
#define MODBUS_STARTUP_ZERO_RETRIES        3U
static char     g_payload_json[MB_JSON_MAX];
static uint16_t g_mb_regs[MB_REG_MAX];
static uint8_t  s_payload_overflow_reported = 0U;
static uint8_t  s_modbus_serial_valid = 0U;
static uint8_t  s_modbus_serial_error_reported = 0U;
static uint32_t s_modbus_serial_baud = 0U;
static uint8_t  s_modbus_serial_data_bits = 0U;
static uint8_t  s_modbus_serial_stop_bits = 0U;
static uint8_t  s_modbus_serial_parity = 0U;

static uint8_t cfg_string_valid(const char *s)
{
  if (!s || s[0] == '\0') {
    return 0U;
  }

  if (strcmp(s, "unknown_device") == 0 ||
      strcmp(s, "unknown_config") == 0 ||
      strcmp(s, "missing") == 0) {
    return 0U;
  }

  return 1U;
}

/* ----- json builder ----- */
static int jb_append(char *dst, int max, int *idx, const char *fmt, ...)
{
  if (*idx >= max) return -1;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(&dst[*idx], (size_t)(max - *idx), fmt, ap);
  va_end(ap);
  if (n < 0) return -1;
  if (n >= (max - *idx)) { *idx = max; return -1; }
  *idx += n;
  return 0;
}

static uint8_t jb_overflowed(int idx, int max)
{
  return (idx >= max) ? 1U : 0U;
}

static void report_payload_overflow_once(void)
{
  if (s_payload_overflow_reported) {
    return;
  }

  s_payload_overflow_reported = 1U;

  print("[PAYLOAD][ERR] telemetry JSON exceeded buffer=%u bytes, publish skipped\r\n",
        (unsigned)MB_JSON_MAX);

  app_log_critical(APP_LOG_ERROR,
                   "PAYLOAD",
                   "TELEMETRY_JSON_OVERFLOW",
                   "telemetry JSON exceeded local buffer, publish skipped",
                   MB_JSON_MAX,
                   0,
                   "CONFIG_REDUCE_READS");
}

/* ----- public init ----- */
void app_modbus_payload_init(mbm_t *mbm)
{
  s_mbm = mbm;
  s_modbus_payload_init_tick = HAL_GetTick();
}

/* ---- config setter ---- */
void app_modbus_payload_set_cfg(const modbus_cfg_t *cfg)
{
  if (!cfg) return;
  s_cfg = *cfg;          // struct copy
  s_cfg_valid = 1;
  s_payload_overflow_reported = 0U;
  s_modbus_serial_valid = 0U;
  s_modbus_serial_error_reported = 0U;
  memset(s_mb_read_ready, 0, sizeof(s_mb_read_ready));
  s_modbus_payload_init_tick = HAL_GetTick();
}

/* =========================================================
   JSON helpers (new expected format)
   ========================================================= */

static void json_values_add_di_2(char *json, int max, int *idx, uint8_t *first_kv)
{
  uint8_t d[8] = {0};
  app_di_read_all(d);

  // only DI_1, DI_2 (as per your requirement)
  for (int i = 0; i < 8; i++) {
    if (!(*first_kv)) jb_append(json, max, idx, ",");
    *first_kv = 0;
    jb_append(json, max, idx, "\"DI_%d\":%u", i + 1, (unsigned)d[i]);
  }
}

static void json_values_add_ai_2_mv(char *json, int max, int *idx, uint8_t *first_kv)
{
  uint32_t mv[10] = {0};
  app_adc_read_all_mv(mv);

  // only AI_1, AI_2 (as per your requirement)
  for (int i = 0; i < 10; i++) {
    if (!(*first_kv)) jb_append(json, max, idx, ",");
    *first_kv = 0;
    jb_append(json, max, idx, "\"AI_%d\":%lu", i + 1, (unsigned long)mv[i]);
  }
}

static void json_add_signal(char *json, int max, int *idx)
{
  app_signal_snapshot_t sig;

  if (!app_health_get_signal_snapshot(&sig)) {
    sig.valid = 0U;
    sig.rssi = 99;
    sig.ber = 99;
    sig.dbm = -999;
    sig.ageMs = 0U;
    sig.quality = "unknown";
  }

  jb_append(json,
            max,
            idx,
            "\"signal\":{\"valid\":%s,\"quality\":\"%s\",\"rssi\":%ld,\"dbm\":%ld,\"ber\":%ld,\"ageMs\":%lu},",
            sig.valid ? "true" : "false",
            sig.quality ? sig.quality : "unknown",
            (long)sig.rssi,
            (long)sig.dbm,
            (long)sig.ber,
            (unsigned long)sig.ageMs);
}

/* One read execution (no optimization, 1 readId = 1 request) */
static HAL_StatusTypeDef mb_read_by_func(uint8_t func,
                                        uint8_t slave_id,
                                        uint16_t start,
                                        uint16_t qty,
                                        uint32_t timeout_ms,
                                        uint16_t *out_regs)
{
  if (!s_mbm) return HAL_ERROR;

  if (func == MB_FUNC_HOLDING_03 || func == 3) {
    return mbm_read_holding_03(s_mbm, slave_id, start, qty, timeout_ms, out_regs);
  } else if (func == MB_FUNC_INPUT_04 || func == 4) {
    return mbm_read_input_04(s_mbm, slave_id, start, qty, timeout_ms, out_regs);
  }
  return HAL_ERROR;
}

static HAL_StatusTypeDef mb_apply_slave_serial(const mb_slave_cfg_t *sc)
{
  if (!s_mbm || !sc) {
    return HAL_ERROR;
  }

  uint32_t baud = (sc->baud > 0U) ? sc->baud : 9600U;
  uint8_t data_bits = (sc->data_bits > 0U) ? sc->data_bits : 8U;
  uint8_t stop_bits = (sc->stop_bits == 2U) ? 2U : 1U;
  uint8_t parity = (sc->parity <= 2U) ? sc->parity : 0U;

  HAL_StatusTypeDef st = mbm_configure_serial(s_mbm,
                                              baud,
                                              data_bits,
                                              stop_bits,
                                              parity);

  if (st != HAL_OK) {
    if (!s_modbus_serial_error_reported) {
      s_modbus_serial_error_reported = 1U;

      print("[MODBUS][ERR] UART config failed sid=%u baud=%lu db=%u sb=%u par=%u\r\n",
            sc->slave_id,
            (unsigned long)baud,
            data_bits,
            stop_bits,
            parity);

      app_log_push(APP_LOG_ERROR,
                   "MODBUS",
                   "UART_CONFIG_FAILED",
                   "Modbus UART serial config failed",
                   (int32_t)sc->slave_id,
                   (int32_t)baud);
    }

    return st;
  }

  if (!s_modbus_serial_valid ||
      s_modbus_serial_baud != baud ||
      s_modbus_serial_data_bits != data_bits ||
      s_modbus_serial_stop_bits != stop_bits ||
      s_modbus_serial_parity != parity) {
    print("[MODBUS] UART configured baud=%lu db=%u sb=%u par=%u\r\n",
          (unsigned long)baud,
          data_bits,
          stop_bits,
          parity);

    s_modbus_serial_valid = 1U;
    s_modbus_serial_baud = baud;
    s_modbus_serial_data_bits = data_bits;
    s_modbus_serial_stop_bits = stop_bits;
    s_modbus_serial_parity = parity;
  }

  s_modbus_serial_error_reported = 0U;
  return HAL_OK;
}

static uint8_t mb_regs_all_zero(const uint16_t *regs, uint16_t qty)
{
  if (!regs || qty == 0) return 0U;

  for (uint16_t i = 0; i < qty; i++) {
    if (regs[i] != 0U) return 0U;
  }

  return 1U;
}

static uint8_t mb_startup_zero_filter_active(void)
{
  return ((HAL_GetTick() - s_modbus_payload_init_tick) < MODBUS_STARTUP_ZERO_FILTER_MS) ? 1U : 0U;
}

static void mb_poll_delay(uint32_t delay_ms)
{
  uint32_t start = HAL_GetTick();

  while ((HAL_GetTick() - start) < delay_ms) {
    app_watchdog_mark_task(APP_WATCHDOG_TASK_RUNTIME);
    app_watchdog_mark_task(APP_WATCHDOG_TASK_MODBUS);
    app_debug_led_run_once();
    app_watchdog_refresh();
    osDelay(100U);
  }
}

static HAL_StatusTypeDef mb_read_with_retries(const mb_slave_cfg_t *sc,
                                              const mb_read_item_t *rc,
                                              uint16_t qty,
                                              uint16_t *out_regs)
{
  uint8_t attempts = (uint8_t)(sc->retries + 1U);
  uint32_t timeout_ms = (sc->timeout_ms > 0U) ? sc->timeout_ms : 2000U;

  if (attempts == 0U) attempts = 1U;
  if (attempts > 5U) attempts = 5U;

  HAL_StatusTypeDef last_st = HAL_ERROR;

  for (uint8_t attempt = 0; attempt < attempts; attempt++) {
    last_st = mb_read_by_func(rc->func,
                              sc->slave_id,
                              rc->start_reg,
                              qty,
                              timeout_ms,
                              out_regs);

    if (last_st == HAL_OK) {
      return HAL_OK;
    }

    if ((attempt + 1U) < attempts) {
      mb_poll_delay(MODBUS_INTER_RETRY_DELAY_MS);
    }
  }

  return last_st;
}

static const char *hal_status_name(HAL_StatusTypeDef st)
{
  switch (st) {
    case HAL_OK: return "OK";
    case HAL_ERROR: return "ERROR";
    case HAL_BUSY: return "BUSY";
    case HAL_TIMEOUT: return "TIMEOUT";
    default: return "UNKNOWN";
  }
}

static HAL_StatusTypeDef mb_read_startup_filtered(uint8_t slave_index,
                                                  uint8_t read_index,
                                                  const mb_slave_cfg_t *sc,
                                                  const mb_read_item_t *rc,
                                                  uint16_t qty,
                                                  uint16_t *out_regs)
{
  HAL_StatusTypeDef st = mb_read_with_retries(sc, rc, qty, out_regs);

  if (st != HAL_OK || qty == 0U) {
    return st;
  }

  if (!mb_regs_all_zero(out_regs, qty)) {
    s_mb_read_ready[slave_index][read_index] = 1U;
    return HAL_OK;
  }

  if (s_mb_read_ready[slave_index][read_index]) {
    return HAL_OK;
  }

  if (!mb_startup_zero_filter_active()) {
    s_mb_read_ready[slave_index][read_index] = 1U;
    return HAL_OK;
  }

  for (uint8_t attempt = 0; attempt < MODBUS_STARTUP_ZERO_RETRIES; attempt++) {
    print("[MODBUS][WARMUP] zero read sid=%u readId=%s retry=%u\r\n",
          sc->slave_id,
          rc->read_id,
          (unsigned)(attempt + 1U));

    mb_poll_delay(MODBUS_STARTUP_ZERO_RETRY_DELAY_MS);

    st = mb_read_with_retries(sc, rc, qty, out_regs);
    if (st != HAL_OK) {
      return st;
    }

    if (!mb_regs_all_zero(out_regs, qty)) {
      s_mb_read_ready[slave_index][read_index] = 1U;
      return HAL_OK;
    }
  }

  print("[MODBUS][WARMUP] suppressing startup zero sid=%u readId=%s\r\n",
        sc->slave_id,
        rc->read_id);

  return HAL_BUSY;
}

/* Add MI_1 populated from config */
static void json_values_add_mi_1(char *json, int max, int *idx, uint8_t *first_kv)
{
  if (!(*first_kv)) jb_append(json, max, idx, ",");
  *first_kv = 0;

  jb_append(json, max, idx, "\"MI_1\":[");

  if (!s_cfg_valid || s_cfg.slave_count == 0) {
    jb_append(json, max, idx, "]");
    return;
  }

  for (uint8_t si = 0; si < s_cfg.slave_count; si++) {
    const mb_slave_cfg_t *sc = &s_cfg.slaves[si];
    uint8_t serial_ok = (mb_apply_slave_serial(sc) == HAL_OK) ? 1U : 0U;

    if (si > 0) jb_append(json, max, idx, ",");

    // In your expected response field name is "slave_id" but value is UUID
    jb_append(json, max, idx, "{");
    jb_append(json, max, idx, "\"slave_id\":\"%s\",", sc->unique_slave_id);
    jb_append(json, max, idx, "\"registers\":[");

    for (uint8_t ri = 0; ri < sc->read_count; ri++) {
      const mb_read_item_t *rc = &sc->reads[ri];

      if (ri > 0) jb_append(json, max, idx, ",");

      // execute read (raw words only)
      uint16_t qty = rc->qty;
      if (qty == 0 || qty > MB_REG_MAX) qty = 0;

      HAL_StatusTypeDef st = HAL_ERROR;
      if (serial_ok && qty > 0) {
        st = mb_read_startup_filtered(si,
                                      ri,
                                      sc,
                                      rc,
                                      qty,
                                      g_mb_regs);
      }

      if (st != HAL_OK) {
        print("[MODBUS][READ_FAIL] sid=%u readId=%s fc=%u start=%u qty=%u timeout=%lu st=%s\r\n",
              sc->slave_id,
              rc->read_id,
              rc->func,
              rc->start_reg,
              qty,
              (unsigned long)((sc->timeout_ms > 0U) ? sc->timeout_ms : 2000U),
              hal_status_name(st));
      }

      jb_append(json, max, idx, "{");
      jb_append(json, max, idx, "\"readId\":\"%s\",", rc->read_id);

      // "value":[...]
      jb_append(json, max, idx, "\"value\":[");
      if (st == HAL_OK && qty > 0) {
        for (uint16_t k = 0; k < qty; k++) {
          if (k > 0) jb_append(json, max, idx, ",");
          jb_append(json, max, idx, "%u", (unsigned)g_mb_regs[k]);
        }
      }
      jb_append(json, max, idx, "]");

      jb_append(json, max, idx, "}");
    }

    jb_append(json, max, idx, "]");
    jb_append(json, max, idx, "}");
  }

  jb_append(json, max, idx, "]");
}

/* ----- build full payload ----- */
//const char* app_build_payload_json_once(void)
//{
//  char ts[40];
//  rtc_get_datetime_strings(ts, sizeof(ts));
//
//  int idx = 0;
//  char *json = g_payload_json;
//
//  jb_append(json, MB_JSON_MAX, &idx, "{");
//
//  // deviceId: use from config if available, else fallback empty
//  const char *dev_id = (s_cfg_valid && s_cfg.device_id[0]) ? s_cfg.device_id : "";
//  jb_append(json, MB_JSON_MAX, &idx, "\"deviceId\":\"%s\",", dev_id);
//
//  // ts
//  jb_append(json, MB_JSON_MAX, &idx, "\"ts\":\"%s\",", ts);
//
//  // values object
//  jb_append(json, MB_JSON_MAX, &idx, "\"values\":{");
//
//  uint8_t first_kv = 1;
//
//  // DI_1..DI_2 only
//  json_values_add_di_2(json, MB_JSON_MAX, &idx, &first_kv);
//
//  // AI_1..AI_2 only (mv)
//  json_values_add_ai_2_mv(json, MB_JSON_MAX, &idx, &first_kv);
//
//  // MI_1 populated
//  json_values_add_mi_1(json, MB_JSON_MAX, &idx, &first_kv);
//
//  jb_append(json, MB_JSON_MAX, &idx, "}"); // end values
//  jb_append(json, MB_JSON_MAX, &idx, "}"); // end root
//
//  if (idx >= MB_JSON_MAX) json[MB_JSON_MAX - 1] = '\0';
//  else json[idx] = '\0';
//
//  print("%s\r\n", json);
//  return json;
//}
/* ----- build full payload ----- */
const char* app_build_payload_json_once(void)
{
  char ts[40];
  rtc_get_datetime_strings(ts, sizeof(ts));

  int idx = 0;
  char *json = g_payload_json;

  jb_append(json, MB_JSON_MAX, &idx, "{");

  // apiKey: use from config if available, else fallback empty
  const char *api_key = (s_cfg_valid && s_cfg.api_key[0]) ? s_cfg.api_key : "";
  jb_append(json, MB_JSON_MAX, &idx, "\"apiKey\":\"%s\",", api_key);

  // deviceId: use from config if available, else fallback empty
  const char *dev_id = (s_cfg_valid && s_cfg.device_id[0]) ? s_cfg.device_id : "";
  jb_append(json, MB_JSON_MAX, &idx, "\"deviceId\":\"%s\",", dev_id);

  // ts
  jb_append(json, MB_JSON_MAX, &idx, "\"ts\":\"%s\",", ts);

  json_add_signal(json, MB_JSON_MAX, &idx);

  // values object
  jb_append(json, MB_JSON_MAX, &idx, "\"values\":{");

  uint8_t first_kv = 1;

  // DI_1..DI_8
  json_values_add_di_2(json, MB_JSON_MAX, &idx, &first_kv);

  // AI_1..AI_10
  json_values_add_ai_2_mv(json, MB_JSON_MAX, &idx, &first_kv);

  // MI_1 populated
  json_values_add_mi_1(json, MB_JSON_MAX, &idx, &first_kv);

  jb_append(json, MB_JSON_MAX, &idx, "}"); // end values
  jb_append(json, MB_JSON_MAX, &idx, "}"); // end root

  if (jb_overflowed(idx, MB_JSON_MAX)) {
    json[MB_JSON_MAX - 1] = '\0';
    report_payload_overflow_once();
    return NULL;
  }

  json[idx] = '\0';

  print("%s\r\n", json);
  s_payload_overflow_reported = 0U;
  return json;
}


const char* app_modbus_payload_get_device_id(void)
{
    if (!s_cfg_valid || s_cfg.device_id[0] == '\0') {
        return "unknown_device";
    }

    return s_cfg.device_id;
}

const char* app_modbus_payload_get_config_id(void)
{
    if (!s_cfg_valid || s_cfg.config_id[0] == '\0') {
        return "unknown_config";
    }

    return s_cfg.config_id;
}

const char* app_modbus_payload_get_api_key(void)
{
    if (!s_cfg_valid || s_cfg.api_key[0] == '\0') {
        return "";
    }

    return s_cfg.api_key;
}

uint8_t app_modbus_payload_has_valid_config(void)
{
    if (!s_cfg_valid) {
        return 0U;
    }

    if (!cfg_string_valid(s_cfg.device_id)) {
        return 0U;
    }

    if (!cfg_string_valid(s_cfg.config_id)) {
        return 0U;
    }

    if (!cfg_string_valid(s_cfg.api_key)) {
        return 0U;
    }

    return 1U;
}

uint32_t app_modbus_payload_get_telemetry_interval_ms(void)
{
    uint32_t interval_s = MODBUS_DEFAULT_TELEMETRY_INTERVAL_S;

    if (s_cfg_valid) {
        interval_s = s_cfg.telemetry_interval_s;
    }

    if (interval_s < MODBUS_MIN_TELEMETRY_INTERVAL_S) {
        interval_s = MODBUS_DEFAULT_TELEMETRY_INTERVAL_S;
    } else if (interval_s > MODBUS_MAX_TELEMETRY_INTERVAL_S) {
        interval_s = MODBUS_MAX_TELEMETRY_INTERVAL_S;
    }

    return interval_s * 1000UL;
}
