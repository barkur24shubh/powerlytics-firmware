#pragma once

#include "stm32f4xx_hal.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTC_BKP_MAGIC  0x1239U

typedef struct
{
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} rtc_datetime_t;

int  rtc_clock_configure_with_fallback(void);
void rtc_init_with_bkp_once(void);
uint8_t rtc_get_datetime(rtc_datetime_t *dt);
uint8_t rtc_set_datetime_utc(uint16_t year,
                             uint8_t month,
                             uint8_t day,
                             uint8_t hour,
                             uint8_t minute,
                             uint8_t second);
void rtc_sync_to_build_time_if_newer(void);
void rtc_get_datetime_strings(char *ts_buf, int ts_buf_sz);
uint8_t rtc_is_datetime_valid(void);
uint32_t rtc_increment_boot_count(void);

#ifdef __cplusplus
}
#endif
