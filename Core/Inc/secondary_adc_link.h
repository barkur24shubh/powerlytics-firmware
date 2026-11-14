#ifndef SECONDARY_ADC_LINK_H
#define SECONDARY_ADC_LINK_H

#include "stm32f4xx_hal.h"

#include <stdint.h>

#define SECONDARY_ADC_PROTOCOL_VERSION 1U
#define SECONDARY_ADC_MAX_CHANNELS     11U

typedef struct {
  uint32_t sequence;
  uint32_t source_tick_ms;
  uint16_t raw[SECONDARY_ADC_MAX_CHANNELS];
  uint16_t status_flags;
  uint8_t channel_count;
} secondary_adc_sample_t;

typedef struct {
  uint32_t valid_frames;
  uint32_t crc_errors;
  uint32_t format_errors;
  uint32_t sequence_gaps;
  uint32_t queue_overflows;
  uint32_t uart_errors;
} secondary_adc_link_stats_t;

uint8_t secondary_adc_link_start(UART_HandleTypeDef *uart);
uint8_t secondary_adc_link_pop(secondary_adc_sample_t *sample);
void secondary_adc_link_get_stats(secondary_adc_link_stats_t *stats);
void secondary_adc_link_uart_rx_event_isr(UART_HandleTypeDef *uart, uint16_t size);
void secondary_adc_link_uart_rx_complete_isr(UART_HandleTypeDef *uart);
void secondary_adc_link_uart_error_isr(UART_HandleTypeDef *uart);

#endif
