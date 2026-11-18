#ifndef APP_BOARD_IO_H
#define APP_BOARD_IO_H

#include "stm32f4xx_hal.h"
#include <stdint.h>

void app_board_io_init(ADC_HandleTypeDef *hadc);

/* DI */
void app_di_read_raw_all(uint8_t out8[8]);
void app_di_read_all(uint8_t out8[8]);
int8_t app_di_index_from_gpio_pin(uint16_t gpio_pin);

/* ADC */
void app_adc_set_secondary_uart(UART_HandleTypeDef *huart);
void app_adc_read_all_mv(uint32_t out10_mv[10]);
uint8_t app_adc_timed_sampler_start(void);
uint8_t app_adc_timed_sampler_is_running(void);
uint8_t app_adc_pop_sample_mv(uint32_t out10_mv[10]);
uint32_t app_adc_timed_sampler_total_samples(void);
uint32_t app_adc_timed_sampler_dropped_samples(void);
uint8_t app_power_monitor_get(uint16_t *raw, uint32_t *sense_mv, uint32_t *supply_mv);
void app_adc_dma_conv_cplt_isr(ADC_HandleTypeDef *hadc);

/* Relay (optional) */
void app_relay_test(uint32_t step_ms);

void app_relay_test_init(void);
void app_relay_test_sequence(void);
uint8_t app_relay_set_by_key(const char *port_key, uint8_t on);
#ifndef APP_RELAY_MIN_SWITCH_MS
#define APP_RELAY_MIN_SWITCH_MS 500U
#endif
uint8_t app_relay_command(const char *port_key, uint8_t on, const char **reason);
void    app_relay_apply_safe_states(uint16_t on_mask, const char *why);
uint8_t app_relay_get_by_key(const char *port_key);
void app_relay_get_all(uint8_t out5[5]);
uint8_t app_relay_feedback_get_by_key(const char *port_key);
void app_relay_feedback_debug_print(void);
#endif
