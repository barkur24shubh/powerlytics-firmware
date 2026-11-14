#include "debug_uart.h"
#include "usb_device.h"
#include "usbd_cdc_if.h"
#include <string.h>
#include <stdio.h>     // <-- needed for vsnprintf
#include <stdarg.h>

static UART_HandleTypeDef *dbg_huart = NULL;
static debug_uart_sink_t dbg_sink = NULL;
static uint32_t dbg_force_enabled_until = 0U;
static uint8_t dbg_at_trace_enabled = 0U;
static volatile uint8_t dbg_usb_writing = 0U;

static void debug_usb_write(const char *text, uint16_t length) {
    if (!text || (length == 0U) ||
        (hUsbDeviceFS.dev_state != USBD_STATE_CONFIGURED)) {
        return;
    }

    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    if (dbg_usb_writing) {
        if (!primask) {
            __enable_irq();
        }
        return;
    }
    dbg_usb_writing = 1U;
    if (!primask) {
        __enable_irq();
    }

    /* Debug output must never block a firmware task when USB is busy. */
    (void)CDC_Transmit_FS((uint8_t *)text, length);

    dbg_usb_writing = 0U;
}

#if defined(DEBUG)
static uint8_t dbg_uart_enabled = 1U;
#else
static uint8_t dbg_uart_enabled = 1U;
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

void debug_uart_set_at_enabled(uint8_t enabled) {
    dbg_at_trace_enabled = enabled ? 1U : 0U;
}

void debug_uart_force_enabled_for(uint32_t duration_ms) {
    dbg_force_enabled_until = HAL_GetTick() + duration_ms;
}

uint8_t debug_uart_is_enabled(void) {
    return dbg_uart_enabled;
}

uint8_t debug_uart_at_is_enabled(void) {
    return dbg_at_trace_enabled;
}

void print(const char *fmt, ...) {
    if (!dbg_huart && !dbg_sink &&
        (hUsbDeviceFS.dev_state != USBD_STATE_CONFIGURED)) return;
    if (!fmt) return;

    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    uint8_t force_enabled = (HAL_GetTick() < dbg_force_enabled_until) ? 1U : 0U;

    if (dbg_huart && (dbg_uart_enabled || force_enabled)) {
        HAL_UART_Transmit(dbg_huart, (uint8_t*)buf, strlen(buf), 1000);
    }

    if (dbg_uart_enabled || force_enabled) {
        debug_usb_write(buf, (uint16_t)strlen(buf));
    }

    if (dbg_sink) {
        dbg_sink(buf);
    }
}
