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
#define MODBUS_CFG_VERSION   2u            // bump because struct layout changed

/* ---------- Limits ---------- */

#define MODBUS_MAX_SLAVES            4u
#define MODBUS_MAX_READS_PER_SLAVE   16u

/* ---------- String sizes ---------- */

/*
 * UUID = 36 chars + null
 * Mongo ObjectId = 24 chars + null, but keep slack
 * SHA-256 hex hash = 64 chars + null
 * API key currently appears to be 96 hex chars + null, but keep extra slack
 */

#define MODBUS_UUID_STR_LEN          37u

#define MODBUS_DEVICE_ID_LEN         32u
#define MODBUS_IMEI_LEN              24u

#define MODBUS_DEPLOYMENT_ID_LEN     32u
#define MODBUS_HASH_LEN              80u
#define MODBUS_API_KEY_LEN           160u

#define MODBUS_DEFAULT_TELEMETRY_INTERVAL_S  1u
#define MODBUS_MIN_TELEMETRY_INTERVAL_S      1u
#define MODBUS_MAX_TELEMETRY_INTERVAL_S      3600u

#define MODBUS_CFG_DEBUG_FLAG_VALID          0x80u
#define MODBUS_CFG_DEBUG_FLAG_UART           0x01u

/* ---------- Modbus function types ---------- */

typedef enum {
  MB_FUNC_HOLDING_03 = 3,
  MB_FUNC_INPUT_04   = 4,
} mb_func_t;

/* ---------- One register read definition ---------- */

typedef struct {
  char     read_id[MODBUS_UUID_STR_LEN];  // UUID string
  uint8_t  func;                          // 3 or 4
  uint16_t start_reg;                     // start address
  uint8_t  qty;                           // number of 16-bit registers: 1 / 2 / 4
  uint8_t  reserved0;
} mb_read_item_t;

/* ---------- One slave definition ---------- */

typedef struct {
  char     unique_slave_id[MODBUS_UUID_STR_LEN]; // UUID string from config
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
  uint16_t length;
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

  /* Modbus slaves */
  uint8_t        slave_count;
  uint8_t        debug_flags;
  uint16_t       telemetry_interval_s;
  mb_slave_cfg_t slaves[MODBUS_MAX_SLAVES];

} modbus_cfg_t;

static inline uint8_t modbus_cfg_default_debug_print_enabled(void)
{
  return 1u;
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

// Load config from FLASH. Returns false if invalid, CRC mismatch, or version mismatch.
bool modbus_cfg_load(modbus_cfg_t *out);

// Save config to FLASH.
bool modbus_cfg_save(const modbus_cfg_t *cfg);
