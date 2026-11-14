#ifndef APP_MODBUS_PAYLOAD_H
#define APP_MODBUS_PAYLOAD_H

#include "stm32f4xx_hal.h"
#include "modbus_rtu_master.h"
#include "modbus_config.h"
#include "w25q16.h"
#include <stddef.h>

void app_modbus_payload_set_cfg(const modbus_cfg_t *cfg);
void app_modbus_payload_set_flash(W25Q_Handle *flash);

void app_modbus_payload_init(mbm_t *mbm);
void app_modbus_payload_sample_due(void);
void app_modbus_payload_flush_events(void);
void app_modbus_payload_di_edge_isr(uint16_t gpio_pin);
const char* app_build_payload_json_once(void);
uint8_t app_modbus_payload_pop_event(char *payload, size_t payload_size);

const char* app_modbus_payload_get_device_id(void);
const char* app_modbus_payload_get_config_id(void);
const char* app_modbus_payload_get_api_key(void);
uint8_t app_modbus_payload_has_valid_config(void);
uint32_t app_modbus_payload_get_telemetry_interval_ms(void);

#endif
