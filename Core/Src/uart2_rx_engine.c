#include "uart2_rx_engine.h"
#include "app_debug_led.h"
#include "app_watchdog.h"
#include <string.h>

/* ===== UART2 RX ENGINE DEBUG =====
 * Set U2RX_DEBUG to 1 to enable verbose diagnostics.
 * Turn OFF once stable (printing inside RX path is expensive).
 */
#ifndef U2RX_DEBUG
#define U2RX_DEBUG 0
#endif

#if U2RX_DEBUG
#define U2RX_DBG_PRINT(...)  print(__VA_ARGS__)
#else
#define U2RX_DBG_PRINT(...)  do{}while(0)
#endif


static inline uint32_t rb_mask(uint32_t x) { return x % U2_RING_SZ; }

static void u2rx_ring_write(uart2_rx_t *u2, const uint8_t *src, uint32_t count)
{
    uint32_t start = rb_mask(u2->wr);
    uint32_t first = U2_RING_SZ - start;
    if (first > count) first = count;

    memcpy(&u2->ring[start], src, first);
    if (first < count) {
        memcpy(&u2->ring[0], src + first, count - first);
    }
    u2->wr += count;
}

void u2rx_init(uart2_rx_t *u2, UART_HandleTypeDef *huart, osSemaphoreId_t sem)
{
    memset(u2, 0, sizeof(*u2));
    u2->huart = huart;
    u2->rxSem = sem;
}

HAL_StatusTypeDef u2rx_start(uart2_rx_t *u2)
{
    if (!u2 || !u2->huart) return HAL_ERROR;

    HAL_StatusTypeDef st = HAL_UARTEx_ReceiveToIdle_DMA(u2->huart, u2->dma_buf, U2_DMA_RX_SZ);
    if (st != HAL_OK) return st;

    /* Reduce IRQ rate */
    if (u2->huart->hdmarx) {
        __HAL_DMA_DISABLE_IT(u2->huart->hdmarx, DMA_IT_HT);
    }
    return HAL_OK;
}

void u2rx_on_idle_event(uart2_rx_t *u2, uint16_t size)
{
    if (!u2 || size == 0U || size > U2_DMA_RX_SZ) return;


#if U2RX_DEBUG
    /* Quick visibility of incoming DMA chunk */
    U2RX_DBG_PRINT("[U2RX][IDLE] size=%u avail_before=%lu\r\n",
                   (unsigned)size, (unsigned long)u2rx_available(u2));
    /* Print ASCII (escaped) for small chunks */
    if (size <= 64) {
        U2RX_DBG_PRINT("[U2RX][IDLE] ASCII: ");
        for (uint16_t k = 0; k < size; k++) {
            uint8_t c = u2->dma_buf[k];
            if (c == '\r') U2RX_DBG_PRINT("\\r");
            else if (c == '\n') U2RX_DBG_PRINT("\\n");
            else if (c >= 32 && c <= 126) U2RX_DBG_PRINT("%c", c);
            else U2RX_DBG_PRINT(".");
        }
        U2RX_DBG_PRINT("\r\n");
    }
#endif

    uint16_t pos = (size == U2_DMA_RX_SZ) ? 0U : size;
    uint16_t last = u2->dma_pos;
    uint32_t count = 0U;

    if (pos > last) {
        count = (uint32_t)(pos - last);
    } else if (pos < last) {
        count = (uint32_t)(U2_DMA_RX_SZ - last) + pos;
    } else if (size == U2_DMA_RX_SZ) {
        count = U2_DMA_RX_SZ;
    }

    if (count > 0U) {
        uint32_t first = U2_DMA_RX_SZ - last;
        if (first > count) first = count;
        u2rx_ring_write(u2, &u2->dma_buf[last], first);
        if (first < count) {
            u2rx_ring_write(u2, &u2->dma_buf[0], count - first);
        }
    }
    u2->dma_pos = pos;

    if ((u2->wr - u2->rd) > (U2_RING_SZ - 16U)) {
        u2->rd = u2->wr - (U2_RING_SZ / 2U);
    }

    if (u2->rxSem) {
        osSemaphoreRelease(u2->rxSem);
    }
}

size_t u2rx_available(uart2_rx_t *u2)
{
    return (size_t)(u2->wr - u2->rd);
}

size_t u2rx_read(uart2_rx_t *u2, uint8_t *out, size_t max)
{
    size_t n = u2rx_available(u2);
    if (n > max) n = max;

    for (size_t i = 0; i < n; i++) {
        out[i] = u2->ring[rb_mask(u2->rd)];
#if U2RX_DEBUG
        /* Very selective debug to avoid flooding: print only control chars and 'O' 'K' */
        if (max == 1) {
            uint8_t cdbg = out[i];
            if (cdbg == '\n' || cdbg == '\r' || cdbg == 'O' || cdbg == 'K' || cdbg == '+') {
                U2RX_DBG_PRINT("[U2RX][POP] ch=0x%02X '%c' rd=%lu wr=%lu avail=%lu task=%s\r\n",
                               (unsigned)cdbg,
                               (cdbg >= 32 && cdbg <= 126) ? cdbg : '.',
                               (unsigned long)u2->rd,
                               (unsigned long)u2->wr,
                               (unsigned long)u2rx_available(u2),
                               osThreadGetName(osThreadGetId()));
            }
        }
#endif
        u2->rd++;
    }
    return n;
}

void u2rx_drop_all(uart2_rx_t *u2)
{
    u2->rd = u2->wr;
    u2->line_len = 0U;
    u2->line[0] = 0;
}

void u2rx_set_line_hook(uart2_rx_t *u2, u2rx_line_hook_t hook)
{
    if (u2) u2->line_hook = hook;
}

/* ---------------- line assembler ----------------
 * One assembler for every line-level reader. Before this, each reader kept
 * its own temporary buffer and threw it away on return or timeout, so an
 * unsolicited modem message (for example an MQTT downlink) that arrived
 * while another part of the firmware was reading was silently lost. */

static void u2rx_line_reset(uart2_rx_t *u2)
{
    u2->line_len = 0U;
    u2->line[0] = 0;
}

/* Returns 1 when u2->line holds a complete, non-empty line (no CR/LF). */
static uint8_t u2rx_line_feed(uart2_rx_t *u2, uint8_t ch)
{
    if (ch == '\r') return 0U;
    if (ch == '\n') {
        return (u2->line_len > 0U) ? 1U : 0U;
    }
    if (u2->line_len < (U2RX_LINE_MAX - 1U)) {
        u2->line[u2->line_len++] = (char)ch;
        u2->line[u2->line_len] = 0;
    }
    return 0U;
}

/* Offer a complete line to the hook. Returns 1 (and clears the line) when
 * the hook consumed it. */
static uint8_t u2rx_line_offer(uart2_rx_t *u2)
{
    if (!u2->line_hook || u2->in_hook) return 0U;
    u2->in_hook = 1U;
    uint8_t used = u2->line_hook(u2, u2->line);
    u2->in_hook = 0U;
    if (used) u2rx_line_reset(u2);
    return used;
}

/* On a reader timeout keep a partial line that may be the start of an
 * unsolicited message ('+...'), so the next reader completes it instead of
 * seeing its tail as garbage. Anything else (a '>' prompt) is dropped. */
static void u2rx_line_settle(uart2_rx_t *u2)
{
    if (u2->line_len > 0U && u2->line[0] != '+') {
        u2rx_line_reset(u2);
    }
}

static void u2rx_wait_data(uart2_rx_t *u2, uint32_t t0, uint32_t timeout_ms)
{
    uint32_t elapsed = HAL_GetTick() - t0;
    uint32_t remain = (elapsed < timeout_ms) ? (timeout_ms - elapsed) : 0U;
    uint32_t slice = (remain > 50U) ? 50U : remain;
    if (slice == 0U) return;
    if (u2->rxSem) (void)osSemaphoreAcquire(u2->rxSem, slice);
    else osDelay(1);
}

void u2rx_drain(uart2_rx_t *u2)
{
    if (!u2) return;
    uint8_t ch;
    while (u2rx_read(u2, &ch, 1U) == 1U) {
        if (u2rx_line_feed(u2, ch)) {
            if (!u2rx_line_offer(u2)) u2rx_line_reset(u2);
        }
    }
    u2rx_line_settle(u2);
}

/* Read a CRLF-terminated line into out (without CRLF). Returns length, or 0
 * on timeout. A partial line is returned on timeout only when it cannot be
 * the start of an unsolicited message (for example the "> " data prompt). */
int u2rx_read_line(uart2_rx_t *u2, char *out, int out_sz, uint32_t timeout_ms)
{
    if (!u2 || !out || out_sz <= 1) return 0;
    out[0] = 0;

    uint32_t t0 = HAL_GetTick();
    for (;;) {
        uint8_t ch;
        while (u2rx_read(u2, &ch, 1U) == 1U) {
            if (!u2rx_line_feed(u2, ch)) continue;
            if (u2rx_line_offer(u2)) continue;

            int n = (int)u2->line_len;
            if (n > out_sz - 1) n = out_sz - 1;
            memcpy(out, u2->line, (size_t)n);
            out[n] = 0;
            u2rx_line_reset(u2);
            U2RX_DBG_PRINT("[RL] line len=%d '%s'\r\n", n, out);
            return n;
        }

        if ((HAL_GetTick() - t0) >= timeout_ms) break;
        u2rx_wait_data(u2, t0, timeout_ms);
    }

    if (u2->line_len > 0U && u2->line[0] != '+') {
        int n = (int)u2->line_len;
        if (n > out_sz - 1) n = out_sz - 1;
        memcpy(out, u2->line, (size_t)n);
        out[n] = 0;
        u2rx_line_reset(u2);
        U2RX_DBG_PRINT("[RL] partial len=%d '%s'\r\n", n, out);
        return n;
    }
    return 0;
}

/* Wait until needle appears in the stream (moving window over bytes read by
 * this call). Complete lines seen on the way are offered to the line hook,
 * so unsolicited messages are captured rather than discarded. */
int u2rx_wait_for(uart2_rx_t *u2, const char *needle, uint32_t timeout_ms)
{
    if (!needle || !needle[0]) return 1;

    const size_t N = strlen(needle);
    char win[96];
    if (N >= sizeof(win)) return 0;

    memset(win, 0, sizeof(win));
    size_t wlen = 0;

    uint32_t t0 = HAL_GetTick();
    for (;;) {
        app_watchdog_mark_task(APP_WATCHDOG_TASK_RUNTIME);
        app_debug_led_run_once();
        app_watchdog_refresh();

        uint8_t ch;
        while (u2rx_read(u2, &ch, 1U) == 1U) {
            if (u2rx_line_feed(u2, ch)) {
                if (u2rx_line_offer(u2)) {
                    continue;
                }
                u2rx_line_reset(u2);
            }

            if (wlen < sizeof(win) - 1) {
                win[wlen++] = (char)ch;
                win[wlen] = 0;
            } else {
                memmove(win, win + 1, sizeof(win) - 2);
                win[sizeof(win) - 2] = (char)ch;
                win[sizeof(win) - 1] = 0;
            }

            if (wlen >= N && memcmp(&win[wlen - N], needle, N) == 0) {
                /* As before: what was read up to the match is consumed, the
                 * rest of the line stays in the ring for the next reader. */
                u2rx_line_reset(u2);
                return 1;
            }
        }

        if ((HAL_GetTick() - t0) >= timeout_ms) break;
        u2rx_wait_data(u2, t0, timeout_ms);
    }

    u2rx_line_settle(u2);
    return 0;
}

/* ---------------- raw helpers for line hooks ---------------- */

int u2rx_read_line_raw(uart2_rx_t *u2, char *out, int out_sz, uint32_t timeout_ms)
{
    if (!u2 || !out || out_sz <= 1) return 0;
    out[0] = 0;
    int idx = 0;
    uint32_t t0 = HAL_GetTick();
    for (;;) {
        uint8_t ch;
        while (u2rx_read(u2, &ch, 1U) == 1U) {
            if (ch == '\r') continue;
            if (ch == '\n') {
                if (idx == 0) continue;
                out[idx] = 0;
                return idx;
            }
            if (idx < out_sz - 1) out[idx++] = (char)ch;
        }
        if ((HAL_GetTick() - t0) >= timeout_ms) break;
        app_watchdog_mark_task(APP_WATCHDOG_TASK_RUNTIME);
        app_watchdog_refresh();
        u2rx_wait_data(u2, t0, timeout_ms);
    }
    out[idx] = 0;
    return 0;
}

int u2rx_read_exact_raw(uart2_rx_t *u2, uint8_t *dst, int want, uint32_t timeout_ms)
{
    if (!u2 || want <= 0) return 0;
    int got = 0;
    uint32_t t0 = HAL_GetTick();
    while (got < want) {
        size_t avail = u2rx_available(u2);
        if (avail > 0U) {
            size_t take = avail;
            if (take > (size_t)(want - got)) take = (size_t)(want - got);
            if (dst) {
                got += (int)u2rx_read(u2, dst + got, take);
            } else {
                uint8_t sink[64];
                if (take > sizeof(sink)) take = sizeof(sink);
                got += (int)u2rx_read(u2, sink, take);
            }
            continue;
        }
        if ((HAL_GetTick() - t0) >= timeout_ms) break;
        app_watchdog_mark_task(APP_WATCHDOG_TASK_RUNTIME);
        app_watchdog_refresh();
        u2rx_wait_data(u2, t0, timeout_ms);
    }
    return got;
}

/* Utility: wait for start_tag then copy bytes until end_tag into out */
int u2rx_find_and_copy_between(uart2_rx_t *u2,
                              const char *start_tag,
                              const char *end_tag,
                              char *out, int out_sz,
                              uint32_t timeout_ms)
{
    if (!out || out_sz <= 1) return 0;
    out[0] = 0;

    if (!u2rx_wait_for(u2, start_tag, timeout_ms)) return 0;

    uint32_t t0 = HAL_GetTick();
    int idx = 0;

    /* Now copy until end_tag seen */
    char tail[64] = {0};
    int  tail_len = 0;

    while ((HAL_GetTick() - t0) < timeout_ms) {
        if (u2rx_available(u2) == 0) {
            if (u2->rxSem) (void)osSemaphoreAcquire(u2->rxSem, 50);
            continue;
        }

        uint8_t ch;
        u2rx_read(u2, &ch, 1);

        /* track tail to detect end_tag */
        if (tail_len < (int)sizeof(tail) - 1) {
            tail[tail_len++] = (char)ch;
            tail[tail_len] = 0;
        } else {
            memmove(tail, tail + 1, sizeof(tail) - 2);
            tail[sizeof(tail) - 2] = (char)ch;
            tail[sizeof(tail) - 1] = 0;
        }

        if (idx < out_sz - 1) out[idx++] = (char)ch;
        out[idx] = 0;

        if (end_tag && strstr(tail, end_tag)) {
            return 1;
        }
    }

    return 0;
}
