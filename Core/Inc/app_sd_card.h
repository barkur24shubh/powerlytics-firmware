#ifndef APP_SD_CARD_H
#define APP_SD_CARD_H

#include <stdint.h>

uint8_t app_sd_self_test_once(void);
uint8_t app_sd_is_available(void);
uint8_t app_sd_append_telemetry_payload(const char *payload);
void app_sd_telemetry_service(void);
void app_sd_terminal_log_sink(const char *text);
void app_sd_terminal_log_enable(uint8_t enabled);
void app_sd_terminal_log_service(void);

#endif
