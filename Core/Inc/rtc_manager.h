#pragma once

#include "stm32f4xx_hal.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTC_BKP_MAGIC  0x1239U

/*
 * Manual one-time RTC set hook.
 *
 * To set RTC from code once:
 *   1. Set RTC_CODE_TIME_SET_ENABLED to 1U.
 *   2. Update the IST date/time below.
 *   3. Build/flash once.
 *   4. Set RTC_CODE_TIME_SET_ENABLED back to 0U for normal firmware.
 *
 * The firmware records the date/time stamp in RTC backup registers, so the same
 * code-time value is applied only once. Changing these constants applies it once
 * again.
 */
#ifndef RTC_CODE_TIME_SET_ENABLED
#define RTC_CODE_TIME_SET_ENABLED 0U
#endif

#define RTC_CODE_TIME_YEAR    2026U
#define RTC_CODE_TIME_MONTH   8U
#define RTC_CODE_TIME_DAY     7U
#define RTC_CODE_TIME_HOUR    8U
#define RTC_CODE_TIME_MINUTE  51U
#define RTC_CODE_TIME_SECOND  0U

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
uint8_t rtc_set_datetime_ist(uint16_t year,
                             uint8_t month,
                             uint8_t day,
                             uint8_t hour,
                             uint8_t minute,
                             uint8_t second);
uint8_t rtc_sync_to_code_time_once(void);
void rtc_sync_to_build_time_if_newer(void);
void rtc_get_datetime_strings(char *ts_buf, int ts_buf_sz);
uint8_t rtc_is_datetime_valid(void);
uint8_t rtc_time_was_preserved(void);
uint32_t rtc_increment_boot_count(void);

#ifdef __cplusplus
}
#endif
