#include "app_debug_led.h"
#include "app_watchdog.h"
#include "board_profile.h"
#include "main.h"

#define DEBUG_LED_GPIO_PORT     BOARD_DEBUG_LED_GPIO_PORT
#define DEBUG_LED_GPIO_PIN      BOARD_DEBUG_LED_GPIO_PIN
#define ACTIVITY_LED_GPIO_PORT  BOARD_ACTIVITY_LED_GPIO_PORT
#define ACTIVITY_LED_GPIO_PIN   BOARD_ACTIVITY_LED_GPIO_PIN
#define DEBUG_LED_ACTIVE_HIGH   BOARD_DEBUG_LED_ACTIVE_HIGH

#define HEARTBEAT_PERIOD_MS       2000UL
#define HEARTBEAT_ON_MS           30UL
#define BOOT_PULSE_ON_MS          35UL
#define ACTIVITY_BLINK_ON_MS      120UL
#define ACTIVITY_BLINK_OFF_MS     100UL

#define OTA_BLINK_PERIOD_MS       160UL
#define OTA_BLINK_ON_MS           80UL

typedef enum {
    ACTIVITY_IDLE = 0,
    ACTIVITY_UPLINK,
    ACTIVITY_DOWNLINK
} activity_mode_t;

static uint32_t s_last_heartbeat_tick = 0;
static uint32_t s_heartbeat_start_tick = 0;
static uint32_t s_activity_start_tick = 0;
static activity_mode_t s_activity_mode = ACTIVITY_IDLE;
static uint8_t s_heartbeat_on = 0;
static uint8_t s_ota_active = 0;

static void gpio_led_write(GPIO_TypeDef *port, uint16_t pin, uint8_t on)
{
#if DEBUG_LED_ACTIVE_HIGH
    HAL_GPIO_WritePin(port, pin, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
#else
    HAL_GPIO_WritePin(port, pin, on ? GPIO_PIN_RESET : GPIO_PIN_SET);
#endif
}

static void heartbeat_write(uint8_t on)
{
    gpio_led_write(DEBUG_LED_GPIO_PORT, DEBUG_LED_GPIO_PIN, on);
}

static void activity_write(uint8_t on)
{
    gpio_led_write(ACTIVITY_LED_GPIO_PORT, ACTIVITY_LED_GPIO_PIN, on);
}

void app_debug_led_init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOD_CLK_ENABLE();

    GPIO_InitStruct.Pin = DEBUG_LED_GPIO_PIN | ACTIVITY_LED_GPIO_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;

    HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

    heartbeat_write(0);
    activity_write(0);

    s_last_heartbeat_tick = HAL_GetTick();
    s_heartbeat_start_tick = s_last_heartbeat_tick;
    s_activity_start_tick = s_last_heartbeat_tick;
    s_activity_mode = ACTIVITY_IDLE;
    s_heartbeat_on = 0U;
    s_ota_active = 0;
}

void app_debug_led_boot_pulse(void)
{
    heartbeat_write(1);
    app_watchdog_force_refresh();
    HAL_Delay(BOOT_PULSE_ON_MS);
    heartbeat_write(0);
    app_watchdog_force_refresh();
}

void app_debug_led_set_ota_active(uint8_t active)
{
    s_ota_active = active ? 1U : 0U;
    s_activity_start_tick = HAL_GetTick();
    if (!s_ota_active) activity_write(0);
}

void app_debug_led_event(debug_led_event_t event)
{
    uint32_t now = HAL_GetTick();

    switch (event)
    {
        case DEBUG_LED_EVENT_UPLINK:
            s_activity_mode = ACTIVITY_UPLINK;
            s_activity_start_tick = now;
            activity_write(1);
            break;

        case DEBUG_LED_EVENT_DOWNLINK:
            s_activity_mode = ACTIVITY_DOWNLINK;
            s_activity_start_tick = now;
            activity_write(1);
            break;

        case DEBUG_LED_EVENT_ERROR_ON:
        case DEBUG_LED_EVENT_ERROR_OFF:
            /* LED roles are fixed: errors must not hide the heartbeat. */
            break;

        default:
            break;
    }
}

void app_debug_led_run_once(void)
{
    uint32_t now = HAL_GetTick();
    uint32_t activity_elapsed = now - s_activity_start_tick;

    app_watchdog_mark_task(APP_WATCHDOG_TASK_LED);

    if (!s_heartbeat_on && (now - s_last_heartbeat_tick) >= HEARTBEAT_PERIOD_MS) {
        s_last_heartbeat_tick = now;
        s_heartbeat_start_tick = now;
        s_heartbeat_on = 1U;
        heartbeat_write(1);
    } else if (s_heartbeat_on && (now - s_heartbeat_start_tick) >= HEARTBEAT_ON_MS) {
        s_heartbeat_on = 0U;
        heartbeat_write(0);
    }

    if (s_ota_active) {
        uint32_t phase = activity_elapsed % OTA_BLINK_PERIOD_MS;
        activity_write((phase < OTA_BLINK_ON_MS) ? 1U : 0U);
    } else if (s_activity_mode == ACTIVITY_UPLINK) {
        if (activity_elapsed >= ACTIVITY_BLINK_ON_MS) {
            activity_write(0);
            s_activity_mode = ACTIVITY_IDLE;
        }
    } else if (s_activity_mode == ACTIVITY_DOWNLINK) {
        if (activity_elapsed < ACTIVITY_BLINK_ON_MS) {
            activity_write(1);
        } else if (activity_elapsed < (ACTIVITY_BLINK_ON_MS + ACTIVITY_BLINK_OFF_MS)) {
            activity_write(0);
        } else if (activity_elapsed < ((2U * ACTIVITY_BLINK_ON_MS) + ACTIVITY_BLINK_OFF_MS)) {
            activity_write(1);
        } else {
            activity_write(0);
            s_activity_mode = ACTIVITY_IDLE;
        }
    }
}
