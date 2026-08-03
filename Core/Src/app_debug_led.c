#include "app_debug_led.h"
#include "app_watchdog.h"
#include "main.h"

#define DEBUG_LED_GPIO_PORT GPIOD
#define DEBUG_LED_GPIO_PIN  GPIO_PIN_3

/*
 * Change this if your LED is active-low.
 * Many STM32 board LEDs are active-low.
 */
#define DEBUG_LED_ACTIVE_HIGH 0

#define HEARTBEAT_PERIOD_MS       2000UL
#define HEARTBEAT_ON_MS           60UL

#define UPLINK_BLINK_ON_MS        900UL

#define DOWNLINK_BLINK_ON_MS      500UL
#define DOWNLINK_BLINK_OFF_MS     250UL

#define OTA_BLINK_PERIOD_MS       160UL
#define OTA_BLINK_ON_MS           80UL

typedef enum
{
    LED_MODE_HEARTBEAT = 0,
    LED_MODE_HEARTBEAT_PULSE,
    LED_MODE_UPLINK,
    LED_MODE_DOWNLINK,
    LED_MODE_OTA,
    LED_MODE_ERROR_SOLID
} led_mode_t;

static led_mode_t s_mode = LED_MODE_HEARTBEAT;
static uint32_t s_mode_start_tick = 0;
static uint32_t s_last_heartbeat_tick = 0;
static uint8_t s_error_active = 0;
static uint8_t s_ota_active = 0;

static void led_write(uint8_t on)
{
#if DEBUG_LED_ACTIVE_HIGH
    HAL_GPIO_WritePin(DEBUG_LED_GPIO_PORT,
                      DEBUG_LED_GPIO_PIN,
                      on ? GPIO_PIN_SET : GPIO_PIN_RESET);
#else
    HAL_GPIO_WritePin(DEBUG_LED_GPIO_PORT,
                      DEBUG_LED_GPIO_PIN,
                      on ? GPIO_PIN_RESET : GPIO_PIN_SET);
#endif
}

void app_debug_led_init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOD_CLK_ENABLE();

    GPIO_InitStruct.Pin = DEBUG_LED_GPIO_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;

    HAL_GPIO_Init(DEBUG_LED_GPIO_PORT, &GPIO_InitStruct);

    led_write(0);

    s_mode = LED_MODE_HEARTBEAT;
    s_mode_start_tick = HAL_GetTick();
    s_last_heartbeat_tick = HAL_GetTick();
    s_error_active = 0;
    s_ota_active = 0;
}

void app_debug_led_set_ota_active(uint8_t active)
{
    uint32_t now = HAL_GetTick();

    s_ota_active = active ? 1U : 0U;

    if (s_error_active)
    {
        return;
    }

    if (s_ota_active)
    {
        s_mode = LED_MODE_OTA;
        s_mode_start_tick = now;
        led_write(1);
    }
    else if (s_mode == LED_MODE_OTA)
    {
        s_mode = LED_MODE_HEARTBEAT;
        s_mode_start_tick = now;
        s_last_heartbeat_tick = now;
        led_write(0);
    }
}

void app_debug_led_event(debug_led_event_t event)
{
    uint32_t now = HAL_GetTick();

    switch (event)
    {
        case DEBUG_LED_EVENT_ERROR_ON:
            s_error_active = 1;
            s_ota_active = 0;
            s_mode = LED_MODE_ERROR_SOLID;
            s_mode_start_tick = now;
            led_write(1);
            break;

        case DEBUG_LED_EVENT_ERROR_OFF:
            s_error_active = 0;
            s_mode = s_ota_active ? LED_MODE_OTA : LED_MODE_HEARTBEAT;
            s_mode_start_tick = now;
            led_write(s_ota_active ? 1U : 0U);
            break;

        case DEBUG_LED_EVENT_UPLINK:
            if (!s_error_active)
            {
                s_mode = LED_MODE_UPLINK;
                s_mode_start_tick = now;
                led_write(1);
            }
            break;

        case DEBUG_LED_EVENT_DOWNLINK:
            if (!s_error_active)
            {
                s_mode = LED_MODE_DOWNLINK;
                s_mode_start_tick = now;
                led_write(1);
            }
            break;

        default:
            break;
    }
}

void app_debug_led_run_once(void)
{
    uint32_t now = HAL_GetTick();
    uint32_t elapsed = now - s_mode_start_tick;

    app_watchdog_mark_task(APP_WATCHDOG_TASK_LED);

    if (s_error_active)
    {
        s_mode = LED_MODE_ERROR_SOLID;
        led_write(1);
        return;
    }

    if (s_ota_active && s_mode != LED_MODE_OTA)
    {
        s_mode = LED_MODE_OTA;
        s_mode_start_tick = now;
        elapsed = 0U;
    }

    switch (s_mode)
    {
        case LED_MODE_ERROR_SOLID:
            led_write(1);
            break;

        case LED_MODE_UPLINK:
            if (elapsed < UPLINK_BLINK_ON_MS)
            {
                led_write(1);
            }
            else
            {
                led_write(0);
                s_mode = LED_MODE_HEARTBEAT;
                s_mode_start_tick = now;
            }
            break;

        case LED_MODE_DOWNLINK:
            /*
             * Double blink:
             * ON  0-120ms
             * OFF 120-240ms
             * ON  240-360ms
             * OFF then heartbeat
             */
            if (elapsed < DOWNLINK_BLINK_ON_MS)
            {
                led_write(1);
            }
            else if (elapsed < (DOWNLINK_BLINK_ON_MS + DOWNLINK_BLINK_OFF_MS))
            {
                led_write(0);
            }
            else if (elapsed < ((2 * DOWNLINK_BLINK_ON_MS) + DOWNLINK_BLINK_OFF_MS))
            {
                led_write(1);
            }
            else
            {
                led_write(0);
                s_mode = LED_MODE_HEARTBEAT;
                s_mode_start_tick = now;
            }
            break;

        case LED_MODE_OTA:
        {
            uint32_t phase = elapsed % OTA_BLINK_PERIOD_MS;
            led_write((phase < OTA_BLINK_ON_MS) ? 1U : 0U);
            break;
        }

        case LED_MODE_HEARTBEAT_PULSE:
            if (elapsed < HEARTBEAT_ON_MS)
            {
                led_write(1);
            }
            else
            {
                led_write(0);
                s_mode = LED_MODE_HEARTBEAT;
                s_mode_start_tick = now;
            }
            break;

        case LED_MODE_HEARTBEAT:
        default:
            if ((now - s_last_heartbeat_tick) >= HEARTBEAT_PERIOD_MS)
            {
                s_last_heartbeat_tick = now;
                s_mode = LED_MODE_HEARTBEAT_PULSE;
                s_mode_start_tick = now;
                led_write(1);
            }
            break;
    }
}
