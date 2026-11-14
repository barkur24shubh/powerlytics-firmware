#include "connectivity_backend.h"

#include "comm_manager.h"
#include "modem_handler.h"
#include "mqtt_handler.h"

static void cellular_init(void)
{
    comm_manager_init();
}

static void cellular_run_once(void)
{
    comm_manager_run_once();
}

static uint8_t cellular_is_online(void)
{
    return comm_manager_is_online();
}

static uint8_t cellular_publish(const char *topic, const char *payload)
{
    return MQTT_Publish(topic, payload);
}

static uint8_t cellular_publish_priority(const char *topic, const char *payload)
{
    return MQTT_PublishPriority(topic, payload);
}

static uint8_t cellular_publish_telemetry(const char *payload)
{
    return mqttPublish(payload);
}

static char *cellular_read_downlink(void)
{
    return MQTT_Subscriber_Run();
}

static void cellular_handle_downlink(const char *payload)
{
    handle_downlink_cjson(payload);
}

static uint8_t cellular_check_disconnect_urc(void)
{
    return MQTT_CheckDisconnectURC();
}

static uint8_t cellular_maintain(uint32_t keepalive_ms,
                                 const char *topic,
                                 const char *ping_payload,
                                 uint8_t *attempted)
{
    return MQTT_Maintain(keepalive_ms, topic, ping_payload, attempted);
}

static void cellular_report_publish_result(uint8_t ok)
{
    comm_manager_report_publish_result(ok);
}

static void cellular_report_disconnected(void)
{
    comm_manager_report_mqtt_disconnected();
}

static uint8_t cellular_last_publish_attempted(void)
{
    return MQTT_LastPublishAttempted();
}

static const char *cellular_last_publish_stage(void)
{
    return MQTT_LastPublishStage();
}

static void cellular_set_ota_flash(W25Q_Handle *flash)
{
    MQTT_SetOtaFlash(flash);
}

static uint8_t cellular_ota_is_busy(void)
{
    return MQTT_OtaIsBusy();
}

static uint8_t cellular_report_ota_boot_result_once(void)
{
    return MQTT_ReportOtaBootResultOnce();
}

static uint8_t cellular_poll_signal_quality(int32_t *rssi, int32_t *ber)
{
    return modem_get_signal_quality(rssi, ber);
}

static connectivity_state_t cellular_get_state(void)
{
    return comm_manager_get_state();
}

static connectivity_error_t cellular_get_last_error(void)
{
    return comm_manager_get_last_error();
}

static const connectivity_health_t *cellular_get_health(void)
{
    return comm_manager_get_health();
}

static const char *cellular_state_name(connectivity_state_t state)
{
    return comm_manager_state_name(state);
}

static const char *cellular_error_name(connectivity_error_t error)
{
    return comm_manager_error_name(error);
}

const connectivity_backend_t *connectivity_cellular_backend(void)
{
    static const connectivity_backend_t backend = {
        .name = "cellular",
        .init = cellular_init,
        .run_once = cellular_run_once,
        .is_online = cellular_is_online,
        .publish = cellular_publish,
        .publish_priority = cellular_publish_priority,
        .publish_telemetry = cellular_publish_telemetry,
        .read_downlink = cellular_read_downlink,
        .handle_downlink = cellular_handle_downlink,
        .check_disconnect_urc = cellular_check_disconnect_urc,
        .maintain = cellular_maintain,
        .report_publish_result = cellular_report_publish_result,
        .report_disconnected = cellular_report_disconnected,
        .last_publish_attempted = cellular_last_publish_attempted,
        .last_publish_stage = cellular_last_publish_stage,
        .set_ota_flash = cellular_set_ota_flash,
        .ota_is_busy = cellular_ota_is_busy,
        .report_ota_boot_result_once = cellular_report_ota_boot_result_once,
        .poll_signal_quality = cellular_poll_signal_quality,
        .get_state = cellular_get_state,
        .get_last_error = cellular_get_last_error,
        .get_health = cellular_get_health,
        .state_name = cellular_state_name,
        .error_name = cellular_error_name,
    };

    return &backend;
}
