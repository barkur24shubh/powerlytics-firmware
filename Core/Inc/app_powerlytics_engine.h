#ifndef APP_POWERLYTICS_ENGINE_H
#define APP_POWERLYTICS_ENGINE_H

#include "modbus_config.h"
#include "modbus_rtu_master.h"
#include "w25q16.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void app_powerlytics_engine_init(mbm_t *mbm);
void app_powerlytics_engine_set_flash(W25Q_Handle *flash);
void app_powerlytics_engine_apply_config(const modbus_cfg_t *cfg);
void app_powerlytics_engine_sample_due(void);
void app_powerlytics_engine_flush_events(void);
void app_powerlytics_engine_di_edge_isr(uint16_t gpio_pin);

const char *app_powerlytics_engine_build_telemetry(void);
uint8_t app_powerlytics_engine_pop_event(char *payload, size_t payload_size);

const char *app_powerlytics_engine_get_device_id(void);
const char *app_powerlytics_engine_get_config_id(void);
const char *app_powerlytics_engine_get_config_hash(void);
const char *app_powerlytics_engine_get_api_key(void);
uint8_t app_powerlytics_engine_has_valid_config(void);
uint32_t app_powerlytics_engine_get_telemetry_interval_ms(void);
uint32_t app_powerlytics_engine_get_sample_interval_ms(void);

float app_powerlytics_calibrate_u32(uint32_t raw, float scale, float offset);
uint32_t app_powerlytics_counter_delta(uint32_t previous, uint32_t current, uint32_t rollover_at);

#ifdef __cplusplus
}
#endif

#endif /* APP_POWERLYTICS_ENGINE_H */
