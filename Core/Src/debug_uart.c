#include "debug_uart.h"
#include <string.h>
#include <stdio.h>     // <-- needed for vsnprintf
#include <stdarg.h>

static UART_HandleTypeDef *dbg_huart = NULL;
static debug_uart_sink_t dbg_sink = NULL;

#if defined(DEBUG)
static uint8_t dbg_uart_enabled = 1U;
#else
static uint8_t dbg_uart_enabled = 0U;
#endif

void debug_uart_init(UART_HandleTypeDef *huart) {
    dbg_huart = huart;
}

void debug_uart_set_sink(debug_uart_sink_t sink) {
    dbg_sink = sink;
}

void debug_uart_set_enabled(uint8_t enabled) {
    dbg_uart_enabled = enabled ? 1U : 0U;
}

uint8_t debug_uart_is_enabled(void) {
    return dbg_uart_enabled;
}

void print(const char *fmt, ...) {
    if (!dbg_huart && !dbg_sink) return;

    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (dbg_huart && dbg_uart_enabled) {
        HAL_UART_Transmit(dbg_huart, (uint8_t*)buf, strlen(buf), 1000);
    }

    if (dbg_sink) {
        dbg_sink(buf);
    }
}
