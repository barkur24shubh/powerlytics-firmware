#include "modem_urc.h"
#include "debug_uart.h"
#include "app_log.h"
#include "main.h"

#include <stdio.h>
#include <string.h>

/* Time allowed for the next header line inside a downlink block. */
#define MODEM_URC_LINE_TIMEOUT_MS   3000UL
/* Payload chunk: base + per-byte allowance (115200 baud is ~11.5 B/ms). */
#define MODEM_URC_CHUNK_BASE_MS     2000UL
#define MODEM_URC_CHUNK_PER_KB_MS   500UL
#define MODEM_URC_MAX_BLOCK_LINES   64

/* Byte ring of records: [len lo][len hi][payload bytes]. Single producer
 * (whichever reader hit the block, always under the AT mutex or the runtime
 * task) and single consumer (the runtime task). */
static uint8_t           s_q[MODEM_URC_QUEUE_BYTES];
static volatile uint32_t s_q_head;   /* write position (monotonic) */
static volatile uint32_t s_q_tail;   /* read position (monotonic) */
static volatile uint32_t s_flags;
static uint32_t          s_rx_count;
static uint32_t          s_dropped;

static uint32_t q_used(void) { return s_q_head - s_q_tail; }
static uint32_t q_free(void) { return MODEM_URC_QUEUE_BYTES - q_used(); }

static void q_put_byte(uint32_t pos, uint8_t b) { s_q[pos % MODEM_URC_QUEUE_BYTES] = b; }
static uint8_t q_get_byte(uint32_t pos) { return s_q[pos % MODEM_URC_QUEUE_BYTES]; }

static uint32_t chunk_timeout_ms(int n)
{
    uint32_t t = MODEM_URC_CHUNK_BASE_MS + (((uint32_t)n * MODEM_URC_CHUNK_PER_KB_MS) / 1024UL);
    return (t > 15000UL) ? 15000UL : t;
}

/* Read n bytes from the modem into the queue at pos (handles wrap), or into
 * nothing when pos is UINT32_MAX. Returns bytes read. */
static int q_fill_from_uart(uart2_rx_t *u2, uint32_t pos, int n, uint32_t timeout_ms)
{
    if (pos == UINT32_MAX) {
        return u2rx_read_exact_raw(u2, NULL, n, timeout_ms);
    }
    uint32_t off = pos % MODEM_URC_QUEUE_BYTES;
    int first = (int)(MODEM_URC_QUEUE_BYTES - off);
    if (first > n) first = n;
    int got = u2rx_read_exact_raw(u2, &s_q[off], first, timeout_ms);
    if (got == first && first < n) {
        got += u2rx_read_exact_raw(u2, &s_q[0], n - first, timeout_ms);
    }
    return got;
}

static int parse_last_int(const char *line)
{
    const char *p = strrchr(line, ',');
    if (!p) p = strchr(line, ':');
    if (!p) return -1;
    p++;
    while (*p == ' ') p++;
    int v = 0;
    uint8_t any = 0U;
    while (*p >= '0' && *p <= '9') {
        v = (v * 10) + (*p - '0');
        p++;
        any = 1U;
    }
    return any ? v : -1;
}

static void note_drop(const char *reason, int len)
{
    s_dropped++;
    print("[MQTT RX][ERR] downlink dropped reason=%s len=%d dropped=%lu\r\n",
          reason, len, (unsigned long)s_dropped);
    app_log_push(APP_LOG_WARN, "MQTT", "DOWNLINK_DROPPED", reason, len, (int32_t)s_dropped);
}

/* Called with the "+CMQTTRXSTART:" line already taken off the ring. */
static void capture_block(uart2_rx_t *u2, const char *start_line)
{
    int client = 0, topic_total = 0, payload_total = 0;
    if (sscanf(start_line, "+CMQTTRXSTART: %d,%d,%d", &client, &topic_total, &payload_total) != 3 ||
        payload_total < 0) {
        print("[MQTT RX][ERR] bad block header\r\n");
        payload_total = -1;
    }

    uint8_t store = 0U;
    if (payload_total > 0) {
        if ((uint32_t)payload_total + 2U > MODEM_URC_QUEUE_BYTES - 1U) {
            note_drop("too_large", payload_total);
        } else if (q_free() < (uint32_t)payload_total + 2U) {
            note_drop("queue_full", payload_total);
        } else {
            store = 1U;
        }
    }

    uint32_t base = s_q_head;          /* record header goes here */
    uint32_t wpos = base + 2U;         /* payload bytes go here */
    int stored = 0;
    char hdr[64];

    for (int i = 0; i < MODEM_URC_MAX_BLOCK_LINES; i++) {
        int n = u2rx_read_line_raw(u2, hdr, (int)sizeof(hdr), MODEM_URC_LINE_TIMEOUT_MS);
        if (n <= 0) {
            if (store) note_drop("timeout", payload_total);
            return;
        }

        if (strncmp(hdr, "+CMQTTRXTOPIC:", 14) == 0) {
            int tn = parse_last_int(hdr);
            if (tn > 0 && u2rx_read_exact_raw(u2, NULL, tn, chunk_timeout_ms(tn)) != tn) {
                if (store) note_drop("topic_timeout", payload_total);
                return;
            }
            continue;
        }

        if (strncmp(hdr, "+CMQTTRXPAYLOAD:", 16) == 0) {
            int pn = parse_last_int(hdr);
            if (pn <= 0) continue;
            uint8_t keep = (store && (stored + pn) <= payload_total) ? 1U : 0U;
            if (store && !keep) {
                note_drop("length_mismatch", payload_total);
                store = 0U;
            }
            int got = q_fill_from_uart(u2, keep ? (wpos + (uint32_t)stored) : UINT32_MAX,
                                       pn, chunk_timeout_ms(pn));
            if (got != pn) {
                if (store) note_drop("payload_timeout", payload_total);
                return;
            }
            if (keep) stored += pn;
            continue;
        }

        if (strncmp(hdr, "+CMQTTRXEND:", 12) == 0) {
            if (!store) return;
            if (stored != payload_total) {
                note_drop("short_payload", payload_total);
                return;
            }
            q_put_byte(base, (uint8_t)(payload_total & 0xFF));
            q_put_byte(base + 1U, (uint8_t)((payload_total >> 8) & 0xFF));
            __DMB();
            s_q_head = wpos + (uint32_t)payload_total;
            s_rx_count++;
            print("[MQTT RX] downlink queued len=%d pending_bytes=%lu total=%lu\r\n",
                  payload_total, (unsigned long)q_used(), (unsigned long)s_rx_count);
            return;
        }

        /* The modem does not interleave other output inside a block; if it
         * ever does, keep reading until the end marker. */
        print("[MQTT RX][WARN] unexpected line inside downlink block len=%d\r\n", n);
    }

    if (store) note_drop("no_end", payload_total);
}

static uint8_t modem_urc_line_hook(uart2_rx_t *u2, const char *line)
{
    if (line[0] != '+') return 0U;

    if (strncmp(line, "+CMQTTRXSTART:", 14) == 0) {
        capture_block(u2, line);
        return 1U;
    }

    /* Fragments of a block whose start was lost (e.g. after a reader timed
     * out mid-block): skip their bytes so they are not parsed as AT output. */
    if (strncmp(line, "+CMQTTRXTOPIC:", 14) == 0 ||
        strncmp(line, "+CMQTTRXPAYLOAD:", 16) == 0) {
        int n = parse_last_int(line);
        if (n > 0) (void)u2rx_read_exact_raw(u2, NULL, n, chunk_timeout_ms(n));
        print("[MQTT RX][WARN] orphan downlink fragment skipped len=%d\r\n", n);
        return 1U;
    }
    if (strncmp(line, "+CMQTTRXEND:", 12) == 0) {
        return 1U;
    }

    /* Disconnect notices: latch, but let the line flow on to the reader. */
    if (strstr(line, "+CMQTTCONNLOST")) {
        s_flags |= MODEM_URC_FLAG_MQTT_CONN_LOST;
    } else if (strstr(line, "+PDP DEACT")) {
        s_flags |= MODEM_URC_FLAG_PDP_DEACT;
    } else if (strstr(line, "+CGEV: ME PDN DEACT")) {
        s_flags |= MODEM_URC_FLAG_PDN_DEACT;
    }
    return 0U;
}

void modem_urc_init(uart2_rx_t *u2)
{
    s_q_head = 0U;
    s_q_tail = 0U;
    s_flags = 0U;
    u2rx_set_line_hook(u2, modem_urc_line_hook);
}

uint8_t modem_urc_downlink_pending(void)
{
    return (s_q_head != s_q_tail) ? 1U : 0U;
}

uint8_t modem_urc_pop_downlink(char *out, size_t out_sz, size_t *out_len)
{
    if (out_len) *out_len = 0U;
    if (!out || out_sz < 2U) return 0U;

    while (s_q_head != s_q_tail) {
        uint32_t tail = s_q_tail;
        uint32_t len = (uint32_t)q_get_byte(tail) | ((uint32_t)q_get_byte(tail + 1U) << 8);
        uint32_t next = tail + 2U + len;

        if (len + 1U > out_sz) {
            s_q_tail = next;
            note_drop("reader_buffer_small", (int)len);
            continue;
        }
        for (uint32_t i = 0; i < len; i++) {
            out[i] = (char)q_get_byte(tail + 2U + i);
        }
        out[len] = 0;
        __DMB();
        s_q_tail = next;
        if (out_len) *out_len = len;
        return 1U;
    }
    return 0U;
}

uint32_t modem_urc_downlink_count(void) { return s_rx_count; }
uint32_t modem_urc_dropped_count(void) { return s_dropped; }

uint32_t modem_urc_take_disconnect_flags(void)
{
    uint32_t f = s_flags;
    s_flags = 0U;
    return f;
}

void modem_urc_clear_disconnect_flags(void)
{
    s_flags = 0U;
}
