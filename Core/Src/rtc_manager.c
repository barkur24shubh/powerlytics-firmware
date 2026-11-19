#include "rtc_manager.h"
#include "debug_uart.h"

#include <stdio.h>
#include <string.h>

extern RTC_HandleTypeDef hrtc;

#define RTC_BKP_BOOT_COUNT_DR RTC_BKP_DR2
/* DR3..DR13 hold the MCU fault record (app_gateway_platform.c), DR15 the
 * reset intent, DR16/17 the watchdog stale task. */
#define RTC_BKP_CODE_TIME_DATE_DR RTC_BKP_DR18
#define RTC_BKP_CODE_TIME_TIME_DR RTC_BKP_DR19
#define RTC_BKP_BOOT_STAGE_DR RTC_BKP_DR14
#define RTC_BKP_BOOT_STAGE_MAGIC 0xB0070000UL
#define RTC_BKP_BOOT_STAGE_MASK  0x000000FFUL

static uint8_t s_rtc_backup_magic_seen = 0U;
static uint8_t s_rtc_time_preserved = 0U;

static void rtc_enable_backup_domain_access(void)
{
    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();
}

static uint32_t rtc_get_clock_source(void)
{
    return (RCC->BDCR & RCC_BDCR_RTCSEL);
}

static const char *rtc_clock_name(uint32_t rtcsel)
{
    switch (rtcsel)
    {
        case RCC_BDCR_RTCSEL_0:
            return "LSE";

        case RCC_BDCR_RTCSEL_1:
            return "LSI";

        case RCC_BDCR_RTCSEL:
            return "HSE_DIV";

        default:
            return "NONE";
    }
}

const char *rtc_boot_stage_name(rtc_boot_stage_t stage)
{
    switch (stage)
    {
        case RTC_BOOT_STAGE_RTC:
            return "RTC";

        case RTC_BOOT_STAGE_SD_INIT:
            return "SD_INIT";

        case RTC_BOOT_STAGE_FLASH_INIT:
            return "FLASH_INIT";

        case RTC_BOOT_STAGE_CONFIG_LOAD:
            return "CONFIG_LOAD";

        case RTC_BOOT_STAGE_FLASH_LOG_INIT:
            return "FLASH_LOG_INIT";

        case RTC_BOOT_STAGE_RTOS_START:
            return "RTOS_START";

        case RTC_BOOT_STAGE_NONE:
        default:
            return "NONE";
    }
}

static uint32_t rtc_boot_stage_encode(rtc_boot_stage_t stage)
{
    return RTC_BKP_BOOT_STAGE_MAGIC | ((uint32_t)stage & RTC_BKP_BOOT_STAGE_MASK);
}

static rtc_boot_stage_t rtc_boot_stage_decode(uint32_t raw)
{
    if ((raw & ~RTC_BKP_BOOT_STAGE_MASK) != RTC_BKP_BOOT_STAGE_MAGIC)
    {
        return RTC_BOOT_STAGE_NONE;
    }

    uint32_t stage = raw & RTC_BKP_BOOT_STAGE_MASK;
    if (stage > (uint32_t)RTC_BOOT_STAGE_RTOS_START)
    {
        return RTC_BOOT_STAGE_NONE;
    }

    return (rtc_boot_stage_t)stage;
}

rtc_boot_stage_t rtc_boot_stage_previous(void)
{
    rtc_enable_backup_domain_access();
    __HAL_RCC_RTC_ENABLE();
    hrtc.Instance = RTC;

    return rtc_boot_stage_decode(HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_BOOT_STAGE_DR));
}

void rtc_boot_stage_mark(rtc_boot_stage_t stage)
{
    rtc_enable_backup_domain_access();
    __HAL_RCC_RTC_ENABLE();
    hrtc.Instance = RTC;

    HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_BOOT_STAGE_DR, rtc_boot_stage_encode(stage));
}

static uint8_t rtc_weekday_from_date(uint16_t year, uint8_t month, uint8_t day)
{
    if (month < 3U)
    {
        month += 12U;
        year--;
    }

    uint16_t k = (uint16_t)(year % 100U);
    uint16_t j = (uint16_t)(year / 100U);
    uint8_t h = (uint8_t)((day + ((13U * (month + 1U)) / 5U) + k + (k / 4U) + (j / 4U) + (5U * j)) % 7U);

    switch (h)
    {
        case 0: return RTC_WEEKDAY_SATURDAY;
        case 1: return RTC_WEEKDAY_SUNDAY;
        case 2: return RTC_WEEKDAY_MONDAY;
        case 3: return RTC_WEEKDAY_TUESDAY;
        case 4: return RTC_WEEKDAY_WEDNESDAY;
        case 5: return RTC_WEEKDAY_THURSDAY;
        default: return RTC_WEEKDAY_FRIDAY;
    }
}

static uint8_t rtc_month_from_build_str(const char *month)
{
    static const char names[] = "JanFebMarAprMayJunJulAugSepOctNovDec";

    for (uint8_t i = 0U; i < 12U; i++)
    {
        if (strncmp(month, &names[i * 3U], 3U) == 0)
        {
            return (uint8_t)(i + 1U);
        }
    }

    return 1U;
}

static void rtc_format_datetime_ist(char *ts_buf, int ts_buf_sz, const rtc_datetime_t *dt)
{
    if (!ts_buf || ts_buf_sz <= 0)
    {
        return;
    }

    if (!dt)
    {
        snprintf(ts_buf, (size_t)ts_buf_sz, "1970-01-01T00:00:00+05:30");
        return;
    }

    snprintf(ts_buf,
             (size_t)ts_buf_sz,
             "20%02u-%02u-%02uT%02u:%02u:%02u+05:30",
             (unsigned int)(dt->year - 2000U),
             dt->month,
             dt->day,
             dt->hour,
             dt->minute,
             dt->second);
}

#if RTC_CODE_TIME_SET_ENABLED
static int rtc_datetime_compare(const rtc_datetime_t *a, const rtc_datetime_t *b)
{
    if (a->year != b->year) return (a->year < b->year) ? -1 : 1;
    if (a->month != b->month) return (a->month < b->month) ? -1 : 1;
    if (a->day != b->day) return (a->day < b->day) ? -1 : 1;
    if (a->hour != b->hour) return (a->hour < b->hour) ? -1 : 1;
    if (a->minute != b->minute) return (a->minute < b->minute) ? -1 : 1;
    if (a->second != b->second) return (a->second < b->second) ? -1 : 1;
    return 0;
}
#endif

static uint8_t rtc_build_datetime(rtc_datetime_t *dt)
{
    if (!dt)
    {
        return 0U;
    }

    char mon[4] = {0};
    int day = 1;
    int year = 2026;
    int hour = 0;
    int minute = 0;
    int second = 0;

    if (sscanf(__DATE__, "%3s %d %d", mon, &day, &year) != 3)
    {
        return 0U;
    }

    if (sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second) != 3)
    {
        return 0U;
    }

    dt->year = (uint16_t)year;
    dt->month = rtc_month_from_build_str(mon);
    dt->day = (uint8_t)day;
    dt->hour = (uint8_t)hour;
    dt->minute = (uint8_t)minute;
    dt->second = (uint8_t)second;
    return 1U;
}

int rtc_clock_configure_with_fallback(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_PeriphCLKInitTypeDef pclk = {0};

    rtc_enable_backup_domain_access();

    uint32_t current_src = rtc_get_clock_source();

    /*
     * If RTC clock source is already selected, do not reset backup domain.
     * This prevents RTC time loss on every boot.
     */
    if (current_src != 0U)
    {
        __HAL_RCC_RTC_ENABLE();

        print("[RTC] Existing clock source: %s\r\n",
              rtc_clock_name(current_src));

        return (current_src == RCC_BDCR_RTCSEL_0) ? HAL_OK : HAL_ERROR;
    }

    /*
     * First try LSE.
     * LSE is preferred because it can keep RTC running from VBAT.
     */
    print("[RTC] No RTC clock selected. Trying LSE...\r\n");

    osc.OscillatorType = RCC_OSCILLATORTYPE_LSE;
    osc.LSEState = RCC_LSE_ON;

    if (HAL_RCC_OscConfig(&osc) == HAL_OK)
    {
        pclk.PeriphClockSelection = RCC_PERIPHCLK_RTC;
        pclk.RTCClockSelection = RCC_RTCCLKSOURCE_LSE;

        if (HAL_RCCEx_PeriphCLKConfig(&pclk) == HAL_OK)
        {
            __HAL_RCC_RTC_ENABLE();
            print("[RTC] LSE selected\r\n");
            return HAL_OK;
        }
    }

    /*
     * Fallback to LSI.
     * LSI works for timestamp during power-on, but it will not preserve
     * accurate time across full power loss like LSE + VBAT.
     */
    print("[RTC][WARN] LSE failed. Falling back to LSI.\r\n");

    memset(&osc, 0, sizeof(osc));
    memset(&pclk, 0, sizeof(pclk));

    osc.OscillatorType = RCC_OSCILLATORTYPE_LSI;
    osc.LSIState = RCC_LSI_ON;

    if (HAL_RCC_OscConfig(&osc) != HAL_OK)
    {
        print("[RTC][ERR] LSI oscillator start failed\r\n");
        return HAL_ERROR;
    }

    pclk.PeriphClockSelection = RCC_PERIPHCLK_RTC;
    pclk.RTCClockSelection = RCC_RTCCLKSOURCE_LSI;

    if (HAL_RCCEx_PeriphCLKConfig(&pclk) != HAL_OK)
    {
        print("[RTC][ERR] RTC LSI clock select failed\r\n");
        return HAL_ERROR;
    }

    __HAL_RCC_RTC_ENABLE();

    print("[RTC] LSI selected\r\n");
    return HAL_ERROR;
}

void rtc_init_with_bkp_once(void)
{
    rtc_enable_backup_domain_access();

    hrtc.Instance = RTC;
    hrtc.Init.HourFormat = RTC_HOURFORMAT_24;

    /*
     * These prescalers are correct for 32768 Hz LSE.
     * With LSI they are approximate, but acceptable as fallback.
     */
    hrtc.Init.AsynchPrediv = 127;
    hrtc.Init.SynchPrediv = 255;

    hrtc.Init.OutPut = RTC_OUTPUT_DISABLE;
    hrtc.Init.OutPutPolarity = RTC_OUTPUT_POLARITY_HIGH;
    hrtc.Init.OutPutType = RTC_OUTPUT_TYPE_OPENDRAIN;

    if (HAL_RTC_Init(&hrtc) != HAL_OK)
    {
        print("[RTC][ERR] HAL_RTC_Init failed\r\n");
        return;
    }

    if (HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR1) == RTC_BKP_MAGIC)
    {
        s_rtc_backup_magic_seen = 1U;
        print("[RTC] Backup magic OK. Time preserved.\r\n");
        return;
    }

    s_rtc_backup_magic_seen = 0U;
    s_rtc_time_preserved = 0U;

    /*
     * First boot/default time.
     * Later we can update RTC from cloud/config/downlink.
     */
    print("[RTC] First-time init. Setting default time.\r\n");

    RTC_TimeTypeDef t = {0};
    RTC_DateTypeDef d = {0};

    t.Hours = 13;
    t.Minutes = 23;
    t.Seconds = 0;
    t.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
    t.StoreOperation = RTC_STOREOPERATION_RESET;

    d.WeekDay = RTC_WEEKDAY_SATURDAY;
    d.Month = RTC_MONTH_JULY;
    d.Date = 11;
    d.Year = 26;

    if (HAL_RTC_SetTime(&hrtc, &t, RTC_FORMAT_BIN) != HAL_OK)
    {
        print("[RTC][ERR] SetTime failed\r\n");
        return;
    }

    if (HAL_RTC_SetDate(&hrtc, &d, RTC_FORMAT_BIN) != HAL_OK)
    {
        print("[RTC][ERR] SetDate failed\r\n");
        return;
    }

    /* The default date is a placeholder: the clock stays INVALID (no backup
     * magic) until network time, NTP or a platform command sets it. Before
     * 2026-11-19 this wrote the magic, so a unit that lost its backup
     * battery ran on 11 Jul 2026 13:23 while reporting a valid clock. */
    print("[RTC] Default time set; clock invalid until set from network/NTP/platform.\r\n");
}

uint32_t rtc_increment_boot_count(void)
{
    rtc_enable_backup_domain_access();

    uint32_t count = HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_BOOT_COUNT_DR);

    if (count == 0xFFFFFFFFUL || count > 1000000000UL) {
        count = 0U;
    }

    count++;

    HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_BOOT_COUNT_DR, count);

    print("[RTC] Boot count=%lu\r\n", (unsigned long)count);

    return count;
}

uint8_t rtc_is_datetime_valid(void)
{
    rtc_enable_backup_domain_access();

    if (HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR1) != RTC_BKP_MAGIC)
    {
        return 0U;
    }

    RTC_TimeTypeDef t = {0};
    RTC_DateTypeDef d = {0};

    if (HAL_RTC_GetTime(&hrtc, &t, RTC_FORMAT_BIN) != HAL_OK)
    {
        return 0U;
    }

    if (HAL_RTC_GetDate(&hrtc, &d, RTC_FORMAT_BIN) != HAL_OK)
    {
        return 0U;
    }

    if (d.Year < 24U || d.Year > 99U)
    {
        return 0U;
    }

    if (d.Month < 1U || d.Month > 12U)
    {
        return 0U;
    }

    if (d.Date < 1U || d.Date > 31U)
    {
        return 0U;
    }

    return 1U;
}

uint8_t rtc_time_was_preserved(void)
{
    return s_rtc_time_preserved;
}

uint8_t rtc_get_datetime(rtc_datetime_t *dt)
{
    if (!dt)
    {
        return 0U;
    }

    RTC_TimeTypeDef t = {0};
    RTC_DateTypeDef d = {0};

    if (HAL_RTC_GetTime(&hrtc, &t, RTC_FORMAT_BIN) != HAL_OK)
    {
        return 0U;
    }

    if (HAL_RTC_GetDate(&hrtc, &d, RTC_FORMAT_BIN) != HAL_OK)
    {
        return 0U;
    }

    if (d.Year < 24U || d.Year > 99U || d.Month < 1U || d.Month > 12U || d.Date < 1U || d.Date > 31U)
    {
        return 0U;
    }

    dt->year = (uint16_t)(2000U + d.Year);
    dt->month = d.Month;
    dt->day = d.Date;
    dt->hour = t.Hours;
    dt->minute = t.Minutes;
    dt->second = t.Seconds;
    return 1U;
}

uint8_t rtc_set_datetime_ist(uint16_t year,
                             uint8_t month,
                             uint8_t day,
                             uint8_t hour,
                             uint8_t minute,
                             uint8_t second)
{
    if (year < 2000U || year > 2099U || month < 1U || month > 12U || day < 1U || day > 31U ||
        hour > 23U || minute > 59U || second > 59U)
    {
        return 0U;
    }

    RTC_TimeTypeDef t = {0};
    RTC_DateTypeDef d = {0};

    t.Hours = hour;
    t.Minutes = minute;
    t.Seconds = second;
    t.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
    t.StoreOperation = RTC_STOREOPERATION_RESET;

    d.WeekDay = rtc_weekday_from_date(year, month, day);
    d.Month = month;
    d.Date = day;
    d.Year = (uint8_t)(year - 2000U);

    if (HAL_RTC_SetTime(&hrtc, &t, RTC_FORMAT_BIN) != HAL_OK)
    {
        print("[RTC][ERR] SetTime failed\r\n");
        return 0U;
    }

    if (HAL_RTC_SetDate(&hrtc, &d, RTC_FORMAT_BIN) != HAL_OK)
    {
        print("[RTC][ERR] SetDate failed\r\n");
        return 0U;
    }

    HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR1, RTC_BKP_MAGIC);
    return 1U;
}

uint8_t rtc_sync_to_code_time_once(void)
{
#if RTC_CODE_TIME_SET_ENABLED
    rtc_enable_backup_domain_access();

    rtc_datetime_t current = {0};
    rtc_datetime_t code_time = {
        .year = (uint16_t)RTC_CODE_TIME_YEAR,
        .month = (uint8_t)RTC_CODE_TIME_MONTH,
        .day = (uint8_t)RTC_CODE_TIME_DAY,
        .hour = (uint8_t)RTC_CODE_TIME_HOUR,
        .minute = (uint8_t)RTC_CODE_TIME_MINUTE,
        .second = (uint8_t)RTC_CODE_TIME_SECOND
    };

    uint32_t date_stamp = (RTC_CODE_TIME_YEAR * 10000UL) +
                          (RTC_CODE_TIME_MONTH * 100UL) +
                          RTC_CODE_TIME_DAY;
    uint32_t time_stamp = (RTC_CODE_TIME_HOUR * 10000UL) +
                          (RTC_CODE_TIME_MINUTE * 100UL) +
                          RTC_CODE_TIME_SECOND;

    if (HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_CODE_TIME_DATE_DR) == date_stamp &&
        HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_CODE_TIME_TIME_DR) == time_stamp &&
        rtc_get_datetime(&current) &&
        rtc_datetime_compare(&current, &code_time) >= 0)
    {
        char current_ts[32];

        rtc_format_datetime_ist(current_ts, sizeof(current_ts), &current);
        s_rtc_time_preserved = s_rtc_backup_magic_seen ? 1U : 0U;
        print("[RTC] code time already applied date=%lu time=%lu current=%s\r\n",
              (unsigned long)date_stamp,
              (unsigned long)time_stamp,
              current_ts);
        return 1U;
    }

    if (HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_CODE_TIME_DATE_DR) == date_stamp &&
        HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_CODE_TIME_TIME_DR) == time_stamp)
    {
        if (rtc_get_datetime(&current))
        {
            char current_ts[32];
            char target_ts[32];

            rtc_format_datetime_ist(current_ts, sizeof(current_ts), &current);
            rtc_format_datetime_ist(target_ts, sizeof(target_ts), &code_time);
            print("[RTC][WARN] code time stamp matched but RTC is older current=%s target=%s; reapplying\r\n",
                  current_ts,
                  target_ts);
        }
        else
        {
            print("[RTC][WARN] code time stamp matched but RTC is invalid; reapplying\r\n");
        }
    }

    if (!rtc_set_datetime_ist((uint16_t)RTC_CODE_TIME_YEAR,
                              (uint8_t)RTC_CODE_TIME_MONTH,
                              (uint8_t)RTC_CODE_TIME_DAY,
                              (uint8_t)RTC_CODE_TIME_HOUR,
                              (uint8_t)RTC_CODE_TIME_MINUTE,
                              (uint8_t)RTC_CODE_TIME_SECOND))
    {
        print("[RTC][ERR] code time set failed\r\n");
        return 0U;
    }

    HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_CODE_TIME_DATE_DR, date_stamp);
    HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_CODE_TIME_TIME_DR, time_stamp);
    s_rtc_time_preserved = 0U;

    {
        char code_ts[32];

        rtc_format_datetime_ist(code_ts, sizeof(code_ts), &code_time);
        print("[RTC] synced to code time %s\r\n", code_ts);
    }

    return 1U;
#else
    print("[RTC] code time set disabled\r\n");
    return 0U;
#endif
}

void rtc_sync_to_build_time_if_newer(void)
{
    rtc_datetime_t build = {0};
    rtc_datetime_t current = {0};

    if (rtc_get_datetime(&current))
    {
        char current_ts[32];

        rtc_format_datetime_ist(current_ts, sizeof(current_ts), &current);
        s_rtc_time_preserved = s_rtc_backup_magic_seen ? 1U : 0U;
        print("[RTC] %s; build time sync skipped current=%s\r\n",
              rtc_is_datetime_valid() ? "valid RTC present" : "RTC not set yet (waiting for network/NTP/platform time)",
              current_ts);
        return;
    }

    if (!rtc_build_datetime(&build))
    {
        print("[RTC][WARN] build time parse failed\r\n");
        s_rtc_time_preserved = 0U;
        return;
    }

    if (rtc_set_datetime_ist(build.year,
                             build.month,
                             build.day,
                             build.hour,
                             build.minute,
                             build.second))
    {
        char build_ts[32];

        rtc_format_datetime_ist(build_ts, sizeof(build_ts), &build);
        print("[RTC] synced invalid RTC to build time %s\r\n",
              build_ts);
    }
    s_rtc_time_preserved = 0U;
}

void rtc_get_datetime_strings(char *ts_buf, int ts_buf_sz)
{
    if (!ts_buf || ts_buf_sz <= 0)
    {
        return;
    }

    rtc_datetime_t dt = {0};

    if (!rtc_get_datetime(&dt))
    {
        snprintf(ts_buf, (size_t)ts_buf_sz, "1970-01-01T00:00:00+05:30");
        return;
    }

    rtc_format_datetime_ist(ts_buf, ts_buf_sz, &dt);
}

/* Set the RTC (kept in IST) from a UTC Unix time. Used when no network or
 * NTP time is available yet: the platform's send time of a command
 * (issuedAt) is then the best clock there is. 1 on success. */
uint8_t rtc_set_from_utc_epoch(int64_t utc_s)
{
    if (utc_s < 1735689600LL) return 0U; /* before 2025-01-01: not a real time */
    int64_t ist_s = utc_s + (5 * 3600 + 30 * 60);
    int64_t z = ist_s / 86400 + 719468;
    int64_t secs = ist_s % 86400;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp + (mp < 10 ? 3 : -9);
    if (m <= 2) y++;
    return rtc_set_datetime_ist((uint16_t)y, (uint8_t)m, (uint8_t)d,
                                (uint8_t)(secs / 3600), (uint8_t)((secs % 3600) / 60), (uint8_t)(secs % 60));
}

