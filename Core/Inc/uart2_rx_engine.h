#pragma once
#include "main.h"
#include "cmsis_os.h"
#include <stdint.h>
#include <stddef.h>

#ifndef U2_DMA_RX_SZ
#define U2_DMA_RX_SZ (1024 * 4)
#endif

#ifndef U2_RING_SZ
#define U2_RING_SZ     8192
#endif

#ifndef U2RX_LINE_MAX
#define U2RX_LINE_MAX  256
#endif

struct uart2_rx_s;

/*
 * Line hook: called with every complete, non-empty line that any line-level
 * reader (u2rx_read_line, u2rx_wait_for, u2rx_drain) takes off the ring.
 * Return 1 when the hook consumed the line (and possibly the bytes that
 * follow it, read with the *_raw helpers); the reader then acts as if the
 * line never arrived. This is how unsolicited modem messages (MQTT downlinks,
 * disconnect notices) survive AT exchanges instead of being discarded.
 */
typedef uint8_t (*u2rx_line_hook_t)(struct uart2_rx_s *u2, const char *line);

typedef struct uart2_rx_s {
    UART_HandleTypeDef *huart;
    uint8_t  dma_buf[U2_DMA_RX_SZ];

    uint8_t  ring[U2_RING_SZ];
    volatile uint32_t wr;     // write index
    volatile uint32_t rd;     // read index
    volatile uint16_t dma_pos; // last consumed position in circular DMA buffer

    osSemaphoreId_t rxSem;    // released on RX idle event

    /* Line assembler shared by all line-level readers, so a line split
     * across two reads (or two different readers) is never cut in half. */
    char     line[U2RX_LINE_MAX];
    uint16_t line_len;
    u2rx_line_hook_t line_hook;
    uint8_t  in_hook;
} uart2_rx_t;

void     u2rx_init(uart2_rx_t *u2, UART_HandleTypeDef *huart, osSemaphoreId_t sem);
HAL_StatusTypeDef u2rx_start(uart2_rx_t *u2);
void     u2rx_on_idle_event(uart2_rx_t *u2, uint16_t size);

size_t   u2rx_available(uart2_rx_t *u2);
size_t   u2rx_read(uart2_rx_t *u2, uint8_t *out, size_t max);
void     u2rx_drop_all(uart2_rx_t *u2);
void     u2rx_set_line_hook(uart2_rx_t *u2, u2rx_line_hook_t hook);

/* Pass every byte already received through the line assembler and hook,
 * discarding lines nobody claimed. Use this instead of u2rx_drop_all before
 * an AT command so pending unsolicited messages are not thrown away. */
void     u2rx_drain(uart2_rx_t *u2);

/* Raw helpers for line hooks: bypass the line assembler and the hook. */
int      u2rx_read_line_raw(uart2_rx_t *u2, char *out, int out_sz, uint32_t timeout_ms);
int      u2rx_read_exact_raw(uart2_rx_t *u2, uint8_t *dst, int want, uint32_t timeout_ms);

int      u2rx_read_line(uart2_rx_t *u2, char *out, int out_sz, uint32_t timeout_ms);
int      u2rx_wait_for(uart2_rx_t *u2, const char *needle, uint32_t timeout_ms);

/* Helpers */
int      u2rx_find_and_copy_between(uart2_rx_t *u2,
                                   const char *start_tag,
                                   const char *end_tag,
                                   char *out, int out_sz,
                                   uint32_t timeout_ms);
