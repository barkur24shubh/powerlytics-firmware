//#include "modbus_config_json.h"
//#include "cJSON.h"
//#include <string.h>
//#include <stdint.h>
//
//static uint8_t parity_from_str(const char *s)
//{
//  if (!s) return 0;
//  if (strcmp(s, "even") == 0) return 1;
//  if (strcmp(s, "odd")  == 0) return 2;
//  return 0; // "none" or unknown
//}
//
//static uint8_t func_from_str(const char *s)
//{
//  if (!s) return 0;
//  if (strcmp(s, "fc_3") == 0) return 3;
//  if (strcmp(s, "fc_4") == 0) return 4;
//  return 0;
//}
//
//static uint8_t qty_from_bits(int bits)
//{
//  // bits are 16/32/64 -> qty 1/2/4 registers
//  if (bits <= 0) return 0;
//  if ((bits % 16) != 0) return 0;
//  int q = bits / 16;
//  if (q == 1 || q == 2 || q == 4) return (uint8_t)q;
//  return 0;
//}
//
//static void copy_json_str(char *dst, size_t dst_sz, cJSON *item)
//{
//  if (!dst || dst_sz == 0) return;
//  dst[0] = '\0';
//  if (cJSON_IsString(item) && item->valuestring) {
//    strncpy(dst, item->valuestring, dst_sz - 1);
//    dst[dst_sz - 1] = '\0';
//  }
//}
//
//bool modbus_cfg_from_json(const char *json, modbus_cfg_t *cfg)
//{
//  if (!json || !cfg) return false;
//
//  // start with defaults so missing fields won’t break you
//  modbus_cfg_defaults(cfg);
//
//  cJSON *root = cJSON_Parse(json);
//  if (!root) return false;
//
//  // root identifiers
//  copy_json_str(cfg->device_id, sizeof(cfg->device_id),
//                cJSON_GetObjectItemCaseSensitive(root, "device_id"));
//  copy_json_str(cfg->config_id, sizeof(cfg->config_id),
//                cJSON_GetObjectItemCaseSensitive(root, "configId"));
//  copy_json_str(cfg->imei, sizeof(cfg->imei),
//                cJSON_GetObjectItemCaseSensitive(root, "imei"));
//
//  // modbusSlaves[]
//  cJSON *slaves = cJSON_GetObjectItemCaseSensitive(root, "modbusSlaves");
//  if (!cJSON_IsArray(slaves)) {
//    cJSON_Delete(root);
//    return false;
//  }
//
//  uint8_t sidx = 0;
//
//  cJSON *s = NULL;
//  cJSON_ArrayForEach(s, slaves) {
//    if (!cJSON_IsObject(s)) continue;
//    if (sidx >= MODBUS_MAX_SLAVES) break;
//
//    mb_slave_cfg_t *sc = &cfg->slaves[sidx];
//    memset(sc, 0, sizeof(*sc));
//
//    // unique_slave_id (UUID string)
//    copy_json_str(sc->unique_slave_id, sizeof(sc->unique_slave_id),
//                  cJSON_GetObjectItemCaseSensitive(s, "unique_slave_id"));
//
//    // numeric slave_id
//    cJSON *sid = cJSON_GetObjectItemCaseSensitive(s, "slave_id");
//    if (cJSON_IsNumber(sid) && sid->valuedouble >= 0) {
//      sc->slave_id = (uint8_t)sid->valuedouble;
//    }
//
//    // serial
//    cJSON *serial = cJSON_GetObjectItemCaseSensitive(s, "serial");
//    if (cJSON_IsObject(serial)) {
//      cJSON *baud = cJSON_GetObjectItemCaseSensitive(serial, "baudRate");
//      cJSON *db   = cJSON_GetObjectItemCaseSensitive(serial, "dataBits");
//      cJSON *sb   = cJSON_GetObjectItemCaseSensitive(serial, "stopBits");
//      cJSON *par  = cJSON_GetObjectItemCaseSensitive(serial, "parity");
//
//      if (cJSON_IsNumber(baud) && baud->valuedouble > 0) sc->baud      = (uint32_t)baud->valuedouble;
//      if (cJSON_IsNumber(db)   && db->valuedouble > 0)   sc->data_bits = (uint8_t)db->valuedouble;
//      if (cJSON_IsNumber(sb))                             sc->stop_bits = ((int)sb->valuedouble == 2) ? 2 : 1;
//      if (cJSON_IsString(par) && par->valuestring)       sc->parity    = parity_from_str(par->valuestring);
//    }
//
//    // polling
//    cJSON *polling = cJSON_GetObjectItemCaseSensitive(s, "polling");
//    if (cJSON_IsObject(polling)) {
//      cJSON *ival = cJSON_GetObjectItemCaseSensitive(polling, "intervalMs");
//      cJSON *tout = cJSON_GetObjectItemCaseSensitive(polling, "timeoutMs");
//      cJSON *ret  = cJSON_GetObjectItemCaseSensitive(polling, "retries");
//
//      if (cJSON_IsNumber(ival) && ival->valuedouble >= 0) sc->interval_ms = (uint32_t)ival->valuedouble;
//      if (cJSON_IsNumber(tout) && tout->valuedouble >= 0) sc->timeout_ms  = (uint32_t)tout->valuedouble;
//      if (cJSON_IsNumber(ret)  && ret->valuedouble >= 0)  sc->retries     = (uint8_t)ret->valuedouble;
//    }
//
//    // registers[]
//    cJSON *regs = cJSON_GetObjectItemCaseSensitive(s, "registers");
//    if (!cJSON_IsArray(regs)) {
//      // no registers => skip this slave
//      continue;
//    }
//
//    uint8_t ridx = 0;
//
//    cJSON *r = NULL;
//    cJSON_ArrayForEach(r, regs) {
//      if (!cJSON_IsObject(r)) continue;
//      if (ridx >= MODBUS_MAX_READS_PER_SLAVE) break;
//
//      mb_read_item_t *rc = &sc->reads[ridx];
//      memset(rc, 0, sizeof(*rc));
//
//      copy_json_str(rc->read_id, sizeof(rc->read_id),
//                    cJSON_GetObjectItemCaseSensitive(r, "readId"));
//
//      cJSON *func = cJSON_GetObjectItemCaseSensitive(r, "func");
//      if (cJSON_IsString(func) && func->valuestring) {
//        rc->func = func_from_str(func->valuestring);
//      }
//
//      cJSON *start = cJSON_GetObjectItemCaseSensitive(r, "start");
//      if (cJSON_IsNumber(start) && start->valuedouble >= 0) {
//        rc->start_reg = (uint16_t)start->valuedouble;
//      }
//
//      cJSON *bits = cJSON_GetObjectItemCaseSensitive(r, "bits");
//      if (cJSON_IsNumber(bits)) {
//        rc->qty = qty_from_bits((int)bits->valuedouble); // 1/2/4
//      }
//
//      // minimal validation: must have readId, func (3/4), qty (1/2/4)
//      if (rc->read_id[0] == '\0') continue;
//      if (!(rc->func == 3 || rc->func == 4)) continue;
//      if (!(rc->qty == 1 || rc->qty == 2 || rc->qty == 4)) continue;
//
//      ridx++;
//    }
//
//    // Accept slave only if it has required ids + at least 1 read
//    if (sc->unique_slave_id[0] == '\0') continue;
//    if (sc->slave_id == 0) continue; // keep strict for now
//    if (ridx == 0) continue;
//
//    sc->read_count = ridx;
//    sidx++;
//  }
//
//  cfg->slave_count = sidx;
//
//  cJSON_Delete(root);
//  return (cfg->slave_count > 0);
//}


#include "modbus_config_json.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

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

static uint8_t parity_from_str(const char *s)
{
  if (!s) return 0;
  if (strcmp(s, "even") == 0 || strcmp(s, "EVEN") == 0) return 1;
  if (strcmp(s, "odd")  == 0 || strcmp(s, "ODD") == 0) return 2;
  return 0; // "none" or unknown
}

static uint8_t func_from_str(const char *s)
{
  if (!s) return 0;
  if (strcmp(s, "fc_3") == 0 || strcmp(s, "fc3") == 0 ||
      strcmp(s, "3") == 0 || strcmp(s, "holding") == 0 ||
      strcmp(s, "holding_registers") == 0) return 3;
  if (strcmp(s, "fc_4") == 0 || strcmp(s, "fc4") == 0 ||
      strcmp(s, "4") == 0 || strcmp(s, "input") == 0 ||
      strcmp(s, "input_registers") == 0) return 4;
  return 0;
}

static uint8_t func_from_json(cJSON *item)
{
  if (cJSON_IsString(item) && item->valuestring) {
    return func_from_str(item->valuestring);
  }

  if (cJSON_IsNumber(item)) {
    int fn = (int)item->valuedouble;
    if (fn == 3 || fn == 4) {
      return (uint8_t)fn;
    }
  }

  return 0;
}

static uint8_t qty_from_bits(int bits)
{
  // bits are 16/32/64 -> qty 1/2/4 registers
  if (bits <= 0) return 0;
  if ((bits % 16) != 0) return 0;
  int q = bits / 16;
  if (q == 1 || q == 2 || q == 4) return (uint8_t)q;
  return 0;
}

static uint8_t qty_from_register_count(int qty)
{
  if (qty == 1 || qty == 2 || qty == 4) {
    return (uint8_t)qty;
  }

  return 0;
}

static void copy_json_str(char *dst, size_t dst_sz, cJSON *item)
{
  if (!dst || dst_sz == 0) return;
  dst[0] = '\0';
  if (cJSON_IsString(item) && item->valuestring) {
    strncpy(dst, item->valuestring, dst_sz - 1);
    dst[dst_sz - 1] = '\0';
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

/*
 * New backend config can arrive in wrapped format:
 *
 * {
 *   "type": "config",
 *   "configId": "...",
 *   "config": {
 *     "device_id": "...",
 *     "configId": "...",
 *     "imei": "...",
 *     "modbusSlaves": [],
 *     "deploymentId": "...",
 *     "hash": "...",
 *     "apiKey": "..."
 *   }
 * }
 *
 * Older/direct format:
 *
 * {
 *   "device_id": "...",
 *   "configId": "...",
 *   "imei": "...",
 *   "modbusSlaves": []
 * }
 *
 * This helper returns root["config"] if present,
 * otherwise returns root itself.
 */
static cJSON *get_runtime_config_obj(cJSON *root)
{
  if (!cJSON_IsObject(root)) {
    return NULL;
  }

  cJSON *inner = cJSON_GetObjectItemCaseSensitive(root, "config");
  if (cJSON_IsObject(inner)) {
    return inner;
  }

  return root;
}

bool modbus_cfg_from_json(const char *json, modbus_cfg_t *cfg)
{
  if (!json || !cfg) return false;

  // start with defaults so missing fields won’t break you
  modbus_cfg_defaults(cfg);

  cJSON *root = cJSON_Parse(json);
  if (!root) return false;

  /*
   * Use inner "config" object if the downlink is wrapped.
   * Otherwise use root directly for backward compatibility.
   */
  cJSON *cfg_json = get_runtime_config_obj(root);
  if (!cfg_json) {
    cJSON_Delete(root);
    return false;
  }

  // root/config identifiers
  copy_json_str(cfg->device_id, sizeof(cfg->device_id),
                cJSON_GetObjectItemCaseSensitive(cfg_json, "device_id"));

  copy_json_str(cfg->config_id, sizeof(cfg->config_id),
                cJSON_GetObjectItemCaseSensitive(cfg_json, "configId"));

  copy_json_str(cfg->imei, sizeof(cfg->imei),
                cJSON_GetObjectItemCaseSensitive(cfg_json, "imei"));

  /*
   * New fields from latest config.
   *
   * JSON field      C struct field
   * deploymentId -> cfg->deployment_id
   * hash         -> cfg->hash
   * apiKey       -> cfg->api_key
   */
  copy_json_str(cfg->deployment_id, sizeof(cfg->deployment_id),
                cJSON_GetObjectItemCaseSensitive(cfg_json, "deploymentId"));

  copy_json_str(cfg->hash, sizeof(cfg->hash),
                cJSON_GetObjectItemCaseSensitive(cfg_json, "hash"));

  copy_json_str(cfg->api_key, sizeof(cfg->api_key),
                cJSON_GetObjectItemCaseSensitive(cfg_json, "apiKey"));

  uint8_t debug_print = modbus_cfg_default_debug_print_enabled();
  cJSON *debug_item = cJSON_GetObjectItemCaseSensitive(cfg_json, "debugPrint");
  if (!read_json_bool(debug_item, &debug_print)) {
    debug_item = cJSON_GetObjectItemCaseSensitive(cfg_json, "debugLogs");
    if (!read_json_bool(debug_item, &debug_print)) {
      cJSON *debug_obj = cJSON_GetObjectItemCaseSensitive(cfg_json, "debug");
      if (cJSON_IsObject(debug_obj)) {
        debug_item = cJSON_GetObjectItemCaseSensitive(debug_obj, "print");
        (void)read_json_bool(debug_item, &debug_print);
      }
    }
  }
  modbus_cfg_set_debug_print_enabled(cfg, debug_print);

  cJSON *telemetry_interval = cJSON_GetObjectItemCaseSensitive(cfg_json, "telemetryInterval");
  if (cJSON_IsNumber(telemetry_interval)) {
    double interval_s = telemetry_interval->valuedouble;

    if (interval_s >= (double)MODBUS_MIN_TELEMETRY_INTERVAL_S) {
      if (interval_s > (double)MODBUS_MAX_TELEMETRY_INTERVAL_S) {
        cfg->telemetry_interval_s = MODBUS_MAX_TELEMETRY_INTERVAL_S;
      } else {
        cfg->telemetry_interval_s = (uint16_t)interval_s;
      }
    }
  }

  /*
   * Basic validation for new config.
   *
   * For now, require only these:
   * - device_id
   * - configId
   * - apiKey
   *
   * Keep deploymentId/hash optional for now to avoid blocking rollout.
   */
  if (cfg->device_id[0] == '\0') {
    cJSON_Delete(root);
    return false;
  }

  if (cfg->config_id[0] == '\0') {
    cJSON_Delete(root);
    return false;
  }

  if (cfg->api_key[0] == '\0') {
    cJSON_Delete(root);
    return false;
  }

  // modbusSlaves[]
  cJSON *slaves = cJSON_GetObjectItemCaseSensitive(cfg_json, "modbusSlaves");
  if (!cJSON_IsArray(slaves)) {
    cJSON_Delete(root);
    return false;
  }

  uint8_t sidx = 0;

  cJSON *s = NULL;
  cJSON_ArrayForEach(s, slaves) {
    if (!cJSON_IsObject(s)) continue;
    if (sidx >= MODBUS_MAX_SLAVES) break;

    mb_slave_cfg_t *sc = &cfg->slaves[sidx];
    memset(sc, 0, sizeof(*sc));
    sc->baud = MODBUS_DEFAULT_SLAVE_BAUD;
    sc->data_bits = MODBUS_DEFAULT_SLAVE_DATA_BITS;
    sc->stop_bits = MODBUS_DEFAULT_SLAVE_STOP_BITS;
    sc->parity = MODBUS_DEFAULT_SLAVE_PARITY;
    sc->interval_ms = MODBUS_DEFAULT_SLAVE_INTERVAL_MS;
    sc->timeout_ms = MODBUS_DEFAULT_SLAVE_TIMEOUT_MS;
    sc->retries = MODBUS_DEFAULT_SLAVE_RETRIES;

    // unique_slave_id (UUID string)
    copy_json_str(sc->unique_slave_id, sizeof(sc->unique_slave_id),
                  json_get_any(s, "unique_slave_id", "uniqueSlaveId", "uniqueSlaveID"));

    // numeric slave_id
    cJSON *sid = json_get_any(s, "slave_id", "slaveId", "address");
    if (cJSON_IsNumber(sid) && sid->valuedouble >= 0) {
      sc->slave_id = (uint8_t)sid->valuedouble;
    }

    // serial
    cJSON *serial = cJSON_GetObjectItemCaseSensitive(s, "serial");
    if (cJSON_IsObject(serial)) {
      cJSON *baud = json_get_any(serial, "baudRate", "baud", "baud_rate");
      cJSON *db   = cJSON_GetObjectItemCaseSensitive(serial, "dataBits");
      cJSON *sb   = cJSON_GetObjectItemCaseSensitive(serial, "stopBits");
      cJSON *par  = cJSON_GetObjectItemCaseSensitive(serial, "parity");

      if (cJSON_IsNumber(baud) && baud->valuedouble > 0) {
        sc->baud = (uint32_t)baud->valuedouble;
      }

      if (cJSON_IsNumber(db) && db->valuedouble > 0) {
        sc->data_bits = (uint8_t)db->valuedouble;
      }

      if (cJSON_IsNumber(sb)) {
        sc->stop_bits = ((int)sb->valuedouble == 2) ? 2 : 1;
      }

      if (cJSON_IsString(par) && par->valuestring) {
        sc->parity = parity_from_str(par->valuestring);
      }
    }

    // polling
    cJSON *polling = cJSON_GetObjectItemCaseSensitive(s, "polling");
    if (cJSON_IsObject(polling)) {
      cJSON *ival = json_get_any(polling, "intervalMs", "interval", "pollIntervalMs");
      cJSON *tout = json_get_any(polling, "timeoutMs", "timeout", "timeout_ms");
      cJSON *ret  = cJSON_GetObjectItemCaseSensitive(polling, "retries");

      if (cJSON_IsNumber(ival) && ival->valuedouble > 0) {
        sc->interval_ms = (uint32_t)ival->valuedouble;
      }

      if (cJSON_IsNumber(tout) && tout->valuedouble > 0) {
        sc->timeout_ms = (uint32_t)tout->valuedouble;
      }

      if (cJSON_IsNumber(ret) && ret->valuedouble >= 0) {
        sc->retries = (uint8_t)ret->valuedouble;
      }
    }

    // registers[]
    cJSON *regs = cJSON_GetObjectItemCaseSensitive(s, "registers");
    if (!cJSON_IsArray(regs)) {
      // no registers => skip this slave
      continue;
    }

    uint8_t ridx = 0;

    cJSON *r = NULL;
    cJSON_ArrayForEach(r, regs) {
      if (!cJSON_IsObject(r)) continue;
      if (ridx >= MODBUS_MAX_READS_PER_SLAVE) break;

      mb_read_item_t *rc = &sc->reads[ridx];
      memset(rc, 0, sizeof(*rc));

      copy_json_str(rc->read_id, sizeof(rc->read_id),
                    json_get_any(r, "readId", "read_id", "id"));

      cJSON *func = json_get_any(r, "func", "function", "functionCode");
      rc->func = func_from_json(func);

      cJSON *start = json_get_any(r, "start", "address", "startAddress");
      if (cJSON_IsNumber(start) && start->valuedouble >= 0) {
        rc->start_reg = (uint16_t)start->valuedouble;
      }

      cJSON *bits = json_get_any(r, "bits", "bitWidth", "registerBits");
      if (cJSON_IsNumber(bits)) {
        rc->qty = qty_from_bits((int)bits->valuedouble); // 1/2/4
      }

      if (rc->qty == 0) {
        cJSON *qty = json_get_any(r, "qty", "quantity", "registerCount");
        if (cJSON_IsNumber(qty)) {
          rc->qty = qty_from_register_count((int)qty->valuedouble);
        }
      }

      // minimal validation: must have readId, func (3/4), qty (1/2/4)
      if (rc->read_id[0] == '\0') continue;
      if (!(rc->func == 3 || rc->func == 4)) continue;
      if (!(rc->qty == 1 || rc->qty == 2 || rc->qty == 4)) continue;

      ridx++;
    }

    // Accept slave only if it has required ids + at least 1 read
    if (sc->slave_id == 0) continue; // keep strict for now
    if (ridx == 0) continue;

    if (sc->unique_slave_id[0] == '\0') {
      snprintf(sc->unique_slave_id, sizeof(sc->unique_slave_id), "%u", sc->slave_id);
    }

    sc->read_count = ridx;
    sidx++;
  }

  cfg->slave_count = sidx;

  cJSON_Delete(root);

  /*
   * Important change:
   *
   * Earlier:
   *   return (cfg->slave_count > 0);
   *
   * Now:
   *   return true;
   *
   * Because latest config can validly have:
   *   "modbusSlaves": []
   *
   * Device may still send DI/AI payload without Modbus.
   */
  return true;
}
