#ifndef APP_IDENTITY_CORE_H
#define APP_IDENTITY_CORE_H

/* Device identity, pure logic (firmware 1.0.21; platform side: docs/ids.md there).
 *
 * ONE id names this unit for its whole life: the deviceId, the platform's own id for it, 24 lowercase hex characters,
 * generated when the unit is registered on the platform. It is written into the unit's flash ONCE, when the unit is
 * programmed, and it is the second segment of every MQTT topic and the client id. Nothing else is ever an identity:
 * not the modem IMEI, not the MCU unique ID, not the deployed configuration. Those are attributes the unit reports in
 * `hello`. A unit with no stored deviceId stays off the network (it still runs its local rules).
 *
 * No HAL here: tested on the host (scripts/identity_host_test.c). */

#include <stdint.h>
#include <stddef.h>

#define APP_ID_LEN 24U /* characters in a deviceId, without the terminator */

/* 1 for exactly 24 lowercase hex characters. */
uint8_t app_id_is_device_id(const char *s);

/* 1 when a configuration's device_id names the unit whose stored id is `record` (the unit refuses nothing on a
 * mismatch; it logs it). */
uint8_t app_id_config_matches(const char *record, const char *config);

/* The three 32-bit words of the STM32 factory unique ID as 24 uppercase hex characters (+ terminator). */
void app_id_format_uid(const uint32_t words[3], char out[APP_ID_LEN + 1U]);

/* ---- the record kept in flash ---- */
#define APP_ID_RECORD_MAGIC 0x4E454449UL /* "IDEN" */
#define APP_ID_RECORD_VERSION 1UL

typedef struct {
  uint32_t magic;
  uint32_t version;
  uint32_t seq;       /* rises with every write; the highest valid copy wins */
  char     device_id[28];
  uint32_t crc32;     /* over every byte before this field */
} app_id_record_t;

void    app_id_record_pack(app_id_record_t *r, const char *device_id, uint32_t seq);
/* 1 when magic, version, id and crc are all good. */
uint8_t app_id_record_valid(const app_id_record_t *r);
uint32_t app_id_crc32(const void *data, size_t len);

#endif
