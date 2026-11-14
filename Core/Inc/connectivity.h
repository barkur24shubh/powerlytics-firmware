#ifndef CONNECTIVITY_H
#define CONNECTIVITY_H

#include "comm_manager.h"
#include "w25q16.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Keep the public health/state shape stable while the cellular backend is migrated. */
typedef comm_state_t connectivity_state_t;
typedef comm_error_t connectivity_error_t;
typedef comm_health_t connectivity_health_t;

void connectivity_init(void);
void connectivity_run_once(void);
uint8_t connectivity_is_online(void);

uint8_t connectivity_publish(const char *topic, const char *payload);
uint8_t connectivity_publish_priority(const char *topic, const char *payload);
uint8_t connectivity_publish_telemetry(const char *payload);
char *connectivity_read_downlink(void);
void connectivity_handle_downlink(const char *payload);

uint8_t connectivity_check_disconnect_urc(void);
uint8_t connectivity_maintain(uint32_t keepalive_ms,
                              const char *topic,
                              const char *ping_payload,
                              uint8_t *attempted);

void connectivity_report_publish_result(uint8_t ok);
void connectivity_report_disconnected(void);
uint8_t connectivity_last_publish_attempted(void);
const char *connectivity_last_publish_stage(void);

void connectivity_set_ota_flash(W25Q_Handle *flash);
uint8_t connectivity_ota_is_busy(void);
uint8_t connectivity_report_ota_boot_result_once(void);

uint8_t connectivity_poll_signal_quality(int32_t *rssi, int32_t *ber);

connectivity_state_t connectivity_get_state(void);
connectivity_error_t connectivity_get_last_error(void);
const connectivity_health_t *connectivity_get_health(void);
const char *connectivity_state_name(connectivity_state_t state);
const char *connectivity_error_name(connectivity_error_t error);
const char *connectivity_backend_name(void);

#ifdef __cplusplus
}
#endif

#endif /* CONNECTIVITY_H */
