#ifndef DEBUG_UART_H
#define DEBUG_UART_H

#include "stm32f4xx_hal.h"
#include <stdarg.h>

typedef void (*debug_uart_sink_t)(const char *text);

void debug_uart_init(UART_HandleTypeDef *huart);
void debug_uart_set_sink(debug_uart_sink_t sink);
void debug_uart_set_enabled(uint8_t enabled);
uint8_t debug_uart_is_enabled(void);
void print(const char *fmt, ...);   // <-- variadic now

#endif
