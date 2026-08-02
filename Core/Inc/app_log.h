#ifndef APP_LOG_H
#define APP_LOG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_LOG_INFO = 0,
    APP_LOG_WARN,
    APP_LOG_ERROR,
    APP_LOG_FATAL
} app_log_level_t;

void app_log_init(void);
uint8_t app_log_critical_persist_deferred_once(void);

/*
 * RAM-only/runtime log.
 * Use this for debug/soft warnings. This is not persisted to flash.
 */
void app_log_push(app_log_level_t level,
                  const char *module,
                  const char *event,
                  const char *message,
                  int32_t value1,
                  int32_t value2);

/*
 * Critical persistent log.
 * Use this only for high-value events:
 * - repeated MQTT/GPRS/AT failures
 * - disconnect URCs
 * - config parse/save failures
 * - config applied
 * - flash/RTC/watchdog faults
 */
void app_log_critical(app_log_level_t level,
                      const char *module,
                      const char *event,
                      const char *message,
                      int32_t value1,
                      int32_t value2,
                      const char *recovery);

/*
 * Slow cloud upload for persistent logs.
 * Internally rate-limited to 1 log/minute by default.
 */
uint8_t app_log_critical_flush_cloud_once(void);

uint16_t app_log_critical_pending_count(void);
uint16_t app_log_critical_total_count(void);
uint32_t app_log_critical_dropped_count(void);

/*
 * Legacy RAM log upload APIs.
 * Keep available, but do not call these in the main loop if using flash logs.
 */
uint8_t app_log_flush_one(void);
uint8_t app_log_flush_all(uint8_t max_to_send);

uint16_t app_log_count(void);
uint32_t app_log_dropped_count(void);

const char *app_log_level_name(app_log_level_t level);

#ifdef __cplusplus
}
#endif

#endif /* APP_LOG_H */
