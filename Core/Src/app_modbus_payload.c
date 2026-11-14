#include "app_modbus_payload.h"

#include "app_powerlytics_engine.h"

void app_modbus_payload_set_cfg(const modbus_cfg_t *cfg)
{
  app_powerlytics_engine_apply_config(cfg);
}

void app_modbus_payload_init(mbm_t *mbm)
{
  app_powerlytics_engine_init(mbm);
}

void app_modbus_payload_set_flash(W25Q_Handle *flash)
{
  app_powerlytics_engine_set_flash(flash);
}

void app_modbus_payload_sample_due(void)
{
  app_powerlytics_engine_sample_due();
}

void app_modbus_payload_flush_events(void)
{
  app_powerlytics_engine_flush_events();
}

void app_modbus_payload_di_edge_isr(uint16_t gpio_pin)
{
  app_powerlytics_engine_di_edge_isr(gpio_pin);
}

const char *app_build_payload_json_once(void)
{
  return app_powerlytics_engine_build_telemetry();
}

uint8_t app_modbus_payload_pop_event(char *payload, size_t payload_size)
{
  return app_powerlytics_engine_pop_event(payload, payload_size);
}

const char *app_modbus_payload_get_device_id(void)
{
  return app_powerlytics_engine_get_device_id();
}

const char *app_modbus_payload_get_config_id(void)
{
  return app_powerlytics_engine_get_config_id();
}

const char *app_modbus_payload_get_api_key(void)
{
  return app_powerlytics_engine_get_api_key();
}

uint8_t app_modbus_payload_has_valid_config(void)
{
  return app_powerlytics_engine_has_valid_config();
}

uint32_t app_modbus_payload_get_telemetry_interval_ms(void)
{
  return app_powerlytics_engine_get_telemetry_interval_ms();
}
