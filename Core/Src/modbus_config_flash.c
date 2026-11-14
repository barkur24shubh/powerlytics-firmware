#include "modbus_config.h"
#include "stm32f4xx_hal.h"
#include <stddef.h>
#include <string.h>

/* ---- Choose Flash location (edit if your flash size differs) ---- */
#ifndef MODBUS_CFG_FLASH_ADDR
#define MODBUS_CFG_FLASH_ADDR   (0x08060000u)      // adjust to your MCU flash map
#endif

#ifndef MODBUS_CFG_FLASH_SECTOR
#define MODBUS_CFG_FLASH_SECTOR FLASH_SECTOR_7
#endif


static modbus_cfg_t s_cfg_workspace;

modbus_cfg_t *modbus_cfg_workspace(void)
{
  return &s_cfg_workspace;
}

/* ---------- CRC32 software (simple, reliable) ---------- */
static uint32_t crc32_update(uint32_t crc, const void *data, uint32_t len)
{
  const uint8_t *p = (const uint8_t*)data;

  for (uint32_t i = 0; i < len; i++) {
    crc ^= p[i];
    for (int b = 0; b < 8; b++) {
      uint32_t mask = -(crc & 1u);
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return crc;
}

static uint32_t cfg_crc32_calc(const modbus_cfg_t *cfg)
{
  static const uint8_t zero_crc[sizeof(uint32_t)] = {0U, 0U, 0U, 0U};
  const uint8_t *bytes = (const uint8_t*)cfg;
  uint32_t crc = 0xFFFFFFFFu;
  const uint32_t crc_off = (uint32_t)offsetof(modbus_cfg_t, crc32);

  crc = crc32_update(crc, bytes, crc_off);
  crc = crc32_update(crc, zero_crc, sizeof(zero_crc));
  crc = crc32_update(crc, bytes + crc_off + sizeof(uint32_t),
                     (uint32_t)sizeof(modbus_cfg_t) - crc_off - (uint32_t)sizeof(uint32_t));
  return ~crc;
}

/* ---------- Defaults ---------- */
//void modbus_cfg_defaults(modbus_cfg_t *cfg)
//{
//  if (!cfg) return;
//  memset(cfg, 0, sizeof(*cfg));
//
//  cfg->magic   = MODBUS_CFG_MAGIC;
//  cfg->version = MODBUS_CFG_VERSION;
//  cfg->length  = sizeof(modbus_cfg_t);
//
//  // identifiers empty by default
//  cfg->device_id[0] = '\0';
//  cfg->config_id[0] = '\0';
//  cfg->imei[0]      = '\0';
//
//  // no slaves by default
//  cfg->slave_count = 0;
//
//  // (optional) you can preset per-slave defaults here, but not required
//}
void modbus_cfg_defaults(modbus_cfg_t *cfg)
{
  if (!cfg) return;
  memset(cfg, 0, sizeof(*cfg));

  cfg->magic   = MODBUS_CFG_MAGIC;
  cfg->version = MODBUS_CFG_VERSION;
  cfg->length  = sizeof(modbus_cfg_t);

  // identifiers empty by default
  cfg->device_id[0] = '\0';
  cfg->config_id[0] = '\0';
  cfg->imei[0]      = '\0';

  // new fields
  cfg->deployment_id[0] = '\0';
  cfg->hash[0]          = '\0';
  cfg->api_key[0]       = '\0';
  cfg->protocol_version = 1U;
  cfg->send_full_snapshot = 1U;
  cfg->event_cap_per_interval = POWERLYTICS_DEFAULT_EVENT_CAP;
  cfg->point_count = 0U;
  cfg->telemetry_interval_ms = MODBUS_DEFAULT_TELEMETRY_INTERVAL_MS;
  cfg->sample_interval_ms = POWERLYTICS_DEFAULT_SAMPLE_INTERVAL_MS;
  modbus_cfg_set_debug_print_enabled(cfg,
                                     modbus_cfg_default_debug_print_enabled());

  // no slaves by default
  cfg->slave_count = 0;
}

static uint32_t clamp_telemetry_interval_ms(uint32_t value)
{
  if (value < MODBUS_MIN_TELEMETRY_INTERVAL_MS) {
    return MODBUS_DEFAULT_TELEMETRY_INTERVAL_MS;
  }

  if (value > MODBUS_MAX_TELEMETRY_INTERVAL_MS) {
    return MODBUS_MAX_TELEMETRY_INTERVAL_MS;
  }

  return value;
}

static uint32_t clamp_sample_interval_ms(uint32_t value)
{
  if (value < POWERLYTICS_MIN_SAMPLE_INTERVAL_MS) {
    return POWERLYTICS_DEFAULT_SAMPLE_INTERVAL_MS;
  }

  if (value > POWERLYTICS_MAX_SAMPLE_INTERVAL_MS) {
    return POWERLYTICS_MAX_SAMPLE_INTERVAL_MS;
  }

  return value;
}

/* ---------- Validate flash struct ---------- */
static bool cfg_is_valid(const modbus_cfg_t *cfg)
{
  if (cfg->magic != MODBUS_CFG_MAGIC) return false;
  if (cfg->version != MODBUS_CFG_VERSION) return false;
  if (cfg->length != sizeof(modbus_cfg_t)) return false;

  if (cfg->slave_count > MODBUS_MAX_SLAVES) return false;
  if (cfg->point_count > POWERLYTICS_MAX_POINTS) return false;

  // validate read counts (defensive)
  for (uint8_t i = 0; i < cfg->slave_count; i++) {
    if (cfg->slaves[i].read_count > MODBUS_MAX_READS_PER_SLAVE) return false;
  }

  uint32_t calc = cfg_crc32_calc(cfg);
  return (cfg->crc32 == calc);
}

/* ---------- Load from flash ---------- */
bool modbus_cfg_load(modbus_cfg_t *out)
{
  if (!out) return false;

  const modbus_cfg_t *in_flash = (const modbus_cfg_t*)MODBUS_CFG_FLASH_ADDR;

  if (cfg_is_valid(in_flash)) {
    memcpy(out, in_flash, sizeof(*out));
    out->telemetry_interval_ms = clamp_telemetry_interval_ms(out->telemetry_interval_ms);
    out->sample_interval_ms = clamp_sample_interval_ms(out->sample_interval_ms);
    if (out->event_cap_per_interval == 0U) {
      out->event_cap_per_interval = POWERLYTICS_DEFAULT_EVENT_CAP;
    }
    return true;
  }

  modbus_cfg_defaults(out);
  return false;
}

/* ---------- Erase one sector ---------- */
static HAL_StatusTypeDef flash_erase_sector(uint32_t sector)
{
  FLASH_EraseInitTypeDef erase = {0};
  uint32_t sector_error = 0;

  erase.TypeErase    = FLASH_TYPEERASE_SECTORS;
  erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;
  erase.Sector       = sector;
  erase.NbSectors    = 1;

  return HAL_FLASHEx_Erase(&erase, &sector_error);
}

/* ---------- Save to flash ---------- */
bool modbus_cfg_save(const modbus_cfg_t *cfg)
{
  if (!cfg) return false;

  modbus_cfg_t *w = &s_cfg_workspace;
  if (cfg != w) {
    memcpy(w, cfg, sizeof(*w));
  }

  // enforce header fields
  w->magic   = MODBUS_CFG_MAGIC;
  w->version = MODBUS_CFG_VERSION;
  w->length  = sizeof(modbus_cfg_t);

  // clamp counts (defensive)
  if (w->slave_count > MODBUS_MAX_SLAVES) w->slave_count = MODBUS_MAX_SLAVES;
  if (w->point_count > POWERLYTICS_MAX_POINTS) w->point_count = POWERLYTICS_MAX_POINTS;
  w->telemetry_interval_ms = clamp_telemetry_interval_ms(w->telemetry_interval_ms);
  w->sample_interval_ms = clamp_sample_interval_ms(w->sample_interval_ms);
  if (w->protocol_version == 0U) w->protocol_version = 1U;
  if (w->event_cap_per_interval == 0U) w->event_cap_per_interval = POWERLYTICS_DEFAULT_EVENT_CAP;
  for (uint8_t i = 0; i < w->slave_count; i++) {
    if (w->slaves[i].read_count > MODBUS_MAX_READS_PER_SLAVE) {
      w->slaves[i].read_count = MODBUS_MAX_READS_PER_SLAVE;
    }
  }

  // compute crc without making a second full struct copy
  w->crc32 = cfg_crc32_calc(w);

  const uint32_t *src = (const uint32_t*)w;
  uint32_t words = sizeof(*w) / 4;

  HAL_FLASH_Unlock();

  if (flash_erase_sector(MODBUS_CFG_FLASH_SECTOR) != HAL_OK) {
    HAL_FLASH_Lock();
    return false;
  }

  for (uint32_t i = 0; i < words; i++) {
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                          MODBUS_CFG_FLASH_ADDR + (i * 4),
                          src[i]) != HAL_OK) {
      HAL_FLASH_Lock();
      return false;
    }
  }

  HAL_FLASH_Lock();

  // verify
  if (memcmp((void*)MODBUS_CFG_FLASH_ADDR, w, sizeof(*w)) != 0) {
    return false;
  }

  return true;
}
