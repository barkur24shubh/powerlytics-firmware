#ifndef MODEM_HANDLER_H
#define MODEM_HANDLER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h> // Required for the new bool parameter
#include "w25q16.h"

/* Public API */
void     WakeUp_GSM(void);

/**
 * @brief Sends an AT command and waits for a specific response.
 * @param command: The AT command string (e.g. "AT")
 * @param expected: The substring to look for (e.g. "OK")
 * @param timeout: Time to wait in milliseconds
 * @param flush_rx: If true, clears the buffer before sending.
 * Set to false when expecting a URC immediately after a command response.
 */
uint8_t  SIM800_SendATCommand2(const char *command, const char *expected, uint32_t timeout, bool flush_rx);

uint8_t  SendCommandWithRetry(const char *cmd, const char *expected, uint16_t timeout, uint8_t retries);

void ModemAT_InitMutex(void);

uint8_t  modem_at_rx_is_active(void);
void     modem_at_rx_feed(const uint8_t *data, uint16_t len);

/* GPRS bring-up with APN from config */
uint8_t  GPRS_Init(const char *apn);

void     GetIMEI(void);
void     WaitForNetworkAndBlinkLED(void);
void     Check_Internet_Service(void);
uint8_t modem_get_signal_quality(int32_t *rssi, int32_t *ber);
uint8_t modem_get_network_time_utc(char *out_iso_utc, uint32_t out_len);
uint8_t Modem_SoftReset(void);

typedef void (*modem_http_download_progress_cb_t)(uint32_t downloaded,
                                                  uint32_t total,
                                                  void *ctx);

uint8_t modem_http_download_to_w25q(const char *url,
                                    W25Q_Handle *flash,
                                    uint32_t base_addr,
                                    uint32_t max_size,
                                    uint32_t expected_size,
                                    const char *expected_sha256,
                                    uint32_t *out_size,
                                    char out_sha256[65],
                                    modem_http_download_progress_cb_t progress_cb,
                                    void *progress_ctx);
uint8_t modem_tcp_http_download_to_w25q(const char *url,
                                        W25Q_Handle *flash,
                                        uint32_t base_addr,
                                        uint32_t max_size,
                                        uint32_t expected_size,
                                        const char *expected_sha256,
                                        uint32_t *out_size,
                                        char out_sha256[65],
                                        modem_http_download_progress_cb_t progress_cb,
                                        void *progress_ctx);
const char *modem_http_download_last_error(void);
#ifdef __cplusplus
}
#endif

#endif /* MODEM_HANDLER_H */
