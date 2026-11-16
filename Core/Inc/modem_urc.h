#ifndef MODEM_URC_H
#define MODEM_URC_H

/*
 * Unsolicited modem message (URC) demultiplexer for the SIMCom MQTT stack.
 *
 * Every line-level reader of the modem serial stream offers each complete
 * line to this module first (see u2rx_set_line_hook). When a downlink block
 *
 *   +CMQTTRXSTART: 0,<topic_len>,<payload_len>
 *   +CMQTTRXTOPIC: 0,<n>      <n topic bytes>
 *   +CMQTTRXPAYLOAD: 0,<n>    <n payload bytes>   (may repeat)
 *   +CMQTTRXEND: 0
 *
 * starts, the whole block is read and its payload queued, no matter which
 * part of the firmware happened to be reading (an AT command, a publish, a
 * signal poll). Disconnect notices are latched as flags. Before this,
 * whatever reader saw a downlink first discarded it, so commands sent while
 * the gateway was busy were lost.
 *
 * Product-generic: it only knows the modem's MQTT framing, not payload types.
 */

#include <stddef.h>
#include <stdint.h>
#include "uart2_rx_engine.h"

#define MODEM_URC_FLAG_MQTT_CONN_LOST  0x01UL
#define MODEM_URC_FLAG_PDP_DEACT       0x02UL
#define MODEM_URC_FLAG_PDN_DEACT       0x04UL

#ifndef MODEM_URC_QUEUE_BYTES
#define MODEM_URC_QUEUE_BYTES  (10U * 1024U)
#endif

void     modem_urc_init(uart2_rx_t *u2);

/* Queued downlinks, oldest first. Returns 1 and copies one payload
 * (NUL-terminated) when one is pending. */
uint8_t  modem_urc_downlink_pending(void);
uint8_t  modem_urc_pop_downlink(char *out, size_t out_sz, size_t *out_len);
uint32_t modem_urc_downlink_count(void);
uint32_t modem_urc_dropped_count(void);

/* Disconnect notices seen since the last take/clear. */
uint32_t modem_urc_take_disconnect_flags(void);
void     modem_urc_clear_disconnect_flags(void);

#endif /* MODEM_URC_H */
