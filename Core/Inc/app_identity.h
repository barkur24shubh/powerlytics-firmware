#ifndef APP_IDENTITY_H
#define APP_IDENTITY_H

/* The unit's identity at run time (firmware 1.0.20). The rules and the pure logic are in app_identity_core.h. */

#include <stddef.h>
#include <stdint.h>
#include "app_identity_core.h"
#include "w25q16.h"

/* Flash (external W25Q16): two 4 KB slots in the free gap after the configuration slots (0x060000..0x067FFF) and before
 * the OTA backup (0x080000). The newest valid copy wins, so a power cut during a write keeps the previous one. */
#define APP_ID_SLOT_A_ADDR 0x06C000UL
#define APP_ID_SLOT_B_ADDR 0x06D000UL

void app_identity_set_flash(W25Q_Handle *flash);

/* Reads the record from flash. Call once after the flash is initialised, before the configuration is applied. */
void app_identity_load(void);

/* The id MQTT topics are built with, or NULL when the unit has none yet (no record, no config, no IMEI). Remembers it. */
const char *app_identity_topic_id(const char *imei);

/* The MQTT client id: "STM32_<imei>" for a unit still on its IMEI (as before 1.0.20), otherwise the deviceId, so the
 * broker can tie the client to its own topics. */
void app_identity_client_id(char *out, size_t n, const char *imei);

/* The deviceId this unit uses, or "" when it is still on its IMEI or has nothing. */
const char *app_identity_device_id(void);
app_id_src_t app_identity_source(const char *imei);

/* Every applied configuration says what the platform calls this unit (device_id). The first one gives a unit that knew
 * only its IMEI its deviceId; the unit then restarts once, shortly after, so its topics change. */
void app_identity_note_config(const char *device_id);

/* {"imei":"...","mcuUid":"..."}: what the hardware says about itself, for `hello`. imei is left out when the unit has none. */
void app_identity_hardware_ids_json(char *out, size_t n, const char *imei);

/* USB command line, parsed in the runtime task: "ID", "ID SET <deviceId>", "ID CLEAR". Safe to call from the USB callback. */
void app_identity_usb_rx(const uint8_t *buf, uint32_t len);

/* Runs the USB commands and the one-time restart after the unit learns its deviceId. Call every pass of the runtime loop. */
void app_identity_service(uint32_t now_ms);

#endif
