#include "modbus_config.h"
#include "stm32f4xx_hal.h"
#include "w25q16.h"
#include "debug_uart.h"
#include <stddef.h>
#include <string.h>

/*
 * Where the configuration lives (2026-11-17):
 *
 * Two copies (A/B) in the external W25Q, each 16 KB (4 x 4 KB sectors):
 *   A = 0x060000, B = 0x064000 (free space between the engine sequence
 *   sector at 0x050000 and the OTA backup slot at 0x080000).
 * A save goes to the copy that is NOT the newest valid one: erase, write
 * the body, verify, and only then write the small header (magic, sequence,
 * length, crc). Power lost at any point leaves the previous copy valid.
 * Load takes the valid copy with the highest sequence.
 *
 * Before, the config was one internal-flash sector (sector 7, 0x08060000)
 * erased and rewritten in place: a power cut between erase and write left
 * no config at all, the 128 KB erase stalled the CPU for ~1-2 s, and an OTA
 * rollback (which restores the whole application region) also restored an
 * old config. That sector is now only read once, to migrate older units.
 */
#define CFG_SLOT_A_ADDR        0x060000UL
#define CFG_SLOT_B_ADDR        0x064000UL
#define CFG_SLOT_SPAN          0x004000UL
#define CFG_SLOT_HDR_SIZE      256UL      /* one page; body starts after it */
#define CFG_SLOT_MAGIC         0x53474643UL /* "CFGS" */

typedef struct {
  uint32_t magic;
  uint32_t seq;
  uint32_t length;
  uint32_t crc32;      /* over the body bytes */
  uint32_t hdr_crc32;  /* over the four fields above */
} cfg_slot_hdr_t;

static W25Q_Handle *s_cfg_flash;

void modbus_cfg_set_flash(void *flash)
{
  s_cfg_flash = (W25Q_Handle *)flash;
}

/* ---- Choose Flash location (edit if your flash size differs) ---- */
#ifndef MODBUS_CFG_FLASH_ADDR
#define MODBUS_CFG_FLASH_ADDR   (0x08060000u)      // adjust to your MCU flash map
#endif

#ifndef MODBUS_CFG_FLASH_SECTOR
#define MODBUS_CFG_FLASH_SECTOR FLASH_SECTOR_7
#endif


_Static_assert(CFG_SLOT_HDR_SIZE + sizeof(modbus_cfg_t) <= CFG_SLOT_SPAN, "config does not fit one W25Q slot");

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

/* ---------- Load ---------- */
static void cfg_normalise(modbus_cfg_t *out)
{
  out->telemetry_interval_ms = clamp_telemetry_interval_ms(out->telemetry_interval_ms);
  out->sample_interval_ms = clamp_sample_interval_ms(out->sample_interval_ms);
  if (out->event_cap_per_interval == 0U) {
    out->event_cap_per_interval = POWERLYTICS_DEFAULT_EVENT_CAP;
  }
}

static uint32_t slot_hdr_crc(const cfg_slot_hdr_t *h)
{
  return ~crc32_update(0xFFFFFFFFu, h, (uint32_t)offsetof(cfg_slot_hdr_t, hdr_crc32));
}

/* Reads a slot header; returns 1 when it describes a plausible body. */
static uint8_t slot_read_hdr(uint32_t addr, cfg_slot_hdr_t *h)
{
  if (!s_cfg_flash || !w25q_read(s_cfg_flash, addr, (uint8_t *)h, sizeof(*h))) return 0U;
  if (h->magic != CFG_SLOT_MAGIC || h->length != sizeof(modbus_cfg_t)) return 0U;
  return (slot_hdr_crc(h) == h->hdr_crc32) ? 1U : 0U;
}

static uint8_t slot_read_body(uint32_t addr, const cfg_slot_hdr_t *h, modbus_cfg_t *out)
{
  if (!w25q_read(s_cfg_flash, addr + CFG_SLOT_HDR_SIZE, (uint8_t *)out, sizeof(*out))) return 0U;
  if (~crc32_update(0xFFFFFFFFu, out, sizeof(*out)) != h->crc32) return 0U;
  return cfg_is_valid(out) ? 1U : 0U;
}

/* Newest valid slot: sets *addr / *seq and returns 1, or 0 when none. */
static uint8_t slot_newest(uint32_t *addr, uint32_t *seq)
{
  cfg_slot_hdr_t a, b;
  uint8_t va = slot_read_hdr(CFG_SLOT_A_ADDR, &a);
  uint8_t vb = slot_read_hdr(CFG_SLOT_B_ADDR, &b);
  if (!va && !vb) return 0U;
  if (va && (!vb || (int32_t)(a.seq - b.seq) > 0)) { *addr = CFG_SLOT_A_ADDR; *seq = a.seq; }
  else { *addr = CFG_SLOT_B_ADDR; *seq = b.seq; }
  return 1U;
}

static uint8_t slots_load(modbus_cfg_t *out)
{
  cfg_slot_hdr_t h[2];
  uint8_t v[2] = { slot_read_hdr(CFG_SLOT_A_ADDR, &h[0]), slot_read_hdr(CFG_SLOT_B_ADDR, &h[1]) };
  const uint32_t addr[2] = { CFG_SLOT_A_ADDR, CFG_SLOT_B_ADDR };
  /* newest first, then the other one */
  int first = (v[0] && (!v[1] || (int32_t)(h[0].seq - h[1].seq) > 0)) ? 0 : 1;
  for (int k = 0; k < 2; k++) {
    int i = (k == 0) ? first : 1 - first;
    if (v[i] && slot_read_body(addr[i], &h[i], out)) {
      if (k == 1) print("[CONFIG][FLASH] newest copy unreadable, using the older one seq=%lu\r\n", (unsigned long)h[i].seq);
      return 1U;
    }
  }
  return 0U;
}

bool modbus_cfg_load(modbus_cfg_t *out)
{
  if (!out) return false;

  if (slots_load(out)) {
    cfg_normalise(out);
    return true;
  }

  /* Units configured before 2026-11-17 keep their config in internal flash:
   * use it once and copy it into the W25Q slots. */
  const modbus_cfg_t *in_flash = (const modbus_cfg_t*)MODBUS_CFG_FLASH_ADDR;
  if (cfg_is_valid(in_flash)) {
    memcpy(out, in_flash, sizeof(*out));
    if (s_cfg_flash) {
      print("[CONFIG][FLASH] migrating config from internal flash to W25Q\r\n");
      (void)modbus_cfg_save(out);
      memcpy(out, in_flash, sizeof(*out));
    }
    cfg_normalise(out);
    return true;
  }

  modbus_cfg_defaults(out);
  return false;
}

/* ---------- Save ---------- */
bool modbus_cfg_save(const modbus_cfg_t *cfg)
{
  if (!cfg || !s_cfg_flash) return false;

  modbus_cfg_t *w = &s_cfg_workspace;
  if (cfg != w) {
    memcpy(w, cfg, sizeof(*w));
  }

  // enforce header fields
  w->magic   = MODBUS_CFG_MAGIC;
  w->version = MODBUS_CFG_VERSION;
  w->length  = sizeof(modbus_cfg_t);

  // clamp counts (defensive; the JSON parser already refuses over-limit configs)
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
  w->crc32 = cfg_crc32_calc(w);

  /* Write over the older (or invalid) copy; keep the newest one intact. */
  uint32_t newest_addr = 0U, newest_seq = 0U;
  uint8_t have = slot_newest(&newest_addr, &newest_seq);
  uint32_t target = (have && newest_addr == CFG_SLOT_A_ADDR) ? CFG_SLOT_B_ADDR : CFG_SLOT_A_ADDR;
  cfg_slot_hdr_t h = {
    .magic = CFG_SLOT_MAGIC,
    .seq = have ? newest_seq + 1U : 1U,
    .length = sizeof(modbus_cfg_t),
    .crc32 = ~crc32_update(0xFFFFFFFFu, w, sizeof(*w)),
  };
  h.hdr_crc32 = slot_hdr_crc(&h);

  for (uint32_t off = 0U; off < CFG_SLOT_SPAN; off += W25Q_SECTOR_SIZE) {
    if (!w25q_erase_sector_4k(s_cfg_flash, target + off)) return false;
  }
  if (!w25q_write(s_cfg_flash, target + CFG_SLOT_HDR_SIZE, (const uint8_t *)w, sizeof(*w))) return false;

  /* Verify the body before committing the header. */
  static uint8_t chk[256];
  for (uint32_t off = 0U; off < sizeof(*w); off += sizeof(chk)) {
    uint32_t n = sizeof(*w) - off;
    if (n > sizeof(chk)) n = sizeof(chk);
    if (!w25q_read(s_cfg_flash, target + CFG_SLOT_HDR_SIZE + off, chk, n)) return false;
    if (memcmp(chk, (const uint8_t *)w + off, n) != 0) return false;
  }
  if (!w25q_write(s_cfg_flash, target, (const uint8_t *)&h, sizeof(h))) return false;

  cfg_slot_hdr_t back;
  if (!slot_read_hdr(target, &back) || back.seq != h.seq) return false;
  print("[CONFIG][FLASH] saved to slot %c seq=%lu\r\n",
        (target == CFG_SLOT_A_ADDR) ? 'A' : 'B', (unsigned long)h.seq);
  return true;
}
