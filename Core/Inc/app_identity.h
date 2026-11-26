#ifndef APP_IDENTITY_H
#define APP_IDENTITY_H

/* The unit's identity at run time (firmware 1.0.21). The rules and the pure logic are in app_identity_core.h.
 *
 * The deviceId is written into flash ONCE, when the unit is programmed, and read from there for the rest of its life.
 * It survives OTA updates and firmware changes (the slots are outside the image, the configuration and the OTA
 * backup). There is no command to change or clear it: re-provisioning a unit means erasing the external flash chip. */

#include <stddef.h>
#include <stdint.h>
#include "app_identity_core.h"
#include "w25q16.h"

/* Flash (external W25Q16): two 4 KB slots in the free gap after the configuration slots (0x060000..0x067FFF) and before
 * the OTA backup (0x080000). The newest valid copy wins, so a power cut during the one write keeps a good copy. */
#define APP_ID_SLOT_A_ADDR 0x06C000UL
#define APP_ID_SLOT_B_ADDR 0x06D000UL

void app_identity_set_flash(W25Q_Handle *flash);

/* Reads the record from flash. Call once after the flash is initialised, before the configuration is applied. */
void app_identity_load(void);

/* The deviceId, or "" when the unit has not been provisioned (it then stays off the network). */
const char *app_identity_device_id(void);

/* The MQTT client id: the deviceId. "" when not provisioned. */
void app_identity_client_id(char *out, size_t n);

/* An applied configuration says which unit it is for. A different id than ours is logged, never adopted. */
void app_identity_note_config(const char *device_id);

/* {"imei":"...","mcuUid":"..."}: what the hardware says about itself, for `hello`. Attributes, never identity. */
void app_identity_hardware_ids_json(char *out, size_t n, const char *imei);

/* USB command line, parsed in the runtime task: "ID" (status) and "ID SET <deviceId>" (once). Safe from the USB callback. */
void app_identity_usb_rx(const uint8_t *buf, uint32_t len);

/* Runs the USB commands. Call every pass of the runtime loop. */
void app_identity_service(uint32_t now_ms);

#endif
