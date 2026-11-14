#ifndef BOARD_DEFS_H
#define BOARD_DEFS_H

#include "stm32f4xx_hal.h"

#include <stdint.h>

#define BOARD_CONNECTIVITY_CELLULAR  (1UL << 0)
#define BOARD_CONNECTIVITY_WIFI      (1UL << 1)
#define BOARD_CONNECTIVITY_ETHERNET  (1UL << 2)

typedef struct {
  GPIO_TypeDef *port;
  uint16_t pin;
} board_gpio_pin_t;

typedef struct {
  GPIO_TypeDef *port;
  uint16_t pin;
  GPIO_PinState initial_state;
} board_gpio_output_t;

typedef struct {
  GPIO_TypeDef *port;
  uint16_t pin;
  uint32_t adc_channel;
  float board_scale;
  const char *name;
} board_adc_channel_t;

#endif /* BOARD_DEFS_H */
