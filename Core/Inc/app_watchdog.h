#ifndef APP_WATCHDOG_H
#define APP_WATCHDOG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_WATCHDOG_TASK_RUNTIME = 0,
    APP_WATCHDOG_TASK_MODBUS,
    APP_WATCHDOG_TASK_LED,
    APP_WATCHDOG_TASK_COUNT
} app_watchdog_task_t;

void app_watchdog_start(void);
void app_watchdog_register_task(app_watchdog_task_t task);
void app_watchdog_mark_task(app_watchdog_task_t task);
void app_watchdog_refresh(void);
void app_watchdog_force_refresh(void);
uint8_t app_watchdog_is_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_WATCHDOG_H */
