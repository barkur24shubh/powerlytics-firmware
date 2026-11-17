//#pragma once
//
//#include <stdint.h>
//#include <stdbool.h>
//#include <string.h>
//
///* ---------- Config metadata ---------- */
//
//#define MODBUS_CFG_MAGIC     0x4D424346u   // 'MBCF'
//#define MODBUS_CFG_VERSION   2u            // bump because struct layout changed
//
///* Limits (keep small; can increase later) */
//#define MODBUS_MAX_SLAVES            4u
//#define MODBUS_MAX_READS_PER_SLAVE   16u
//
///* String sizes (UUID = 36 chars + null) */
//#define MODBUS_UUID_STR_LEN          37u
//#define MODBUS_DEVICE_ID_LEN         32u   // fits Mongo id "695fe..." (24) + slack
//#define MODBUS_IMEI_LEN              20u
//
///* ---------- Modbus function types ---------- */
//
//typedef enum {
//  MB_FUNC_HOLDING_03 = 3,
//  MB_FUNC_INPUT_04   = 4,
//} mb_func_t;
//
///* ---------- One register read definition (readId aligned) ---------- */
//typedef struct {
//  char     read_id[MODBUS_UUID_STR_LEN];  // UUID string
//  uint8_t  func;                          // 3 or 4
//  uint16_t start_reg;                     // start address
//  uint8_t  qty;                           // number of 16-bit registers: 1/2/4
//  uint8_t  reserved0;
//} mb_read_item_t;
//
///* ---------- One slave definition (contains multiple reads) ---------- */
//typedef struct {
//  char     unique_slave_id[MODBUS_UUID_STR_LEN]; // UUID string from config
//  uint8_t  slave_id;                              // numeric modbus address
//
//  // serial (per slave, as per new config)
//  uint32_t baud;
//  uint8_t  parity;     // 0=None, 1=Even, 2=Odd
//  uint8_t  stop_bits;  // 1 or 2
//  uint8_t  data_bits;  // typically 8
//  uint8_t  reserved1;
//
//  // polling (per slave)
//  uint32_t interval_ms;
//  uint32_t timeout_ms;
//  uint8_t  retries;
//  uint8_t  reserved2[3];
//
//  // reads
//  uint8_t        read_count;
//  uint8_t        reserved3[3];
//  mb_read_item_t reads[MODBUS_MAX_READS_PER_SLAVE];
//
//} mb_slave_cfg_t;
//
///* ---------- Full Modbus config stored in FLASH ---------- */
//typedef struct {
//  uint32_t magic;
//  uint16_t version;
//  uint16_t length;
//  uint32_t crc32;
//
//  // identifiers from config JSON
//  char device_id[MODBUS_DEVICE_ID_LEN];
//  char config_id[MODBUS_UUID_STR_LEN];
//  char imei[MODBUS_IMEI_LEN];
//  char deploymentId[...];
//  char hash[...];
//  char apiKey[...];
//
//  // slaves
//  uint8_t        slave_count;
//  uint8_t        reservedA[3];
//  mb_slave_cfg_t slaves[MODBUS_MAX_SLAVES];
//
//} modbus_cfg_t;
//
///* ---------- API ---------- */
//
//// Fill structure with safe defaults
//void modbus_cfg_defaults(modbus_cfg_t *cfg);
//
//// Load config from FLASH (returns false if invalid)
//bool modbus_cfg_load(modbus_cfg_t *out);
//
//// Save config to FLASH
//bool modbus_cfg_save(const modbus_cfg_t *cfg);

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ---------- Config metadata ---------- */

#define MODBUS_CFG_MAGIC     0x4D424346u   // 'MBCF'
#define MODBUS_CFG_VERSION   4u            // full points[] contract plus per-point debounce

/* ---------- Limits ---------- */

#define MODBUS_MAX_SLAVES            4u
#define MODBUS_MAX_READS_PER_SLAVE   6u
#define POWERLYTICS_MAX_POINTS        24u
#define POWERLYTICS_MAX_RULES_PER_POINT 2u
#define POWERLYTICS_MAX_CONTEXT_KEYS  2u

/* ---------- String sizes ---------- */

/*
 * UUID = 36 chars + null
 * Mongo ObjectId = 24 chars + null, but keep slack
 * SHA-256 hex hash = 64 chars + null
 * API key currently appears to be 96 hex chars + null, but keep extra slack
 */

#define MODBUS_UUID_STR_LEN          37u
#define POWERLYTICS_NAME_LEN          24u
#define POWERLYTICS_TAG_LEN           16u
#define POWERLYTICS_CONTEXT_KEY_LEN   24u

#define MODBUS_DEVICE_ID_LEN         32u
#define MODBUS_IMEI_LEN              24u

#define MODBUS_DEPLOYMENT_ID_LEN     32u
#define MODBUS_HASH_LEN              80u
#define MODBUS_API_KEY_LEN           160u

#define MODBUS_DEFAULT_TELEMETRY_INTERVAL_MS 60000UL
#define MODBUS_MIN_TELEMETRY_INTERVAL_MS     1000UL
#define MODBUS_MAX_TELEMETRY_INTERVAL_MS     3600000UL
#define POWERLYTICS_DEFAULT_SAMPLE_INTERVAL_MS 50UL
#define POWERLYTICS_MIN_SAMPLE_INTERVAL_MS     10UL
#define POWERLYTICS_MAX_SAMPLE_INTERVAL_MS     10000UL
#define POWERLYTICS_DEFAULT_DI_DEBOUNCE_MS      50UL
#define POWERLYTICS_DEFAULT_EVENT_CAP          8U
#define POWERLYTICS_MAX_EVENT_CAP              8U

#define MODBUS_CFG_DEBUG_FLAG_VALID          0x80u
#define MODBUS_CFG_DEBUG_FLAG_UART           0x01u

/* ---------- Modbus function types ---------- */

typedef enum {
  MB_FUNC_HOLDING_03 = 3,
  MB_FUNC_INPUT_04   = 4,
} mb_func_t;

typedef enum {
  POWERLYTICS_POINT_DISABLED = 0,
  POWERLYTICS_POINT_DIGITAL,
  POWERLYTICS_POINT_COUNTER,
  POWERLYTICS_POINT_ANALOG,
  POWERLYTICS_POINT_RELAY,
  POWERLYTICS_POINT_MODBUS,
} powerlytics_point_kind_t;

typedef enum {
  POWERLYTICS_MODBUS_KIND_ANALOG = 0,
  POWERLYTICS_MODBUS_KIND_DIGITAL,
  POWERLYTICS_MODBUS_KIND_COUNTER,
  POWERLYTICS_MODBUS_KIND_ENUM,
  POWERLYTICS_MODBUS_KIND_RAW,
} powerlytics_modbus_value_kind_t;

typedef enum {
  POWERLYTICS_RULE_NONE = 0,
  POWERLYTICS_RULE_RISING_EDGE,
  POWERLYTICS_RULE_FALLING_EDGE,
  POWERLYTICS_RULE_THRESHOLD_HIGH,
  POWERLYTICS_RULE_THRESHOLD_LOW,
  POWERLYTICS_RULE_OUT_OF_RANGE,
  POWERLYTICS_RULE_IN_RANGE,
} powerlytics_rule_type_t;

typedef enum {
  POWERLYTICS_SEVERITY_INFO = 0,
  POWERLYTICS_SEVERITY_WARNING,
  POWERLYTICS_SEVERITY_CRITICAL,
} powerlytics_severity_t;

typedef enum {
  POWERLYTICS_EDGE_RISING = 0,
  POWERLYTICS_EDGE_FALLING,
  POWERLYTICS_EDGE_BOTH,
} powerlytics_edge_t;

typedef enum {
  POWERLYTICS_LATCH_NONE = 0,
  POWERLYTICS_LATCH_UNTIL_NORMAL,
  POWERLYTICS_LATCH_UNTIL_RESET,
} powerlytics_latch_mode_t;

typedef enum {
  POWERLYTICS_DTYPE_UINT16 = 0,
  POWERLYTICS_DTYPE_INT16,
  POWERLYTICS_DTYPE_UINT32,
  POWERLYTICS_DTYPE_INT32,
  POWERLYTICS_DTYPE_FLOAT32,
  POWERLYTICS_DTYPE_BOOL,
  POWERLYTICS_DTYPE_BIT,
  POWERLYTICS_DTYPE_BITFIELD,
  POWERLYTICS_DTYPE_ENUM,
  POWERLYTICS_DTYPE_RAW16,
} powerlytics_data_type_t;

typedef enum {
  POWERLYTICS_ENDIAN_NONE = 0,
  POWERLYTICS_ENDIAN_ABCD,
  POWERLYTICS_ENDIAN_BADC,
  POWERLYTICS_ENDIAN_CDAB,
  POWERLYTICS_ENDIAN_DCBA,
} powerlytics_endianness_t;

typedef struct {
  uint8_t  type;
  uint8_t  enabled;
  uint8_t  severity;
  uint8_t  latch_mode;
  uint32_t debounce_ms;
  uint32_t duration_ms;
  uint32_t recovery_duration_ms;
  float    high;
  float    low;
  float    hysteresis;
  uint8_t  context_count;
  char     context_keys[POWERLYTICS_MAX_CONTEXT_KEYS][POWERLYTICS_CONTEXT_KEY_LEN];
} powerlytics_rule_cfg_t;

typedef struct {
  char     key[16];
  char     name[POWERLYTICS_NAME_LEN];
  uint8_t  kind;
  uint8_t  enabled;
  uint8_t  edge;
  uint8_t  active_high;
  uint8_t  counter_width;
  uint8_t  rule_count;
  uint16_t reserved;
  uint32_t debounce_ms;
  uint32_t rollover_at;
  float    scale;
  float    offset;
  char     unit[16];
  powerlytics_rule_cfg_t rules[POWERLYTICS_MAX_RULES_PER_POINT];
} powerlytics_point_cfg_t;

/* ---------- One register read definition ---------- */

typedef struct {
  char     read_id[MODBUS_UUID_STR_LEN];  // UUID string
  char     name[POWERLYTICS_NAME_LEN];
  char     tag[POWERLYTICS_TAG_LEN];
  uint8_t  func;                          // 3 or 4
  uint16_t start_reg;                     // start address
  uint8_t  qty;                           // number of 16-bit registers: 1 / 2 / 4
  uint8_t  kind;
  uint8_t  data_type;
  uint8_t  endianness;
  uint8_t  bit_index;
  uint8_t  rule_count;
  float    scale;
  float    offset;
  char     unit[16];
  powerlytics_rule_cfg_t rules[POWERLYTICS_MAX_RULES_PER_POINT];
} mb_read_item_t;

/* ---------- One slave definition ---------- */

typedef struct {
  char     unique_slave_id[MODBUS_UUID_STR_LEN]; // UUID string from config
  char     name[POWERLYTICS_NAME_LEN];
  uint8_t  slave_id;                             // numeric Modbus address

  /*
   * Serial settings per slave.
   * Note: physically, one RS-485 UART can only run one baud/parity setting at a time.
   * If different slaves use different serial settings, firmware must reconfigure UART
   * before polling that slave.
   */
  uint32_t baud;
  uint8_t  parity;     // 0=None, 1=Even, 2=Odd
  uint8_t  stop_bits;  // 1 or 2
  uint8_t  data_bits;  // typically 8
  uint8_t  reserved1;

  /* Polling settings per slave */
  uint32_t interval_ms;
  uint32_t timeout_ms;
  uint8_t  retries;
  uint8_t  reserved2[3];

  /* Register reads */
  uint8_t        read_count;
  uint8_t        reserved3[3];
  mb_read_item_t reads[MODBUS_MAX_READS_PER_SLAVE];

} mb_slave_cfg_t;

/* ---------- Full config stored in FLASH ---------- */

typedef struct {
  /*
   * Header used for flash validation.
   * crc32 should normally be calculated over the payload area after this header,
   * not including magic/version/length/crc32 itself.
   */
  uint32_t magic;
  uint16_t version;
  uint16_t reserved_header;
  uint32_t length;
  uint32_t crc32;

  /*
   * Identifiers from config JSON.
   *
   * Incoming JSON names:
   *   device_id
   *   configId
   *   imei
   *   deploymentId
   *   hash
   *   apiKey
   *
   * C names intentionally use snake_case.
   */
  char device_id[MODBUS_DEVICE_ID_LEN];
  char config_id[MODBUS_UUID_STR_LEN];
  char imei[MODBUS_IMEI_LEN];

  char deployment_id[MODBUS_DEPLOYMENT_ID_LEN];
  char hash[MODBUS_HASH_LEN];
  char api_key[MODBUS_API_KEY_LEN];

  uint8_t  protocol_version;
  uint8_t  send_full_snapshot;
  uint8_t  event_cap_per_interval;
  uint8_t  point_count;
  uint32_t telemetry_interval_ms;
  uint32_t sample_interval_ms;

  powerlytics_point_cfg_t points[POWERLYTICS_MAX_POINTS];

  /* Modbus slaves */
  uint8_t        slave_count;
  uint8_t        debug_flags;
  uint16_t       reserved_flags;
  mb_slave_cfg_t slaves[MODBUS_MAX_SLAVES];

} modbus_cfg_t;

#ifndef POWERLYTICS_UART_DEBUG_ENABLED
#define POWERLYTICS_UART_DEBUG_ENABLED 1U
#endif

static inline uint8_t modbus_cfg_default_debug_print_enabled(void)
{
  return POWERLYTICS_UART_DEBUG_ENABLED ? 1U : 0U;
}

static inline void modbus_cfg_set_debug_print_enabled(modbus_cfg_t *cfg,
                                                      uint8_t enabled)
{
  if (!cfg) return;
  cfg->debug_flags = MODBUS_CFG_DEBUG_FLAG_VALID |
                     (enabled ? MODBUS_CFG_DEBUG_FLAG_UART : 0u);
}

static inline uint8_t modbus_cfg_debug_print_enabled(const modbus_cfg_t *cfg)
{
  if (!cfg || ((cfg->debug_flags & MODBUS_CFG_DEBUG_FLAG_VALID) == 0u)) {
    return modbus_cfg_default_debug_print_enabled();
  }

  return (cfg->debug_flags & MODBUS_CFG_DEBUG_FLAG_UART) ? 1u : 0u;
}

/* ---------- API ---------- */

// Fill structure with safe defaults
void modbus_cfg_defaults(modbus_cfg_t *cfg);

// Shared large config workspace used for boot/downlink parse-load-save flows.
modbus_cfg_t *modbus_cfg_workspace(void);

/* External flash used for the A/B config copies (call before load/save). */
void modbus_cfg_set_flash(void *flash);

// Load config from FLASH. Returns false if invalid, CRC mismatch, or version mismatch.
bool modbus_cfg_load(modbus_cfg_t *out);

// Save config to FLASH.
bool modbus_cfg_save(const modbus_cfg_t *cfg);
