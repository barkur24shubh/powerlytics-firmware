#include "modbus_config_json.h"
#include "cJSON.h"
#include "app_rule_eval.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define MODBUS_DEFAULT_SLAVE_BAUD        9600UL
#define MODBUS_DEFAULT_SLAVE_DATA_BITS   8U
#define MODBUS_DEFAULT_SLAVE_STOP_BITS   1U
#define MODBUS_DEFAULT_SLAVE_PARITY      0U
#define MODBUS_DEFAULT_SLAVE_INTERVAL_MS 1000UL
#define MODBUS_DEFAULT_SLAVE_TIMEOUT_MS  2000UL
#define MODBUS_DEFAULT_SLAVE_RETRIES     3U

static cJSON *json_get_any(cJSON *obj, const char *a, const char *b, const char *c)
{
  cJSON *item = NULL;

  if (!cJSON_IsObject(obj)) return NULL;

  if (a) {
    item = cJSON_GetObjectItemCaseSensitive(obj, a);
    if (item) return item;
  }

  if (b) {
    item = cJSON_GetObjectItemCaseSensitive(obj, b);
    if (item) return item;
  }

  if (c) {
    item = cJSON_GetObjectItemCaseSensitive(obj, c);
    if (item) return item;
  }

  return NULL;
}

static void copy_json_str(char *dst, size_t dst_sz, cJSON *item)
{
  if (!dst || dst_sz == 0U) return;
  dst[0] = '\0';
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(dst, item->valuestring, dst_sz - 1U);
    dst[dst_sz - 1U] = '\0';
  }
}

static uint8_t read_json_bool(cJSON *item, uint8_t *out)
{
  if (!out) return 0U;

  if (cJSON_IsBool(item)) {
    *out = cJSON_IsTrue(item) ? 1U : 0U;
    return 1U;
  }

  if (cJSON_IsNumber(item)) {
    *out = (item->valuedouble != 0.0) ? 1U : 0U;
    return 1U;
  }

  if (cJSON_IsString(item) && item->valuestring) {
    if (strcmp(item->valuestring, "true") == 0 ||
        strcmp(item->valuestring, "1") == 0 ||
        strcmp(item->valuestring, "on") == 0 ||
        strcmp(item->valuestring, "enabled") == 0) {
      *out = 1U;
      return 1U;
    }

    if (strcmp(item->valuestring, "false") == 0 ||
        strcmp(item->valuestring, "0") == 0 ||
        strcmp(item->valuestring, "off") == 0 ||
        strcmp(item->valuestring, "disabled") == 0) {
      *out = 0U;
      return 1U;
    }
  }

  return 0U;
}

static uint8_t str_eq_ci(const char *a, const char *b)
{
  if (!a || !b) return 0U;

  while (*a && *b) {
    char ca = *a++;
    char cb = *b++;

    if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');

    if (ca != cb) return 0U;
  }

  return (*a == '\0' && *b == '\0') ? 1U : 0U;
}

static uint8_t parity_from_str(const char *s)
{
  if (!s) return 0U;
  if (str_eq_ci(s, "even")) return 1U;
  if (str_eq_ci(s, "odd")) return 2U;
  return 0U;
}

static uint8_t func_from_str(const char *s)
{
  if (!s) return 0U;
  if (str_eq_ci(s, "fc_3") || str_eq_ci(s, "fc3") ||
      strcmp(s, "3") == 0 || str_eq_ci(s, "holding") ||
      str_eq_ci(s, "holding_registers")) return 3U;
  if (str_eq_ci(s, "fc_4") || str_eq_ci(s, "fc4") ||
      strcmp(s, "4") == 0 || str_eq_ci(s, "input") ||
      str_eq_ci(s, "input_registers")) return 4U;
  if (str_eq_ci(s, "fc_1") || str_eq_ci(s, "fc1") || strcmp(s, "1") == 0) return 1U;
  if (str_eq_ci(s, "fc_2") || str_eq_ci(s, "fc2") || strcmp(s, "2") == 0) return 2U;
  return 0U;
}

static uint8_t func_from_json(cJSON *item)
{
  if (cJSON_IsString(item) && item->valuestring) {
    return func_from_str(item->valuestring);
  }

  if (cJSON_IsNumber(item)) {
    int fn = (int)item->valuedouble;
    if (fn >= 1 && fn <= 4) return (uint8_t)fn;
  }

  return 0U;
}

static uint8_t qty_from_bits(int bits)
{
  if (bits <= 0) return 0U;
  if ((bits % 16) != 0) return 0U;

  int q = bits / 16;
  if (q == 1 || q == 2 || q == 4) return (uint8_t)q;
  return 0U;
}

static uint8_t qty_from_register_count(int qty)
{
  if (qty == 1 || qty == 2 || qty == 4) return (uint8_t)qty;
  return 0U;
}

static cJSON *get_runtime_config_obj(cJSON *root)
{
  if (!cJSON_IsObject(root)) return NULL;

  cJSON *inner = cJSON_GetObjectItemCaseSensitive(root, "config");
  if (cJSON_IsObject(inner)) return inner;

  return root;
}

static uint8_t point_kind_from_str(const char *s)
{
  if (str_eq_ci(s, "digital")) return POWERLYTICS_POINT_DIGITAL;
  if (str_eq_ci(s, "counter")) return POWERLYTICS_POINT_COUNTER;
  if (str_eq_ci(s, "analog")) return POWERLYTICS_POINT_ANALOG;
  if (str_eq_ci(s, "relay")) return POWERLYTICS_POINT_RELAY;
  if (str_eq_ci(s, "modbus")) return POWERLYTICS_POINT_MODBUS;
  return POWERLYTICS_POINT_DISABLED;
}

static uint8_t modbus_value_kind_from_str(const char *s)
{
  if (str_eq_ci(s, "digital")) return POWERLYTICS_MODBUS_KIND_DIGITAL;
  if (str_eq_ci(s, "counter")) return POWERLYTICS_MODBUS_KIND_COUNTER;
  if (str_eq_ci(s, "enum")) return POWERLYTICS_MODBUS_KIND_ENUM;
  if (str_eq_ci(s, "raw")) return POWERLYTICS_MODBUS_KIND_RAW;
  return POWERLYTICS_MODBUS_KIND_ANALOG;
}

static uint8_t rule_type_from_str(const char *s)
{
  if (str_eq_ci(s, "rising_edge")) return POWERLYTICS_RULE_RISING_EDGE;
  if (str_eq_ci(s, "falling_edge")) return POWERLYTICS_RULE_FALLING_EDGE;
  if (str_eq_ci(s, "threshold_high")) return POWERLYTICS_RULE_THRESHOLD_HIGH;
  if (str_eq_ci(s, "threshold_low")) return POWERLYTICS_RULE_THRESHOLD_LOW;
  if (str_eq_ci(s, "out_of_range")) return POWERLYTICS_RULE_OUT_OF_RANGE;
  if (str_eq_ci(s, "in_range")) return POWERLYTICS_RULE_IN_RANGE;
  return POWERLYTICS_RULE_NONE;
}

static uint8_t severity_from_str(const char *s)
{
  if (str_eq_ci(s, "critical")) return POWERLYTICS_SEVERITY_CRITICAL;
  if (str_eq_ci(s, "warning")) return POWERLYTICS_SEVERITY_WARNING;
  return POWERLYTICS_SEVERITY_INFO;
}

static uint8_t edge_from_str(const char *s)
{
  if (str_eq_ci(s, "falling")) return POWERLYTICS_EDGE_FALLING;
  if (str_eq_ci(s, "both")) return POWERLYTICS_EDGE_BOTH;
  return POWERLYTICS_EDGE_RISING;
}

static uint8_t latch_from_str(const char *s)
{
  if (str_eq_ci(s, "latch_until_normal")) return POWERLYTICS_LATCH_UNTIL_NORMAL;
  if (str_eq_ci(s, "latch_until_reset")) return POWERLYTICS_LATCH_UNTIL_RESET;
  return POWERLYTICS_LATCH_NONE;
}

static uint8_t dtype_from_str(const char *s)
{
  if (str_eq_ci(s, "int16")) return POWERLYTICS_DTYPE_INT16;
  if (str_eq_ci(s, "uint32")) return POWERLYTICS_DTYPE_UINT32;
  if (str_eq_ci(s, "int32")) return POWERLYTICS_DTYPE_INT32;
  if (str_eq_ci(s, "float32")) return POWERLYTICS_DTYPE_FLOAT32;
  if (str_eq_ci(s, "bool")) return POWERLYTICS_DTYPE_BOOL;
  if (str_eq_ci(s, "bit")) return POWERLYTICS_DTYPE_BIT;
  if (str_eq_ci(s, "bitfield")) return POWERLYTICS_DTYPE_BITFIELD;
  if (str_eq_ci(s, "enum")) return POWERLYTICS_DTYPE_ENUM;
  if (str_eq_ci(s, "raw16")) return POWERLYTICS_DTYPE_RAW16;
  return POWERLYTICS_DTYPE_UINT16;
}

static uint8_t endian_from_str(const char *s)
{
  if (str_eq_ci(s, "ABCD")) return POWERLYTICS_ENDIAN_ABCD;
  if (str_eq_ci(s, "BADC")) return POWERLYTICS_ENDIAN_BADC;
  if (str_eq_ci(s, "CDAB")) return POWERLYTICS_ENDIAN_CDAB;
  if (str_eq_ci(s, "DCBA")) return POWERLYTICS_ENDIAN_DCBA;
  return POWERLYTICS_ENDIAN_NONE;
}

static uint32_t clamp_u32(uint32_t value, uint32_t min_value, uint32_t max_value, uint32_t fallback)
{
  if (value < min_value) return fallback;
  if (value > max_value) return max_value;
  return value;
}

static void parse_rules(cJSON *rules, powerlytics_rule_cfg_t out[POWERLYTICS_MAX_RULES_PER_POINT], uint8_t *out_count)
{
  uint8_t count = 0U;
  cJSON *r = NULL;

  if (out_count) *out_count = 0U;
  if (!cJSON_IsArray(rules) || !out || !out_count) return;

  cJSON_ArrayForEach(r, rules) {
    if (!cJSON_IsObject(r) || count >= POWERLYTICS_MAX_RULES_PER_POINT) continue;

    powerlytics_rule_cfg_t *rule = &out[count];
    memset(rule, 0, sizeof(*rule));

    cJSON *type = cJSON_GetObjectItemCaseSensitive(r, "type");
    rule->type = rule_type_from_str(cJSON_IsString(type) ? type->valuestring : NULL);
    if (rule->type == POWERLYTICS_RULE_NONE) continue;

    rule->enabled = 1U;
    (void)read_json_bool(cJSON_GetObjectItemCaseSensitive(r, "enabled"), &rule->enabled);

    cJSON *severity = cJSON_GetObjectItemCaseSensitive(r, "severity");
    rule->severity = severity_from_str(cJSON_IsString(severity) ? severity->valuestring : NULL);

    cJSON *latch = cJSON_GetObjectItemCaseSensitive(r, "latchMode");
    rule->latch_mode = latch_from_str(cJSON_IsString(latch) ? latch->valuestring : NULL);

    /* Optional local action: actPort "RO_n", actRaise 0|1, optional actClear 0|1. */
    cJSON *act = cJSON_GetObjectItemCaseSensitive(r, "actPort");
    if (act != NULL) {
      rule->act = POWERLYTICS_ACT_INVALID; /* until it parses: the config is then refused with a reason */
      cJSON *raise = cJSON_GetObjectItemCaseSensitive(r, "actRaise");
      if (cJSON_IsString(act) && act->valuestring && strncmp(act->valuestring, "RO_", 3U) == 0 &&
          act->valuestring[3] >= '1' && act->valuestring[3] <= '8' && act->valuestring[4] == '\0' &&
          cJSON_IsNumber(raise) && (raise->valuedouble == 0.0 || raise->valuedouble == 1.0)) {
        uint8_t has_clear = 0U, clear_state = 0U;
        cJSON *clear = cJSON_GetObjectItemCaseSensitive(r, "actClear");
        if (cJSON_IsNumber(clear) && (clear->valuedouble == 0.0 || clear->valuedouble == 1.0)) {
          has_clear = 1U;
          clear_state = (uint8_t)clear->valuedouble;
        }
        rule->act = POWERLYTICS_ACT_PACK((uint8_t)(act->valuestring[3] - '0'), (uint8_t)raise->valuedouble, has_clear, clear_state);
      }
    }

    cJSON *num = cJSON_GetObjectItemCaseSensitive(r, "debounceMs");
    if (cJSON_IsNumber(num) && num->valuedouble > 0.0) rule->debounce_ms = (uint32_t)num->valuedouble;

    num = cJSON_GetObjectItemCaseSensitive(r, "durationMs");
    if (cJSON_IsNumber(num) && num->valuedouble > 0.0) rule->duration_ms = (uint32_t)num->valuedouble;

    num = cJSON_GetObjectItemCaseSensitive(r, "recoveryDurationMs");
    if (cJSON_IsNumber(num) && num->valuedouble > 0.0) rule->recovery_duration_ms = (uint32_t)num->valuedouble;

    num = cJSON_GetObjectItemCaseSensitive(r, "high");
    if (cJSON_IsNumber(num)) rule->high = (float)num->valuedouble;

    num = cJSON_GetObjectItemCaseSensitive(r, "low");
    if (cJSON_IsNumber(num)) rule->low = (float)num->valuedouble;

    num = cJSON_GetObjectItemCaseSensitive(r, "hysteresis");
    if (cJSON_IsNumber(num)) rule->hysteresis = (float)num->valuedouble;

    cJSON *ctx = cJSON_GetObjectItemCaseSensitive(r, "contextKeys");
    cJSON *key = NULL;
    if (cJSON_IsArray(ctx)) {
      cJSON_ArrayForEach(key, ctx) {
        if (rule->context_count >= POWERLYTICS_MAX_CONTEXT_KEYS) break;
        if (!cJSON_IsString(key) || !key->valuestring) continue;
        strncpy(rule->context_keys[rule->context_count], key->valuestring, POWERLYTICS_CONTEXT_KEY_LEN - 1U);
        rule->context_count++;
      }
    }

    count++;
  }

  *out_count = count;
}

static void parse_digital_cfg(cJSON *digital, powerlytics_point_cfg_t *point)
{
  if (!point) return;

  point->edge = POWERLYTICS_EDGE_BOTH;
  point->active_high = 1U;
  point->counter_width = 32U;
  point->rollover_at = 0xFFFFFFFFUL;
  point->debounce_ms = POWERLYTICS_DEFAULT_DI_DEBOUNCE_MS;

  if (!cJSON_IsObject(digital)) return;

  cJSON *edge = cJSON_GetObjectItemCaseSensitive(digital, "edge");
  point->edge = edge_from_str(cJSON_IsString(edge) ? edge->valuestring : NULL);
  (void)read_json_bool(cJSON_GetObjectItemCaseSensitive(digital, "activeHigh"), &point->active_high);

  cJSON *debounce = cJSON_GetObjectItemCaseSensitive(digital, "debounceMs");
  if (cJSON_IsNumber(debounce) && debounce->valuedouble >= 0.0 && debounce->valuedouble <= 10000.0 &&
      debounce->valuedouble == (double)(uint32_t)debounce->valuedouble) {
    point->debounce_ms = (uint32_t)debounce->valuedouble;
  }

  cJSON *width = cJSON_GetObjectItemCaseSensitive(digital, "counterWidth");
  if (cJSON_IsNumber(width)) {
    int w = (int)width->valuedouble;
    point->counter_width = (w == 16) ? 16U : 32U;
  }

  cJSON *roll = cJSON_GetObjectItemCaseSensitive(digital, "rolloverAt");
  if (cJSON_IsNumber(roll) && roll->valuedouble > 0.0) {
    point->rollover_at = (uint32_t)roll->valuedouble;
  } else if (point->counter_width == 16U) {
    point->rollover_at = 0xFFFFUL;
  }
}

static void parse_point_common(cJSON *p, powerlytics_point_cfg_t *point)
{
  if (!point) return;

  memset(point, 0, sizeof(*point));
  point->enabled = 1U;
  point->scale = 1.0f;
  point->offset = 0.0f;

  copy_json_str(point->key, sizeof(point->key), cJSON_GetObjectItemCaseSensitive(p, "key"));
  copy_json_str(point->name, sizeof(point->name), cJSON_GetObjectItemCaseSensitive(p, "name"));
  copy_json_str(point->unit, sizeof(point->unit), cJSON_GetObjectItemCaseSensitive(p, "unit"));

  cJSON *kind = cJSON_GetObjectItemCaseSensitive(p, "kind");
  point->kind = point_kind_from_str(cJSON_IsString(kind) ? kind->valuestring : NULL);
  (void)read_json_bool(cJSON_GetObjectItemCaseSensitive(p, "enabled"), &point->enabled);

  cJSON *num = cJSON_GetObjectItemCaseSensitive(p, "scale");
  if (cJSON_IsNumber(num)) point->scale = (float)num->valuedouble;

  num = cJSON_GetObjectItemCaseSensitive(p, "offset");
  if (cJSON_IsNumber(num)) point->offset = (float)num->valuedouble;

  parse_digital_cfg(cJSON_GetObjectItemCaseSensitive(p, "digital"), point);
  parse_rules(cJSON_GetObjectItemCaseSensitive(p, "rules"), point->rules, &point->rule_count);
}

static void parse_read_common(cJSON *r, mb_read_item_t *rc)
{
  if (!rc) return;

  memset(rc, 0, sizeof(*rc));
  rc->scale = 1.0f;
  rc->offset = 0.0f;
  rc->kind = POWERLYTICS_MODBUS_KIND_ANALOG;
  rc->data_type = POWERLYTICS_DTYPE_UINT16;
  rc->endianness = POWERLYTICS_ENDIAN_NONE;

  copy_json_str(rc->read_id, sizeof(rc->read_id), json_get_any(r, "readId", "read_id", "id"));
  copy_json_str(rc->name, sizeof(rc->name), cJSON_GetObjectItemCaseSensitive(r, "name"));
  copy_json_str(rc->tag, sizeof(rc->tag), cJSON_GetObjectItemCaseSensitive(r, "tag"));
  copy_json_str(rc->unit, sizeof(rc->unit), cJSON_GetObjectItemCaseSensitive(r, "unit"));

  cJSON *kind = cJSON_GetObjectItemCaseSensitive(r, "kind");
  rc->kind = modbus_value_kind_from_str(cJSON_IsString(kind) ? kind->valuestring : NULL);

  cJSON *func = json_get_any(r, "func", "function", "functionCode");
  rc->func = func_from_json(func);

  cJSON *start = json_get_any(r, "start", "address", "startAddress");
  if (cJSON_IsNumber(start) && start->valuedouble >= 0.0) {
    rc->start_reg = (uint16_t)start->valuedouble;
  }

  cJSON *bits = json_get_any(r, "bits", "bitWidth", "registerBits");
  if (cJSON_IsNumber(bits)) rc->qty = qty_from_bits((int)bits->valuedouble);

  if (rc->qty == 0U) {
    cJSON *qty = json_get_any(r, "qty", "quantity", "registerCount");
    if (cJSON_IsNumber(qty)) rc->qty = qty_from_register_count((int)qty->valuedouble);
  }

  cJSON *dtype = cJSON_GetObjectItemCaseSensitive(r, "dataType");
  rc->data_type = dtype_from_str(cJSON_IsString(dtype) ? dtype->valuestring : NULL);

  cJSON *endian = cJSON_GetObjectItemCaseSensitive(r, "endianness");
  rc->endianness = endian_from_str(cJSON_IsString(endian) ? endian->valuestring : NULL);

  cJSON *bit = cJSON_GetObjectItemCaseSensitive(r, "bitIndex");
  if (cJSON_IsNumber(bit) && bit->valuedouble >= 0.0 && bit->valuedouble < 32.0) {
    rc->bit_index = (uint8_t)bit->valuedouble;
  }

  cJSON *num = cJSON_GetObjectItemCaseSensitive(r, "scale");
  if (cJSON_IsNumber(num)) rc->scale = (float)num->valuedouble;

  num = cJSON_GetObjectItemCaseSensitive(r, "offset");
  if (cJSON_IsNumber(num)) rc->offset = (float)num->valuedouble;

  parse_rules(cJSON_GetObjectItemCaseSensitive(r, "rules"), rc->rules, &rc->rule_count);
}

static uint8_t parse_slave(cJSON *s, mb_slave_cfg_t *sc)
{
  if (!cJSON_IsObject(s) || !sc) return 0U;

  memset(sc, 0, sizeof(*sc));
  sc->baud = MODBUS_DEFAULT_SLAVE_BAUD;
  sc->data_bits = MODBUS_DEFAULT_SLAVE_DATA_BITS;
  sc->stop_bits = MODBUS_DEFAULT_SLAVE_STOP_BITS;
  sc->parity = MODBUS_DEFAULT_SLAVE_PARITY;
  sc->interval_ms = MODBUS_DEFAULT_SLAVE_INTERVAL_MS;
  sc->timeout_ms = MODBUS_DEFAULT_SLAVE_TIMEOUT_MS;
  sc->retries = MODBUS_DEFAULT_SLAVE_RETRIES;

  copy_json_str(sc->unique_slave_id, sizeof(sc->unique_slave_id),
                json_get_any(s, "unique_slave_id", "uniqueSlaveId", "uniqueSlaveID"));
  copy_json_str(sc->name, sizeof(sc->name), cJSON_GetObjectItemCaseSensitive(s, "name"));

  cJSON *sid = json_get_any(s, "slave_id", "slaveId", "address");
  if (cJSON_IsString(sid) && sid->valuestring) {
    int parsed = atoi(sid->valuestring);
    if (parsed > 0 && parsed <= 247) sc->slave_id = (uint8_t)parsed;
  } else if (cJSON_IsNumber(sid) && sid->valuedouble > 0.0 && sid->valuedouble <= 247.0) {
    sc->slave_id = (uint8_t)sid->valuedouble;
  }

  cJSON *serial = cJSON_GetObjectItemCaseSensitive(s, "serial");
  if (cJSON_IsObject(serial)) {
    cJSON *baud = json_get_any(serial, "baudRate", "baud", "baud_rate");
    cJSON *db   = cJSON_GetObjectItemCaseSensitive(serial, "dataBits");
    cJSON *sb   = cJSON_GetObjectItemCaseSensitive(serial, "stopBits");
    cJSON *par  = cJSON_GetObjectItemCaseSensitive(serial, "parity");

    if (cJSON_IsNumber(baud) && baud->valuedouble > 0.0) sc->baud = (uint32_t)baud->valuedouble;
    if (cJSON_IsNumber(db) && db->valuedouble > 0.0) sc->data_bits = (uint8_t)db->valuedouble;
    if (cJSON_IsNumber(sb)) sc->stop_bits = ((int)sb->valuedouble == 2) ? 2U : 1U;
    if (cJSON_IsString(par) && par->valuestring) sc->parity = parity_from_str(par->valuestring);
  }

  cJSON *polling = cJSON_GetObjectItemCaseSensitive(s, "polling");
  if (cJSON_IsObject(polling)) {
    cJSON *ival = json_get_any(polling, "intervalMs", "interval", "pollIntervalMs");
    cJSON *tout = json_get_any(polling, "timeoutMs", "timeout", "timeout_ms");
    cJSON *ret  = cJSON_GetObjectItemCaseSensitive(polling, "retries");

    if (cJSON_IsNumber(ival) && ival->valuedouble > 0.0) sc->interval_ms = (uint32_t)ival->valuedouble;
    if (cJSON_IsNumber(tout) && tout->valuedouble > 0.0) sc->timeout_ms = (uint32_t)tout->valuedouble;
    if (cJSON_IsNumber(ret) && ret->valuedouble >= 0.0) sc->retries = (uint8_t)ret->valuedouble;
  }

  cJSON *regs = cJSON_GetObjectItemCaseSensitive(s, "reads");
  if (!cJSON_IsArray(regs)) regs = cJSON_GetObjectItemCaseSensitive(s, "registers");
  if (!cJSON_IsArray(regs)) return 0U;

  uint8_t ridx = 0U;
  cJSON *r = NULL;
  cJSON_ArrayForEach(r, regs) {
    if (!cJSON_IsObject(r) || ridx >= MODBUS_MAX_READS_PER_SLAVE) continue;

    mb_read_item_t tmp;
    parse_read_common(r, &tmp);

    if (tmp.read_id[0] == '\0') continue;
    if (!(tmp.func == 1U || tmp.func == 2U || tmp.func == 3U || tmp.func == 4U)) continue;
    if (tmp.qty == 0U && !(tmp.data_type == POWERLYTICS_DTYPE_BOOL || tmp.data_type == POWERLYTICS_DTYPE_BIT)) continue;
    if (tmp.qty == 0U) tmp.qty = 1U;

    sc->reads[ridx++] = tmp;
  }

  if (sc->slave_id == 0U || ridx == 0U) return 0U;

  if (sc->unique_slave_id[0] == '\0') {
    snprintf(sc->unique_slave_id, sizeof(sc->unique_slave_id), "%u", sc->slave_id);
  }

  sc->read_count = ridx;
  return 1U;
}

static void parse_slaves_array(cJSON *slaves, modbus_cfg_t *cfg)
{
  if (!cJSON_IsArray(slaves) || !cfg) return;

  cJSON *s = NULL;
  cJSON_ArrayForEach(s, slaves) {
    if (cfg->slave_count >= MODBUS_MAX_SLAVES) break;

    mb_slave_cfg_t parsed;
    if (parse_slave(s, &parsed)) {
      cfg->slaves[cfg->slave_count++] = parsed;
    }
  }
}

static void parse_points_array(cJSON *points, modbus_cfg_t *cfg)
{
  if (!cJSON_IsArray(points) || !cfg) return;

  cJSON *p = NULL;
  cJSON_ArrayForEach(p, points) {
    if (!cJSON_IsObject(p)) continue;

    powerlytics_point_cfg_t point;
    parse_point_common(p, &point);
    if (point.key[0] == '\0' || point.kind == POWERLYTICS_POINT_DISABLED) continue;

    if (point.kind == POWERLYTICS_POINT_MODBUS) {
      parse_slaves_array(cJSON_GetObjectItemCaseSensitive(p, "slaves"), cfg);
    }

    if (cfg->point_count < POWERLYTICS_MAX_POINTS) {
      cfg->points[cfg->point_count++] = point;
    }
  }
}

/* Why the last parse refused the config, sent back to the platform in the
 * error ack. Before 2026-11-17 over-limit values were cut or clamped and
 * the config was reported APPLIED anyway. */
static const char *s_parse_error = "parse_to_struct_failed";

const char *modbus_cfg_parse_error(void)
{
  return s_parse_error;
}

static uint8_t kind_known(const char *k)
{
  return (k && (str_eq_ci(k, "digital") || str_eq_ci(k, "counter") || str_eq_ci(k, "analog") ||
                str_eq_ci(k, "relay") || str_eq_ci(k, "modbus"))) ? 1U : 0U;
}

/* Refuses anything the device would otherwise have to drop or change. */
static const char *check_limits(cJSON *cfg_json)
{
  cJSON *points = cJSON_GetObjectItemCaseSensitive(cfg_json, "points");
  if (!cJSON_IsArray(points)) return NULL;

  cJSON *ti = cJSON_GetObjectItemCaseSensitive(cfg_json, "telemetryInterval");
  if (cJSON_IsNumber(ti) &&
      (ti->valuedouble < (double)MODBUS_MIN_TELEMETRY_INTERVAL_MS ||
       ti->valuedouble > (double)MODBUS_MAX_TELEMETRY_INTERVAL_MS)) {
    return "interval_out_of_range";
  }

  uint32_t enabled = 0U, slaves = 0U;
  cJSON *p = NULL;
  cJSON_ArrayForEach(p, points) {
    if (!cJSON_IsObject(p)) return "invalid_point";
    cJSON *kind = cJSON_GetObjectItemCaseSensitive(p, "kind");
    if (!cJSON_IsString(kind) || !kind_known(kind->valuestring)) return "unknown_point_kind";
    cJSON *en = cJSON_GetObjectItemCaseSensitive(p, "enabled");
    if (!cJSON_IsFalse(en)) enabled++;
    cJSON *rules = cJSON_GetObjectItemCaseSensitive(p, "rules");
    if (cJSON_IsArray(rules) && cJSON_GetArraySize(rules) > (int)POWERLYTICS_MAX_RULES_PER_POINT) return "too_many_rules";
    cJSON *sl = cJSON_GetObjectItemCaseSensitive(p, "slaves");
    if (cJSON_IsArray(sl) && !cJSON_IsFalse(en)) {
      cJSON *one = NULL;
      cJSON_ArrayForEach(one, sl) {
        slaves++;
        cJSON *reads = cJSON_GetObjectItemCaseSensitive(one, "reads");
        if (!cJSON_IsArray(reads)) reads = cJSON_GetObjectItemCaseSensitive(one, "registers");
        if (cJSON_IsArray(reads) && cJSON_GetArraySize(reads) > (int)MODBUS_MAX_READS_PER_SLAVE) return "too_many_reads";
      }
    }
  }
  if (enabled > POWERLYTICS_MAX_POINTS) return "too_many_points";
  cJSON *ll = cJSON_GetObjectItemCaseSensitive(cfg_json, "relayLinkLossSafeMs");
  if (ll && (!cJSON_IsNumber(ll) || ll->valuedouble < 0.0 || ll->valuedouble > 255.0 * 60000.0)) return "invalid_link_loss_timeout";
  if (slaves > MODBUS_MAX_SLAVES) return "too_many_slaves";
  return NULL;
}

static bool modbus_cfg_parse_root(cJSON *root, modbus_cfg_t *cfg)
{
  s_parse_error = "parse_to_struct_failed";
  if (!root || !cfg) return false;

  modbus_cfg_defaults(cfg);

  cJSON *cfg_json = get_runtime_config_obj(root);
  if (!cfg_json) {
    return false;
  }

  if (cJSON_GetObjectItemCaseSensitive(cfg_json, "modbusSlaves") != NULL) {
    return false;
  }

  const char *limit_error = check_limits(cfg_json);
  if (limit_error) {
    s_parse_error = limit_error;
    return false;
  }

  cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "v");
  if (!cJSON_IsNumber(version)) version = cJSON_GetObjectItemCaseSensitive(cfg_json, "v");
  cfg->protocol_version = cJSON_IsNumber(version) ? (uint8_t)version->valuedouble : 1U;
  if (cfg->protocol_version == 0U) cfg->protocol_version = 1U;

  copy_json_str(cfg->device_id, sizeof(cfg->device_id), cJSON_GetObjectItemCaseSensitive(cfg_json, "device_id"));
  copy_json_str(cfg->config_id, sizeof(cfg->config_id), cJSON_GetObjectItemCaseSensitive(cfg_json, "configId"));
  copy_json_str(cfg->imei, sizeof(cfg->imei), cJSON_GetObjectItemCaseSensitive(cfg_json, "imei"));
  copy_json_str(cfg->deployment_id, sizeof(cfg->deployment_id), cJSON_GetObjectItemCaseSensitive(cfg_json, "deploymentId"));
  copy_json_str(cfg->api_key, sizeof(cfg->api_key), cJSON_GetObjectItemCaseSensitive(cfg_json, "apiKey"));

  cJSON *telemetry_interval = cJSON_GetObjectItemCaseSensitive(cfg_json, "telemetryInterval");
  if (cJSON_IsNumber(telemetry_interval)) {
    uint32_t raw = (uint32_t)telemetry_interval->valuedouble;
    cJSON *points = cJSON_GetObjectItemCaseSensitive(cfg_json, "points");
    if (!cJSON_IsArray(points) && raw > 0U && raw < 1000U) {
      raw *= 1000UL;
    }
    cfg->telemetry_interval_ms = clamp_u32(raw,
                                           MODBUS_MIN_TELEMETRY_INTERVAL_MS,
                                           MODBUS_MAX_TELEMETRY_INTERVAL_MS,
                                           MODBUS_DEFAULT_TELEMETRY_INTERVAL_MS);
  }

  cJSON *telemetry = cJSON_GetObjectItemCaseSensitive(cfg_json, "telemetry");
  if (!cJSON_IsObject(telemetry)) return false;

  cJSON *sample = cJSON_GetObjectItemCaseSensitive(telemetry, "sampleIntervalMs");
  cJSON *snapshot = cJSON_GetObjectItemCaseSensitive(telemetry, "sendFullSnapshot");
  cJSON *cap = cJSON_GetObjectItemCaseSensitive(telemetry, "eventCapPerInterval");

  if (!cJSON_IsNumber(sample) ||
      sample->valuedouble < POWERLYTICS_MIN_SAMPLE_INTERVAL_MS ||
      sample->valuedouble > POWERLYTICS_MAX_SAMPLE_INTERVAL_MS ||
      sample->valuedouble != (double)(uint32_t)sample->valuedouble ||
      !cJSON_IsTrue(snapshot) ||
      !cJSON_IsNumber(cap) ||
      cap->valuedouble < 1.0 || cap->valuedouble > POWERLYTICS_MAX_EVENT_CAP ||
      cap->valuedouble != (double)(uint32_t)cap->valuedouble) {
    s_parse_error = "invalid_telemetry_settings";
    return false;
  }

  cfg->sample_interval_ms = (uint32_t)sample->valuedouble;
  cfg->send_full_snapshot = 1U;
  cfg->event_cap_per_interval = (uint8_t)cap->valuedouble;

  if (cfg->device_id[0] == '\0' || cfg->config_id[0] == '\0' ||
      cfg->deployment_id[0] == '\0' || cfg->api_key[0] == '\0') {
    s_parse_error = "missing_identity_fields";
    return false;
  }

  cJSON *points = cJSON_GetObjectItemCaseSensitive(cfg_json, "points");
  parse_points_array(points, cfg);

  /* A rule that cannot behave sensibly (band with low >= high, hysteresis
   * wider than the band, negative hysteresis) is refused with its reason
   * instead of running an alarm that never clears. */
  for (uint8_t i = 0U; i < cfg->point_count && i < POWERLYTICS_MAX_POINTS; i++) {
    for (uint8_t r = 0U; r < cfg->points[i].rule_count && r < POWERLYTICS_MAX_RULES_PER_POINT; r++) {
      const char *why = NULL;
      if (!app_rule_cfg_valid(&cfg->points[i].rules[r], &why)) {
        s_parse_error = why;
        return false;
      }
    }
  }
  for (uint8_t si = 0U; si < cfg->slave_count && si < MODBUS_MAX_SLAVES; si++) {
    for (uint8_t ri = 0U; ri < cfg->slaves[si].read_count && ri < MODBUS_MAX_READS_PER_SLAVE; ri++) {
      const mb_read_item_t *rc = &cfg->slaves[si].reads[ri];
      for (uint8_t r = 0U; r < rc->rule_count && r < POWERLYTICS_MAX_RULES_PER_POINT; r++) {
        const char *why = NULL;
        if (!app_rule_cfg_valid(&rc->rules[r], &why)) {
          s_parse_error = why;
          return false;
        }
      }
    }
  }

  /* Relay safe states: applied at boot and, when relayLinkLossSafeMs is set,
   * after the connection has been lost that long. Default OFF / hold. */
  uint16_t flags = 0U;
  cJSON *p = NULL;
  cJSON_ArrayForEach(p, points) {
    cJSON *kind = cJSON_GetObjectItemCaseSensitive(p, "kind");
    cJSON *key = cJSON_GetObjectItemCaseSensitive(p, "key");
    cJSON *safe = cJSON_GetObjectItemCaseSensitive(p, "safeState");
    if (!cJSON_IsString(kind) || !str_eq_ci(kind->valuestring, "relay") || !cJSON_IsString(key) || !cJSON_IsString(safe)) continue;
    if (strncmp(key->valuestring, "RO_", 3) != 0) continue;
    int n = atoi(key->valuestring + 3);
    if (n >= 1 && n <= 8 && str_eq_ci(safe->valuestring, "ON")) flags |= (uint16_t)(1U << (n - 1));
  }
  cJSON *ll = cJSON_GetObjectItemCaseSensitive(cfg_json, "relayLinkLossSafeMs");
  if (cJSON_IsNumber(ll) && ll->valuedouble > 0.0) {
    uint32_t minutes = (uint32_t)((ll->valuedouble + 59999.0) / 60000.0);
    if (minutes > 255U) minutes = 255U;
    flags |= (uint16_t)(minutes << 8);
  }
  cfg->reserved_flags = flags;

  return true;
}

bool modbus_cfg_from_cjson(cJSON *root, modbus_cfg_t *cfg)
{
  return modbus_cfg_parse_root(root, cfg);
}

bool modbus_cfg_from_json(const char *json, modbus_cfg_t *cfg)
{
  if (!json || !cfg) return false;

  cJSON *root = cJSON_Parse(json);
  if (!root) return false;

  bool ok = modbus_cfg_parse_root(root, cfg);
  cJSON_Delete(root);
  return ok;
}
