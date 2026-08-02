#ifndef COMM_MANAGER_H
#define COMM_MANAGER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    COMM_STATE_BOOT = 0,
    COMM_STATE_MODEM_SYNC,
    COMM_STATE_SIM_CHECK,
    COMM_STATE_NETWORK_WAIT,
    COMM_STATE_GPRS_ATTACH,
    COMM_STATE_MQTT_CONNECT,
    COMM_STATE_MQTT_SUBSCRIBE,
    COMM_STATE_ONLINE,
	COMM_STATE_MODEM_SOFT_RESET,
    COMM_STATE_BACKOFF,
    COMM_STATE_DEGRADED
} comm_state_t;

typedef enum {
    COMM_ERR_NONE = 0,
    COMM_ERR_AT_TIMEOUT,
    COMM_ERR_SIM_NOT_READY,
    COMM_ERR_NETWORK_NOT_REGISTERED,
    COMM_ERR_GPRS_FAILED,
    COMM_ERR_MQTT_CONNECT_FAILED,
    COMM_ERR_MQTT_SUBSCRIBE_FAILED,
    COMM_ERR_MQTT_PUBLISH_FAILED,
    COMM_ERR_MQTT_DISCONNECTED
} comm_error_t;

typedef struct {
    uint32_t at_fail_count;
    uint32_t sim_not_ready_count;
    uint32_t network_reg_fail_count;
    uint32_t gprs_fail_count;
    uint32_t mqtt_connect_fail_count;
    uint32_t mqtt_subscribe_fail_count;
    uint32_t mqtt_publish_fail_count;
    uint32_t mqtt_disconnect_count;
    uint32_t online_enter_count;
    uint32_t last_successful_publish_tick;
    uint32_t last_state_change_tick;
} comm_health_t;

void comm_manager_init(void);
void comm_manager_run_once(void);
uint8_t comm_manager_is_online(void);
void comm_manager_report_publish_result(uint8_t ok);
void comm_manager_report_mqtt_disconnected(void);
comm_state_t comm_manager_get_state(void);
comm_error_t comm_manager_get_last_error(void);
const comm_health_t *comm_manager_get_health(void);
const char *comm_manager_state_name(comm_state_t state);
const char *comm_manager_error_name(comm_error_t error);
#ifdef __cplusplus
}
#endif

#endif /* COMM_MANAGER_H */
