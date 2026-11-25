#include "app_identity_core.h"

#include <string.h>

uint8_t app_id_is_device_id(const char *s)
{
  if (!s) return 0U;
  for (uint8_t i = 0U; i < APP_ID_LEN; i++) {
    char c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0U;
  }
  return (s[APP_ID_LEN] == '\0') ? 1U : 0U;
}

const char *app_id_choose(const char *record, const char *config, const char *imei, app_id_src_t *src)
{
  app_id_src_t s = APP_ID_SRC_NONE;
  const char *id = NULL;
  if (app_id_is_device_id(record)) { id = record; s = APP_ID_SRC_RECORD; }
  else if (app_id_is_device_id(config)) { id = config; s = APP_ID_SRC_CONFIG; }
  else if (imei && imei[0]) { id = imei; s = APP_ID_SRC_IMEI; }
  if (src) *src = s;
  return id;
}

const char *app_id_src_name(app_id_src_t src)
{
  switch (src) {
    case APP_ID_SRC_RECORD: return "record";
    case APP_ID_SRC_CONFIG: return "config";
    case APP_ID_SRC_IMEI:   return "imei";
    default:                return "none";
  }
}

void app_id_format_uid(const uint32_t words[3], char out[APP_ID_LEN + 1U])
{
  static const char hex[] = "0123456789ABCDEF";
  for (uint8_t w = 0U; w < 3U; w++) {
    for (uint8_t n = 0U; n < 8U; n++) {
      out[w * 8U + n] = hex[(words[w] >> (28U - 4U * n)) & 0xFU];
    }
  }
  out[APP_ID_LEN] = '\0';
}

uint32_t app_id_crc32(const void *data, size_t len)
{
  const uint8_t *p = (const uint8_t *)data;
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t i = 0U; i < len; i++) {
    crc ^= p[i];
    for (uint8_t b = 0U; b < 8U; b++) {
      uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
      crc = (crc >> 1) ^ (0xEDB88320U & mask);
    }
  }
  return ~crc;
}

void app_id_record_pack(app_id_record_t *r, const char *device_id, uint32_t seq)
{
  if (!r) return;
  memset(r, 0, sizeof(*r));
  r->magic = APP_ID_RECORD_MAGIC;
  r->version = APP_ID_RECORD_VERSION;
  r->seq = seq;
  if (device_id) strncpy(r->device_id, device_id, APP_ID_LEN);
  r->crc32 = app_id_crc32(r, offsetof(app_id_record_t, crc32));
}

uint8_t app_id_record_valid(const app_id_record_t *r)
{
  if (!r || r->magic != APP_ID_RECORD_MAGIC || r->version != APP_ID_RECORD_VERSION) return 0U;
  if (r->crc32 != app_id_crc32(r, offsetof(app_id_record_t, crc32))) return 0U;
  return app_id_is_device_id(r->device_id);
}
