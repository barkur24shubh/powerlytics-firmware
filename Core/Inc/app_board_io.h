#ifndef APP_BOARD_IO_H
#define APP_BOARD_IO_H

#include "stm32f4xx_hal.h"
#include <stdint.h>

void app_board_io_init(ADC_HandleTypeDef *hadc);

/* DI */
void app_di_read_all(uint8_t out8[8]);

/* ADC */
void app_adc_read_all_mv(uint32_t out10_mv[10]);

/* Relay (optional) */
void app_relay_test(uint32_t step_ms);

void app_relay_test_init(void);
void app_relay_test_sequence(void);
uint8_t app_relay_set_by_key(const char *port_key, uint8_t on);
uint8_t app_relay_get_by_key(const char *port_key);
void app_relay_get_all(uint8_t out5[5]);
#endif
