#ifndef APP_DEBUG_LED_H
#define APP_DEBUG_LED_H

#include <stdint.h>

typedef enum
{
    DEBUG_LED_EVENT_UPLINK = 0,
    DEBUG_LED_EVENT_DOWNLINK,
    DEBUG_LED_EVENT_ERROR_ON,
    DEBUG_LED_EVENT_ERROR_OFF
} debug_led_event_t;

void app_debug_led_init(void);
void app_debug_led_boot_pulse(void);
void app_debug_led_run_once(void);
void app_debug_led_event(debug_led_event_t event);
void app_debug_led_set_ota_active(uint8_t active);

#endif
