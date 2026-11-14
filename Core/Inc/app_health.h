#ifndef APP_HEALTH_H
#define APP_HEALTH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t valid;
    int32_t rssi;
    int32_t ber;
    int32_t dbm;
    uint32_t ageMs;
    const char *quality;
} app_signal_snapshot_t;

void app_health_init(void);
void app_health_set_boot_info(uint32_t boot_count, const char *boot_id, const char *reset_reason);
void app_health_run_once(void);
uint8_t app_health_publish_now(void);
uint8_t app_health_signal_poll_due(void);
uint8_t app_health_get_signal_snapshot(app_signal_snapshot_t *out);

#ifdef __cplusplus
}
#endif

#endif /* APP_HEALTH_H */
