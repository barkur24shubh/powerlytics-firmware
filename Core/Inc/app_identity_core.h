#ifndef APP_IDENTITY_CORE_H
#define APP_IDENTITY_CORE_H

/* Device identity, pure logic (firmware 1.0.20; platform side: docs/program/device-identity-proposal.md there).
 *
 * ONE id names this unit everywhere: the deviceId, the platform's own id for it, 24 lowercase hex characters. It is the
 * second segment of every MQTT topic and the client id. Hardware ids (the modem IMEI, the MCU factory unique ID) are
 * attributes the unit reports in `hello`; they are never the identity once a deviceId is known.
 *
 * Where the unit gets its deviceId, first match wins:
 *   1. the identity record written at manufacture (flash, over USB)
 *   2. the `device_id` of the configuration the platform deployed (the platform's id for this unit)
 *   3. nothing yet: a cellular unit falls back to its IMEI for first contact (firmware before 1.0.20 always did), and
 *      the first configuration then gives it its deviceId.
 *
 * No HAL here: tested on the host (scripts/identity_host_test.c). */

#include <stdint.h>
#include <stddef.h>

#define APP_ID_LEN 24U /* characters in a deviceId, without the terminator */

typedef enum {
  APP_ID_SRC_NONE = 0,
  APP_ID_SRC_RECORD,
  APP_ID_SRC_CONFIG,
  APP_ID_SRC_IMEI,
} app_id_src_t;

/* 1 for exactly 24 lowercase hex characters. */
uint8_t app_id_is_device_id(const char *s);

/* The id topics are built with. `*src` (optional) says which source won; NULL when there is nothing at all. */
const char *app_id_choose(const char *record, const char *config, const char *imei, app_id_src_t *src);

const char *app_id_src_name(app_id_src_t src);

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
