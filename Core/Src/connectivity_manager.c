#include "connectivity.h"

#include "board_profile.h"
#include "connectivity_backend.h"

#ifndef BOARD_CONNECTIVITY_MASK
#error "BOARD_CONNECTIVITY_MASK must be defined by the active board profile"
#endif

#ifndef BOARD_CONNECTIVITY_PRIMARY
#define BOARD_CONNECTIVITY_PRIMARY BOARD_CONNECTIVITY_MASK
#endif

static const connectivity_health_t s_empty_health = {0};

static const connectivity_backend_t *active_backend(void)
{
    static const connectivity_backend_t *s_backend = 0;

    if (s_backend) {
        return s_backend;
    }

#if BOARD_CONNECTIVITY_PRIMARY == BOARD_CONNECTIVITY_CELLULAR
    s_backend = connectivity_cellular_backend();
#elif BOARD_CONNECTIVITY_PRIMARY == BOARD_CONNECTIVITY_ETHERNET
    s_backend = connectivity_ethernet_backend();
#elif BOARD_CONNECTIVITY_PRIMARY == BOARD_CONNECTIVITY_WIFI
    s_backend = connectivity_wifi_backend();
#else
#error "Active board profile selects an unsupported connectivity backend"
#endif

    return s_backend;
}

const char *connectivity_backend_name(void)
{
    const connectivity_backend_t *b = active_backend();
    return (b && b->name) ? b->name : "unknown";
}

void connectivity_init(void)
{
    const connectivity_backend_t *b = active_backend();
    if (b && b->init) {
        b->init();
    }
}

void connectivity_run_once(void)
{
    const connectivity_backend_t *b = active_backend();
    if (b && b->run_once) {
        b->run_once();
    }
}

uint8_t connectivity_is_online(void)
{
    const connectivity_backend_t *b = active_backend();
    return (b && b->is_online) ? b->is_online() : 0U;
}

uint8_t connectivity_publish(const char *topic, const char *payload)
{
    const connectivity_backend_t *b = active_backend();
    return (b && b->publish) ? b->publish(topic, payload) : 0U;
}

uint8_t connectivity_publish_priority(const char *topic, const char *payload)
{
    const connectivity_backend_t *b = active_backend();
    if (b && b->publish_priority) {
        return b->publish_priority(topic, payload);
    }
    return (b && b->publish) ? b->publish(topic, payload) : 0U;
}

uint8_t connectivity_publish_telemetry(const char *payload)
{
    const connectivity_backend_t *b = active_backend();
    return (b && b->publish_telemetry) ? b->publish_telemetry(payload) : 0U;
}

char *connectivity_read_downlink(void)
{
    const connectivity_backend_t *b = active_backend();
    return (b && b->read_downlink) ? b->read_downlink() : 0;
}

void connectivity_handle_downlink(const char *payload)
{
    const connectivity_backend_t *b = active_backend();
    if (b && b->handle_downlink) {
        b->handle_downlink(payload);
    }
}

uint8_t connectivity_check_disconnect_urc(void)
{
    const connectivity_backend_t *b = active_backend();
    return (b && b->check_disconnect_urc) ? b->check_disconnect_urc() : 0U;
}

uint8_t connectivity_maintain(uint32_t keepalive_ms,
                              const char *topic,
                              const char *ping_payload,
                              uint8_t *attempted)
{
    const connectivity_backend_t *b = active_backend();
    if (attempted) {
        *attempted = 0U;
    }
    return (b && b->maintain) ? b->maintain(keepalive_ms, topic, ping_payload, attempted) : 1U;
}

void connectivity_report_publish_result(uint8_t ok)
{
    const connectivity_backend_t *b = active_backend();
    if (b && b->report_publish_result) {
        b->report_publish_result(ok);
    }
}

void connectivity_report_disconnected(void)
{
    const connectivity_backend_t *b = active_backend();
    if (b && b->report_disconnected) {
        b->report_disconnected();
    }
}

uint8_t connectivity_last_publish_attempted(void)
{
    const connectivity_backend_t *b = active_backend();
    return (b && b->last_publish_attempted) ? b->last_publish_attempted() : 0U;
}

const char *connectivity_last_publish_stage(void)
{
    const connectivity_backend_t *b = active_backend();
    return (b && b->last_publish_stage) ? b->last_publish_stage() : "UNSUPPORTED";
}

void connectivity_set_ota_flash(W25Q_Handle *flash)
{
    const connectivity_backend_t *b = active_backend();
    if (b && b->set_ota_flash) {
        b->set_ota_flash(flash);
    }
}

uint8_t connectivity_ota_is_busy(void)
{
    const connectivity_backend_t *b = active_backend();
    return (b && b->ota_is_busy) ? b->ota_is_busy() : 0U;
}

uint8_t connectivity_report_ota_boot_result_once(void)
{
    const connectivity_backend_t *b = active_backend();
    return (b && b->report_ota_boot_result_once) ? b->report_ota_boot_result_once() : 0U;
}

uint8_t connectivity_poll_signal_quality(int32_t *rssi, int32_t *ber)
{
    const connectivity_backend_t *b = active_backend();
    return (b && b->poll_signal_quality) ? b->poll_signal_quality(rssi, ber) : 0U;
}

connectivity_state_t connectivity_get_state(void)
{
    const connectivity_backend_t *b = active_backend();
    return (b && b->get_state) ? b->get_state() : COMM_STATE_BOOT;
}

connectivity_error_t connectivity_get_last_error(void)
{
    const connectivity_backend_t *b = active_backend();
    return (b && b->get_last_error) ? b->get_last_error() : COMM_ERR_NONE;
}

const connectivity_health_t *connectivity_get_health(void)
{
    const connectivity_backend_t *b = active_backend();
    return (b && b->get_health) ? b->get_health() : &s_empty_health;
}

const char *connectivity_state_name(connectivity_state_t state)
{
    const connectivity_backend_t *b = active_backend();
    return (b && b->state_name) ? b->state_name(state) : "UNSUPPORTED";
}

const char *connectivity_error_name(connectivity_error_t error)
{
    const connectivity_backend_t *b = active_backend();
    return (b && b->error_name) ? b->error_name(error) : "NONE";
}
