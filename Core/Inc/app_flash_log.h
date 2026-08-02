#ifndef APP_FLASH_LOG_H
#define APP_FLASH_LOG_H

#include <stdint.h>
#include "w25q16.h"
#include "app_log.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Persistent critical log queue.
 *
 * Storage policy:
 * - Only critical/important events should be written here.
 * - Uploaded logs are marked as uploaded, not physically erased immediately.
 * - Physical erase happens when the log region is reused.
 */

#define APP_FLASH_LOG_UPLOAD_INTERVAL_MS   60000UL
#define APP_FLASH_LOG_MAX_UPLOAD_PER_RUN   1U

uint8_t app_flash_log_init(W25Q_Handle *flash);

uint8_t app_flash_log_write(app_log_level_t level,
                            const char *module,
                            const char *event,
                            const char *message,
                            int32_t value1,
                            int32_t value2,
                            const char *recovery);

uint8_t app_flash_log_write_incident(app_log_level_t level,
                                     const char *module,
                                     const char *event,
                                     const char *message,
                                     int32_t value1,
                                     int32_t value2,
                                     const char *recovery,
                                     uint32_t first_seen_tick);

uint8_t app_flash_log_upload_one(void);
uint8_t app_flash_log_upload_due(void);

uint16_t app_flash_log_pending_count(void);
uint16_t app_flash_log_total_count(void);
uint32_t app_flash_log_dropped_count(void);
uint32_t app_flash_log_deduped_count(void);
uint8_t app_flash_log_is_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_FLASH_LOG_H */
