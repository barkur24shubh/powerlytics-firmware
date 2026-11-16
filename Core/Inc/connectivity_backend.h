#ifndef CONNECTIVITY_BACKEND_H
#define CONNECTIVITY_BACKEND_H

#include "connectivity.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    void (*init)(void);
    void (*run_once)(void);
    uint8_t (*is_online)(void);
    uint8_t (*publish)(const char *topic, const char *payload);
    uint8_t (*publish_priority)(const char *topic, const char *payload);
    uint8_t (*publish_telemetry)(const char *payload);
    char *(*read_downlink)(void);
    void (*handle_downlink)(const char *payload);
    uint8_t (*check_disconnect_urc)(void);
    uint8_t (*maintain)(uint32_t keepalive_ms,
                        const char *topic,
                        const char *ping_payload,
                        uint8_t *attempted);
    void (*report_publish_result)(uint8_t ok);
    void (*report_disconnected)(void);
    uint8_t (*last_publish_attempted)(void);
    const char *(*last_publish_stage)(void);
    void (*set_ota_flash)(W25Q_Handle *flash);
    uint8_t (*ota_is_busy)(void);
    uint8_t (*report_ota_boot_result_once)(void);
    uint8_t (*poll_signal_quality)(int32_t *rssi, int32_t *ber);
    uint8_t (*flush_pending_acks)(void);
    uint32_t (*publish_gap_remaining_ms)(void);
    uint8_t (*sync_clock)(void);
    connectivity_state_t (*get_state)(void);
    connectivity_error_t (*get_last_error)(void);
    const connectivity_health_t *(*get_health)(void);
    const char *(*state_name)(connectivity_state_t state);
    const char *(*error_name)(connectivity_error_t error);
} connectivity_backend_t;

const connectivity_backend_t *connectivity_cellular_backend(void);
const connectivity_backend_t *connectivity_ethernet_backend(void);
const connectivity_backend_t *connectivity_wifi_backend(void);

#ifdef __cplusplus
}
#endif

#endif /* CONNECTIVITY_BACKEND_H */
