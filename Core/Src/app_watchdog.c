#include "app_watchdog.h"

#include "debug_uart.h"
#include "main.h"

#define IWDG_KEY_START        0xCCCCU
#define IWDG_KEY_REFRESH      0xAAAAU
#define IWDG_KEY_WRITE_ACCESS 0x5555U

#define IWDG_PRESCALER_256_VALUE 6U
#define IWDG_RELOAD_MAX_VALUE    4095U
#define IWDG_LSI_READY_MAX_LOOPS      1000000UL
#define IWDG_REGISTER_SYNC_MAX_LOOPS  1000000UL

#define APP_WATCHDOG_RUNTIME_TIMEOUT_MS 180000UL
#define APP_WATCHDOG_MODBUS_TIMEOUT_MS  15000UL
#define APP_WATCHDOG_LED_TIMEOUT_MS     15000UL
#define APP_WATCHDOG_DIAG_INTERVAL_MS   5000UL

static volatile uint8_t s_watchdog_ready = 0U;
static volatile uint32_t s_registered_tasks = 0U;
static volatile uint32_t s_task_last_seen[APP_WATCHDOG_TASK_COUNT] = {0};
static uint32_t s_last_blocked_diag_tick = 0U;

static uint32_t app_watchdog_task_bit(app_watchdog_task_t task)
{
    return (uint32_t)1UL << (uint32_t)task;
}

static uint32_t app_watchdog_task_timeout_ms(app_watchdog_task_t task)
{
    switch (task) {
        case APP_WATCHDOG_TASK_RUNTIME:
            return APP_WATCHDOG_RUNTIME_TIMEOUT_MS;

        case APP_WATCHDOG_TASK_MODBUS:
            return APP_WATCHDOG_MODBUS_TIMEOUT_MS;

        case APP_WATCHDOG_TASK_LED:
            return APP_WATCHDOG_LED_TIMEOUT_MS;

        default:
            return 0U;
    }
}

static const char *app_watchdog_task_name(app_watchdog_task_t task)
{
    switch (task) {
        case APP_WATCHDOG_TASK_RUNTIME:
            return "runtime";

        case APP_WATCHDOG_TASK_MODBUS:
            return "modbus";

        case APP_WATCHDOG_TASK_LED:
            return "led";

        default:
            return "unknown";
    }
}

static uint8_t app_watchdog_tasks_healthy(void)
{
    uint32_t registered = s_registered_tasks;

    if (registered == 0U) {
        return 1U;
    }

    uint32_t now = HAL_GetTick();

    for (uint32_t task = 0U; task < (uint32_t)APP_WATCHDOG_TASK_COUNT; task++) {
        uint32_t bit = 1UL << task;

        if ((registered & bit) == 0U) {
            continue;
        }

        uint32_t timeout_ms = app_watchdog_task_timeout_ms((app_watchdog_task_t)task);

        if (timeout_ms == 0U) {
            return 0U;
        }

        uint32_t last_seen = s_task_last_seen[task];
        if (last_seen > now) {
            continue;
        }

        if ((now - last_seen) > timeout_ms) {
            if ((now - s_last_blocked_diag_tick) >= APP_WATCHDOG_DIAG_INTERVAL_MS) {
                s_last_blocked_diag_tick = now;
                print("[IWDG] refresh blocked: task=%s stale age=%lu timeout=%lu\r\n",
                      app_watchdog_task_name((app_watchdog_task_t)task),
                      (unsigned long)(now - last_seen),
                      (unsigned long)timeout_ms);
            }
            return 0U;
        }
    }

    return 1U;
}

void app_watchdog_start(void)
{
#ifdef DEBUG
    __HAL_DBGMCU_FREEZE_IWDG();
#endif

    __HAL_RCC_LSI_ENABLE();

    for (uint32_t i = 0; i < IWDG_LSI_READY_MAX_LOOPS; i++) {
        if (__HAL_RCC_GET_FLAG(RCC_FLAG_LSIRDY) != RESET) {
            break;
        }
    }

    IWDG->KR = IWDG_KEY_WRITE_ACCESS;
    IWDG->PR = IWDG_PRESCALER_256_VALUE;
    IWDG->RLR = IWDG_RELOAD_MAX_VALUE;

    for (uint32_t i = 0; i < IWDG_REGISTER_SYNC_MAX_LOOPS; i++) {
        if (IWDG->SR == 0U) {
            break;
        }
    }

    IWDG->KR = IWDG_KEY_START;
    s_watchdog_ready = 1U;
    app_watchdog_refresh();
}

void app_watchdog_register_task(app_watchdog_task_t task)
{
    if ((uint32_t)task >= (uint32_t)APP_WATCHDOG_TASK_COUNT) {
        return;
    }

    s_task_last_seen[task] = HAL_GetTick();
    s_registered_tasks |= app_watchdog_task_bit(task);
}

void app_watchdog_mark_task(app_watchdog_task_t task)
{
    if ((uint32_t)task >= (uint32_t)APP_WATCHDOG_TASK_COUNT) {
        return;
    }

    s_task_last_seen[task] = HAL_GetTick();
}

void app_watchdog_refresh(void)
{
    if (s_watchdog_ready && app_watchdog_tasks_healthy()) {
        IWDG->KR = IWDG_KEY_REFRESH;
    }
}

void app_watchdog_force_refresh(void)
{
    if (s_watchdog_ready) {
        IWDG->KR = IWDG_KEY_REFRESH;
    }
}

uint8_t app_watchdog_is_ready(void)
{
    return s_watchdog_ready;
}
