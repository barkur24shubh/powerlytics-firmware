#include "comm_manager.h"
#include "app_gateway_platform.h"

#include "cmsis_os.h"
#include "stm32f4xx_hal.h"

#include "app_log.h"
#include "app_modbus_payload.h"
#include "config.h"
#include "debug_uart.h"
#include "modem_handler.h"
#include "mqtt_handler.h"

#include <stdio.h>
#include <string.h>

/* =========================================================
 * Communication recovery policy
 * =========================================================
 *
 * Recovery ladder:
 *   1. Soft retry with exponential backoff
 *   2. MQTT reconnect
 *   3. GPRS/PDP reattach
 *   4. Network/SIM/AT sync retry
 *   5. AT soft modem reset from:
 *        - AT sync failure
 *        - network registration/attach failure after threshold
 *        - MQTT connect failure after the full lower-layer ladder was retried
 *   6. MCU reset only if modem reset recovery itself keeps failing
 *
 * Important:
 *   - MQTT failure does not immediately soft reset modem.
 *   - Publish/MQTT failures move backward through the ladder first, then reset
 *     the modem if the MQTT engine is still stuck.
 *   - Soft modem reset is a last fallback, not the first MQTT retry action.
 */

#define COMM_STARTUP_DELAY_MS              500UL

#define COMM_BACKOFF_INITIAL_MS            2000UL
#define COMM_BACKOFF_MAX_MQTT_MS           30000UL
#define COMM_BACKOFF_MAX_NETWORK_MS        60000UL
#define COMM_RECOVERY_SOFT_RESET_MAX_MS    (5UL * 60UL * 1000UL)
#define MODEM_BOOT_GRACE_MS                90000UL
#define MODEM_RESET_MIN_INTERVAL_MS        120000UL

#define ENABLE_MODEM_SOFT_RESET            1
#define RESET_MODEM_ON_COMM_STARTUP        0
#define TEST_FORCE_SOFT_RESET_ON_SIM_FAIL  0

#define THRESHOLD_3X                       3U
#define THRESHOLD_5X                       5U
#define THRESHOLD_STILL_FAILING_STEP       10U

/*
 * Soft reset trigger points.
 * MQTT reset is allowed only after the full recovery ladder is exhausted.
 */
#define AT_SOFT_RESET_THRESHOLD            3U
#define NETWORK_SOFT_RESET_THRESHOLD       5U
#define SIM_SOFT_RESET_THRESHOLD           5U
#define GPRS_SOFT_RESET_THRESHOLD          4U
#define MQTT_CONNECT_SOFT_RESET_THRESHOLD  5U
#define MQTT_SUBSCRIBE_SOFT_RESET_THRESHOLD 5U
#define MQTT_PUBLISH_SOFT_RESET_THRESHOLD  5U
#define MQTT_DISCONNECT_SOFT_RESET_THRESHOLD 3U
#define MODEM_RESET_MCU_RESET_THRESHOLD    3U

/* =========================================================
 * Internal state
 * ========================================================= */
static comm_state_t  s_state = COMM_STATE_BOOT;
static comm_state_t  s_retry_state = COMM_STATE_MODEM_SYNC;
static comm_error_t  s_last_error = COMM_ERR_NONE;
static comm_health_t s_health;

static uint32_t s_next_action_tick = 0;
static uint32_t s_backoff_ms = COMM_BACKOFF_INITIAL_MS;
static uint32_t s_backoff_max_ms = COMM_BACKOFF_MAX_MQTT_MS;
static uint8_t  s_recovery_active = 0;
static uint32_t s_recovery_start_tick = 0;
static comm_error_t s_recovery_root_error = COMM_ERR_NONE;
static uint32_t s_modem_boot_grace_until_tick = 0;
static uint32_t s_last_modem_reset_request_tick = 0;

/* Consecutive failure counters used for staged recovery. */
static uint8_t s_consecutive_at_fail = 0;
static uint8_t s_consecutive_sim_fail = 0;
static uint8_t s_consecutive_network_fail = 0;
static uint8_t s_consecutive_gprs_fail = 0;
static uint8_t s_consecutive_mqtt_connect_fail = 0;
static uint8_t s_consecutive_mqtt_subscribe_fail = 0;
static uint8_t s_consecutive_publish_fail = 0;
static uint8_t s_consecutive_mqtt_disconnect_fail = 0;
static uint8_t s_consecutive_soft_reset_fail = 0;

extern char imei[20];

/* =========================================================
 * Small helpers
 * ========================================================= */
static uint8_t time_reached(uint32_t tick)
{
    return ((int32_t)(HAL_GetTick() - tick) >= 0) ? 1U : 0U;
}

static uint8_t should_log_still_failing(uint8_t count)
{
    return ((count > THRESHOLD_5X) &&
            ((count % THRESHOLD_STILL_FAILING_STEP) == 0U)) ? 1U : 0U;
}

static void recovery_mark_failure(comm_error_t err)
{
    if (err == COMM_ERR_NONE) {
        return;
    }

    if (!s_recovery_active) {
        s_recovery_active = 1U;
        s_recovery_start_tick = HAL_GetTick();
        s_recovery_root_error = err;

        print("[COMM][RECOVERY] start root=%s max=%lu ms\r\n",
              comm_manager_error_name(err),
              (unsigned long)COMM_RECOVERY_SOFT_RESET_MAX_MS);
    }
}

static void recovery_clear(void)
{
    if (s_recovery_active) {
        print("[COMM][RECOVERY] clear root=%s age=%lu ms\r\n",
              comm_manager_error_name(s_recovery_root_error),
              (unsigned long)(HAL_GetTick() - s_recovery_start_tick));
    }

    s_recovery_active = 0U;
    s_recovery_start_tick = 0U;
    s_recovery_root_error = COMM_ERR_NONE;
}

static void recovery_clear_if_not_publish(void)
{
    if (s_recovery_root_error != COMM_ERR_MQTT_PUBLISH_FAILED) {
        recovery_clear();
    }
}

static void modem_boot_grace_start(const char *reason)
{
    s_modem_boot_grace_until_tick = HAL_GetTick() + MODEM_BOOT_GRACE_MS;

    print("[COMM][MODEM_BOOT] grace start reason=%s duration=%lu ms\r\n",
          reason ? reason : "",
          (unsigned long)MODEM_BOOT_GRACE_MS);
}

static uint8_t modem_boot_grace_active(void)
{
    return ((int32_t)(s_modem_boot_grace_until_tick - HAL_GetTick()) > 0) ? 1U : 0U;
}

static uint32_t modem_boot_grace_remaining_ms(void)
{
    if (!modem_boot_grace_active()) {
        return 0UL;
    }

    return s_modem_boot_grace_until_tick - HAL_GetTick();
}

static void set_state(comm_state_t next)
{
    if (s_state != next)
    {
        print("[COMM] %s -> %s\r\n",
              comm_manager_state_name(s_state),
              comm_manager_state_name(next));

        s_state = next;
        s_health.last_state_change_tick = HAL_GetTick();
    }
}

static void reset_backoff(void)
{
    s_backoff_ms = COMM_BACKOFF_INITIAL_MS;
}

static void enter_backoff(comm_state_t retry_state,
                          comm_error_t err,
                          uint32_t max_backoff_ms)
{
    s_last_error = err;
    recovery_mark_failure(err);

    s_retry_state = retry_state;
    s_backoff_max_ms = max_backoff_ms;
    s_next_action_tick = HAL_GetTick() + s_backoff_ms;

    print("[COMM][BACKOFF] err=%s retry=%s delay=%lu ms\r\n",
          comm_manager_error_name(err),
          comm_manager_state_name(retry_state),
          (unsigned long)s_backoff_ms);

    if (s_backoff_ms < s_backoff_max_ms)
    {
        s_backoff_ms *= 2UL;
        if (s_backoff_ms > s_backoff_max_ms)
        {
            s_backoff_ms = s_backoff_max_ms;
        }
    }

    set_state(COMM_STATE_BACKOFF);
}

static uint8_t network_registered(void)
{
    /*
     * First query flushes old URCs.
     * Later queries do not flush to avoid clearing useful response fragments.
     */
    if (SIM800_SendATCommand2("AT+CGREG?", "0,1", 5000, true))  return 1U;
    if (SIM800_SendATCommand2("AT+CGREG?", "0,5", 5000, false)) return 1U;
    if (SIM800_SendATCommand2("AT+CEREG?", "0,1", 5000, false)) return 1U;
    if (SIM800_SendATCommand2("AT+CEREG?", "0,5", 5000, false)) return 1U;

    return 0U;
}

static void log_threshold_failure(app_log_level_t level,
                                  const char *module,
                                  const char *event,
                                  const char *message,
                                  uint8_t consecutive_count,
                                  uint32_t total_count,
                                  const char *recovery)
{
    app_log_critical(level,
                     module,
                     event,
                     message,
                     consecutive_count,
                     total_count,
                     recovery);
}

static void clear_network_counters(void)
{
    s_consecutive_at_fail = 0;
    s_consecutive_sim_fail = 0;
    s_consecutive_network_fail = 0;
    s_consecutive_gprs_fail = 0;
}

static void clear_mqtt_counters(void)
{
    s_consecutive_mqtt_connect_fail = 0;
    s_consecutive_mqtt_subscribe_fail = 0;
    s_consecutive_publish_fail = 0;
    s_consecutive_mqtt_disconnect_fail = 0;
}

static void clear_all_failure_counters(void)
{
    clear_network_counters();
    clear_mqtt_counters();
}

static void request_soft_reset(comm_error_t err,
                               const char *event,
                               const char *message,
                               uint8_t consecutive_count,
                               uint32_t total_count)
{
#if ENABLE_MODEM_SOFT_RESET
    uint32_t now = HAL_GetTick();

    if ((s_last_modem_reset_request_tick != 0UL) &&
        ((now - s_last_modem_reset_request_tick) < MODEM_RESET_MIN_INTERVAL_MS))
    {
        uint32_t wait_ms = MODEM_RESET_MIN_INTERVAL_MS - (now - s_last_modem_reset_request_tick);

        print("[COMM] modem reset suppressed, cooldown remaining=%lu ms\r\n",
              (unsigned long)wait_ms);

        enter_backoff(COMM_STATE_MODEM_SYNC,
                      err,
                      COMM_BACKOFF_MAX_NETWORK_MS);
        return;
    }

    s_last_modem_reset_request_tick = now;
    modem_boot_grace_start(event);

    app_log_critical(APP_LOG_FATAL,
                     "MODEM",
                     event,
                     message,
                     consecutive_count,
                     total_count,
                     "AT_CFUN_RESET");

    reset_backoff();
    enter_backoff(COMM_STATE_MODEM_SOFT_RESET,
                  err,
                  COMM_BACKOFF_MAX_NETWORK_MS);
#else
    print("[COMM] soft reset disabled, using MODEM_SYNC fallback\r\n");

    reset_backoff();
    enter_backoff(COMM_STATE_MODEM_SYNC,
                  err,
                  COMM_BACKOFF_MAX_NETWORK_MS);
#endif
}

static uint8_t recovery_timeout_check(void)
{
    if (!s_recovery_active) {
        return 0U;
    }

    if ((s_state == COMM_STATE_MODEM_SOFT_RESET) ||
        (s_retry_state == COMM_STATE_MODEM_SOFT_RESET)) {
        return 0U;
    }

    if ((HAL_GetTick() - s_recovery_start_tick) < COMM_RECOVERY_SOFT_RESET_MAX_MS) {
        return 0U;
    }

    request_soft_reset(s_recovery_root_error,
                       "SOFT_RESET_TRIGGERED_RECOVERY_TIMEOUT",
                       "communication recovery exceeded time limit, soft resetting modem",
                       0,
                       (uint32_t)(HAL_GetTick() - s_recovery_start_tick));

    return 1U;
}

/* =========================================================
 * Public API
 * ========================================================= */
void comm_manager_init(void)
{
    memset(&s_health, 0, sizeof(s_health));

    s_state = COMM_STATE_BOOT;
    s_retry_state = COMM_STATE_MODEM_SYNC;
    s_last_error = COMM_ERR_NONE;
    s_next_action_tick = HAL_GetTick() + COMM_STARTUP_DELAY_MS;
    s_consecutive_soft_reset_fail = 0;

    reset_backoff();
    clear_all_failure_counters();
    recovery_clear();

    print("[COMM] manager init\r\n");
}

void comm_manager_run_once(void)
{
    if (recovery_timeout_check()) {
        return;
    }

    switch (s_state)
    {
    case COMM_STATE_BOOT:
        if (time_reached(s_next_action_tick))
        {
#if RESET_MODEM_ON_COMM_STARTUP
            print("[COMM] startup modem reset for MCU/modem sync\r\n");

            app_log_push(APP_LOG_INFO,
                         "MODEM",
                         "STARTUP_RESET",
                         "startup modem reset for MCU/modem sync",
                         0,
                         0);

            s_last_modem_reset_request_tick = HAL_GetTick();
            modem_boot_grace_start("STARTUP_RESET");
            (void)Modem_SoftReset();
            imei[0] = '\0';
            clear_all_failure_counters();
            recovery_clear();
            reset_backoff();
#endif
            set_state(COMM_STATE_MODEM_SYNC);
        }
        break;

    case COMM_STATE_MODEM_SYNC:
        /*
         * Soft AT sync.
         * Plain AT is only a short wake/primer. Some modem firmware echoes AT
         * but does not return OK until echo is disabled with ATE0.
         */
        if (!SIM800_SendATCommand2("AT", "OK", 500, false))
        {
            if (modem_boot_grace_active())
            {
                print("[COMM] plain AT primer missed during modem boot grace, remaining=%lu ms\r\n",
                      (unsigned long)modem_boot_grace_remaining_ms());
            }
            else
            {
                print("[COMM] plain AT primer missed OK; trying ATE0 as real sync\r\n");

                app_log_push(APP_LOG_WARN,
                             "COMM",
                             "AT_SOFT_MISS",
                             "plain AT primer missed OK, trying ATE0",
                             0,
                             0);
            }
        }

        if (!SIM800_SendATCommand2("ATE0", "OK", 5000, true))
        {
            if (modem_boot_grace_active())
            {
                print("[COMM] ATE0 missed during modem boot grace, remaining=%lu ms\r\n",
                      (unsigned long)modem_boot_grace_remaining_ms());

                enter_backoff(COMM_STATE_MODEM_SYNC,
                              COMM_ERR_NONE,
                              COMM_BACKOFF_MAX_MQTT_MS);
                break;
            }

            s_health.at_fail_count++;
            s_consecutive_at_fail++;

            print("[COMM] ATE0 failed consecutive=%u total=%lu\r\n",
                  s_consecutive_at_fail,
                  (unsigned long)s_health.at_fail_count);

            app_log_push(APP_LOG_ERROR,
                         "COMM",
                         "AT_TIMEOUT",
                         "ATE0 failed, modem AT timeout",
                         s_consecutive_at_fail,
                         s_health.at_fail_count);

            if (s_consecutive_at_fail == THRESHOLD_3X)
            {
                log_threshold_failure(APP_LOG_ERROR,
                                      "COMM",
                                      "AT_TIMEOUT_3X",
                                      "ATE0 failed 3 times",
                                      s_consecutive_at_fail,
                                      s_health.at_fail_count,
                                      "MODEM_SOFT_RESET");
            }
            else if (should_log_still_failing(s_consecutive_at_fail))
            {
                log_threshold_failure(APP_LOG_ERROR,
                                      "COMM",
                                      "AT_STILL_FAILING",
                                      "modem AT sync is still failing",
                                      s_consecutive_at_fail,
                                      s_health.at_fail_count,
                                      "MODEM_SOFT_RESET");
            }

            /*
             * AT level is allowed to trigger modem soft reset.
             * If ATE0 itself fails repeatedly, modem may be stuck.
             */
            if (s_consecutive_at_fail >= AT_SOFT_RESET_THRESHOLD)
            {
                request_soft_reset(COMM_ERR_AT_TIMEOUT,
                                   "SOFT_RESET_TRIGGERED_AT",
                                   "AT sync failed repeatedly, soft resetting modem",
                                   s_consecutive_at_fail,
                                   s_health.at_fail_count);
                break;
            }

            enter_backoff(COMM_STATE_MODEM_SYNC,
                          COMM_ERR_AT_TIMEOUT,
                          COMM_BACKOFF_MAX_NETWORK_MS);
            break;
        }

        s_consecutive_at_fail = 0;
        s_consecutive_soft_reset_fail = 0;
        s_modem_boot_grace_until_tick = 0UL;

        if (imei[0] == '\0')
        {
            GetIMEI();
        }

        Prepare_MQTT_Topics();

        reset_backoff();
        set_state(COMM_STATE_SIM_CHECK);
        break;

    case COMM_STATE_SIM_CHECK:
        if (!SIM800_SendATCommand2("AT+CPIN?", "READY", 5000, true))
        {
            s_health.sim_not_ready_count++;
            s_consecutive_sim_fail++;

			#if TEST_FORCE_SOFT_RESET_ON_SIM_FAIL
			if (s_consecutive_sim_fail == 2U)
			{
				app_log_critical(APP_LOG_FATAL,
								 "TEST",
								 "FORCED_SOFT_RESET",
								 "forcing modem soft reset for test",
								 s_consecutive_sim_fail,
								 s_health.sim_not_ready_count,
								 "AT_CFUN_RESET");

				enter_backoff(COMM_STATE_MODEM_SOFT_RESET,
							  COMM_ERR_SIM_NOT_READY,
							  COMM_BACKOFF_INITIAL_MS);
				break;
			}
			#endif

            print("[COMM] SIM not ready consecutive=%u total=%lu\r\n",
                  s_consecutive_sim_fail,
                  (unsigned long)s_health.sim_not_ready_count);

            app_log_push(APP_LOG_WARN,
                         "COMM",
                         "SIM_NOT_READY",
                         "SIM not ready",
                         s_consecutive_sim_fail,
                         s_health.sim_not_ready_count);

            if (s_consecutive_sim_fail == THRESHOLD_3X)
            {
                log_threshold_failure(APP_LOG_ERROR,
                                      "COMM",
                                      "SIM_NOT_READY_3X",
                                      "SIM not ready 3 times",
                                      s_consecutive_sim_fail,
                                      s_health.sim_not_ready_count,
                                      "MODEM_SYNC");
            }
            else if (s_consecutive_sim_fail == THRESHOLD_5X)
            {
                log_threshold_failure(APP_LOG_FATAL,
                                      "COMM",
                                      "SIM_NOT_READY_5X",
                                      "SIM not ready 5 times",
                                      s_consecutive_sim_fail,
                                      s_health.sim_not_ready_count,
                                      "MODEM_SYNC");
            }
            else if (should_log_still_failing(s_consecutive_sim_fail))
            {
                log_threshold_failure(APP_LOG_ERROR,
                                      "COMM",
                                      "SIM_STILL_NOT_READY",
                                      "SIM is still not ready",
                                      s_consecutive_sim_fail,
                                      s_health.sim_not_ready_count,
                                      "MODEM_SYNC");
            }

            /*
             * SIM may be physically missing, so do not reset immediately.
             * But if the modem SIM interface is wedged, a modem reset can
             * recover it. Escalate only after a bounded retry ladder.
             */
            if (s_consecutive_sim_fail >= SIM_SOFT_RESET_THRESHOLD)
            {
                request_soft_reset(COMM_ERR_SIM_NOT_READY,
                                   "SOFT_RESET_TRIGGERED_SIM",
                                   "SIM not ready repeatedly, soft resetting modem",
                                   s_consecutive_sim_fail,
                                   s_health.sim_not_ready_count);
            }
            else if (s_consecutive_sim_fail < THRESHOLD_5X)
            {
                enter_backoff(COMM_STATE_SIM_CHECK,
                              COMM_ERR_SIM_NOT_READY,
                              COMM_BACKOFF_MAX_NETWORK_MS);
            }
            else
            {
                enter_backoff(COMM_STATE_MODEM_SYNC,
                              COMM_ERR_SIM_NOT_READY,
                              COMM_BACKOFF_MAX_NETWORK_MS);
            }
            break;
        }

        s_consecutive_sim_fail = 0;
        reset_backoff();
        set_state(COMM_STATE_NETWORK_WAIT);
        break;

    case COMM_STATE_NETWORK_WAIT:
        if (!network_registered())
        {
            s_health.network_reg_fail_count++;
            s_consecutive_network_fail++;

            print("[COMM] network registration failed consecutive=%u total=%lu\r\n",
                  s_consecutive_network_fail,
                  (unsigned long)s_health.network_reg_fail_count);

            app_log_push(APP_LOG_WARN,
                         "COMM",
                         "NETWORK_NOT_REGISTERED",
                         "network registration failed",
                         s_consecutive_network_fail,
                         s_health.network_reg_fail_count);

            if (s_consecutive_network_fail == THRESHOLD_3X)
            {
                log_threshold_failure(APP_LOG_ERROR,
                                      "COMM",
                                      "NETWORK_REG_FAILED_3X",
                                      "network registration failed 3 times",
                                      s_consecutive_network_fail,
                                      s_health.network_reg_fail_count,
                                      "MODEM_SYNC");
            }
            else if (s_consecutive_network_fail == THRESHOLD_5X)
            {
                log_threshold_failure(APP_LOG_FATAL,
                                      "COMM",
                                      "NETWORK_REG_FAILED_5X",
                                      "network registration failed 5 times",
                                      s_consecutive_network_fail,
                                      s_health.network_reg_fail_count,
                                      "MODEM_SOFT_RESET");
            }
            else if (should_log_still_failing(s_consecutive_network_fail))
            {
                log_threshold_failure(APP_LOG_ERROR,
                                      "COMM",
                                      "NETWORK_STILL_NOT_REGISTERED",
                                      "network registration is still failing",
                                      s_consecutive_network_fail,
                                      s_health.network_reg_fail_count,
                                      "MODEM_SOFT_RESET");
            }

            /*
             * Network registration/attach level is allowed to trigger soft reset,
             * because this is already below MQTT/GPRS fallback in the ladder.
             */
            if (s_consecutive_network_fail >= NETWORK_SOFT_RESET_THRESHOLD)
            {
                request_soft_reset(COMM_ERR_NETWORK_NOT_REGISTERED,
                                   "SOFT_RESET_TRIGGERED_NETWORK",
                                   "network attach failed repeatedly, soft resetting modem",
                                   s_consecutive_network_fail,
                                   s_health.network_reg_fail_count);
                break;
            }

            if (s_consecutive_network_fail < THRESHOLD_3X)
            {
                enter_backoff(COMM_STATE_NETWORK_WAIT,
                              COMM_ERR_NETWORK_NOT_REGISTERED,
                              COMM_BACKOFF_MAX_NETWORK_MS);
            }
            else
            {
                enter_backoff(COMM_STATE_MODEM_SYNC,
                              COMM_ERR_NETWORK_NOT_REGISTERED,
                              COMM_BACKOFF_MAX_NETWORK_MS);
            }
            break;
        }

        s_consecutive_network_fail = 0;
        reset_backoff();
        set_state(COMM_STATE_GPRS_ATTACH);
        break;

    case COMM_STATE_GPRS_ATTACH:
        if (!GPRS_Init(APN))
        {
            s_health.gprs_fail_count++;
            s_consecutive_gprs_fail++;

            print("[COMM] GPRS failed consecutive=%u total=%lu\r\n",
                  s_consecutive_gprs_fail,
                  (unsigned long)s_health.gprs_fail_count);

            app_log_push(APP_LOG_ERROR,
                         "COMM",
                         "GPRS_FAILED",
                         "GPRS init failed",
                         s_consecutive_gprs_fail,
                         s_health.gprs_fail_count);

            if (s_consecutive_gprs_fail == THRESHOLD_3X)
            {
                log_threshold_failure(APP_LOG_ERROR,
                                      "COMM",
                                      "GPRS_FAILED_3X",
                                      "GPRS failed 3 times, going to modem sync",
                                      s_consecutive_gprs_fail,
                                      s_health.gprs_fail_count,
                                      "MODEM_SYNC");
            }
            else if (s_consecutive_gprs_fail == THRESHOLD_5X)
            {
                log_threshold_failure(APP_LOG_FATAL,
                                      "COMM",
                                      "GPRS_FAILED_5X",
                                      "GPRS failed 5 times, falling back to network attach",
                                      s_consecutive_gprs_fail,
                                      s_health.gprs_fail_count,
                                      "NETWORK_WAIT");
            }
            else if (should_log_still_failing(s_consecutive_gprs_fail))
            {
                log_threshold_failure(APP_LOG_ERROR,
                                      "COMM",
                                      "GPRS_STILL_FAILING",
                                      "GPRS init is still failing",
                                      s_consecutive_gprs_fail,
                                      s_health.gprs_fail_count,
                                      "NETWORK_WAIT");
            }

            /*
             * GPRS/PDP failure first moves backward through network and AT
             * sync. If those lower layers keep passing but PDP still fails,
             * reset modem because PDP context can be wedged.
             */
            if (s_consecutive_gprs_fail >= GPRS_SOFT_RESET_THRESHOLD)
            {
                request_soft_reset(COMM_ERR_GPRS_FAILED,
                                   "SOFT_RESET_TRIGGERED_GPRS",
                                   "GPRS/PDP failed repeatedly, soft resetting modem",
                                   s_consecutive_gprs_fail,
                                   s_health.gprs_fail_count);
            }
            else if (s_consecutive_gprs_fail < THRESHOLD_3X)
            {
                enter_backoff(COMM_STATE_NETWORK_WAIT,
                              COMM_ERR_GPRS_FAILED,
                              COMM_BACKOFF_MAX_NETWORK_MS);
            }
            else if (s_consecutive_gprs_fail < THRESHOLD_5X)
            {
                enter_backoff(COMM_STATE_MODEM_SYNC,
                              COMM_ERR_GPRS_FAILED,
                              COMM_BACKOFF_MAX_NETWORK_MS);
            }
            else
            {
                /*
                 * Push down to network attach level; network state decides
                 * whether soft reset is needed after its own threshold.
                 */
                enter_backoff(COMM_STATE_NETWORK_WAIT,
                              COMM_ERR_GPRS_FAILED,
                              COMM_BACKOFF_MAX_NETWORK_MS);
            }
            break;
        }

        s_consecutive_gprs_fail = 0;
        reset_backoff();
        set_state(COMM_STATE_MQTT_CONNECT);
        break;

    case COMM_STATE_MQTT_CONNECT:
        if (!MQTT_Connect(MQTT_HOST_URI,
                          MQTT_PORT,
                          MQTT_USERNAME,
                          MQTT_PASSWORD))
        {
            s_health.mqtt_connect_fail_count++;
            s_consecutive_mqtt_connect_fail++;

            print("[COMM] MQTT connect failed consecutive=%u total=%lu\r\n",
                  s_consecutive_mqtt_connect_fail,
                  (unsigned long)s_health.mqtt_connect_fail_count);

            app_log_push(APP_LOG_ERROR,
                         "MQTT",
                         "CONNECT_FAILED",
                         "MQTT connect failed",
                         s_consecutive_mqtt_connect_fail,
                         s_health.mqtt_connect_fail_count);

            /*
             * MQTT_Connect() itself already emits threshold critical logs.
             * Comm manager chooses the recovery level. MQTT failures first
             * fall back through MQTT/GPRS/MODEM_SYNC. If all lower layers
             * still look healthy but MQTT keeps failing, reset the modem
             * because the modem's MQTT/PDP engine may be wedged.
             */
            if (s_consecutive_mqtt_connect_fail < THRESHOLD_3X)
            {
                enter_backoff(COMM_STATE_MQTT_CONNECT,
                              COMM_ERR_MQTT_CONNECT_FAILED,
                              COMM_BACKOFF_MAX_MQTT_MS);
            }
            else if (s_consecutive_mqtt_connect_fail < THRESHOLD_5X)
            {
                enter_backoff(COMM_STATE_GPRS_ATTACH,
                              COMM_ERR_MQTT_CONNECT_FAILED,
                              COMM_BACKOFF_MAX_NETWORK_MS);
            }
            else if (s_consecutive_mqtt_connect_fail < MQTT_CONNECT_SOFT_RESET_THRESHOLD)
            {
                /*
                 * Full lower-layer fallback before modem reset. If MODEM_SYNC,
                 * SIM, network, and GPRS all pass, this counter is preserved
                 * so repeated MQTT engine failure can still trip the hard limit.
                 */
                enter_backoff(COMM_STATE_MODEM_SYNC,
                              COMM_ERR_MQTT_CONNECT_FAILED,
                              COMM_BACKOFF_MAX_NETWORK_MS);
            }
            else
            {
                request_soft_reset(COMM_ERR_MQTT_CONNECT_FAILED,
                                   "SOFT_RESET_TRIGGERED_MQTT",
                                   "MQTT connect failed repeatedly, soft resetting modem",
                                   s_consecutive_mqtt_connect_fail,
                                   s_health.mqtt_connect_fail_count);
            }
            break;
        }

        s_consecutive_mqtt_connect_fail = 0;
        reset_backoff();
        set_state(COMM_STATE_MQTT_SUBSCRIBE);
        break;

    case COMM_STATE_MQTT_SUBSCRIBE:
        if (!MQTT_Subscribe(NULL))
        {
            s_health.mqtt_subscribe_fail_count++;
            s_consecutive_mqtt_subscribe_fail++;

            print("[COMM] MQTT subscribe failed consecutive=%u total=%lu\r\n",
                  s_consecutive_mqtt_subscribe_fail,
                  (unsigned long)s_health.mqtt_subscribe_fail_count);

            app_log_push(APP_LOG_ERROR,
                         "MQTT",
                         "SUBSCRIBE_FAILED",
                         "MQTT subscribe failed",
                         s_consecutive_mqtt_subscribe_fail,
                         s_health.mqtt_subscribe_fail_count);

            /*
             * Subscribe failure follows the same bounded ladder as MQTT
             * connect failure. Do not clear the counter during lower-layer
             * fallbacks; clear it only after subscribe succeeds.
             */
            if (s_consecutive_mqtt_subscribe_fail < THRESHOLD_3X)
            {
                enter_backoff(COMM_STATE_MQTT_CONNECT,
                              COMM_ERR_MQTT_SUBSCRIBE_FAILED,
                              COMM_BACKOFF_MAX_MQTT_MS);
            }
            else if (s_consecutive_mqtt_subscribe_fail < THRESHOLD_5X)
            {
                enter_backoff(COMM_STATE_GPRS_ATTACH,
                              COMM_ERR_MQTT_SUBSCRIBE_FAILED,
                              COMM_BACKOFF_MAX_NETWORK_MS);
            }
            else if (s_consecutive_mqtt_subscribe_fail < MQTT_SUBSCRIBE_SOFT_RESET_THRESHOLD)
            {
                enter_backoff(COMM_STATE_MODEM_SYNC,
                              COMM_ERR_MQTT_SUBSCRIBE_FAILED,
                              COMM_BACKOFF_MAX_NETWORK_MS);
            }
            else
            {
                request_soft_reset(COMM_ERR_MQTT_SUBSCRIBE_FAILED,
                                   "SOFT_RESET_TRIGGERED_MQTT_SUB",
                                   "MQTT subscribe failed repeatedly, soft resetting modem",
                                   s_consecutive_mqtt_subscribe_fail,
                                   s_health.mqtt_subscribe_fail_count);
            }
            break;
        }

        s_last_error = COMM_ERR_NONE;
        s_health.online_enter_count++;
        s_consecutive_mqtt_subscribe_fail = 0;
        recovery_clear_if_not_publish();

        reset_backoff();
        set_state(COMM_STATE_ONLINE);
        break;

    case COMM_STATE_ONLINE:
        /*
         * Keep ONLINE state non-invasive.
         * Do not poll AT commands here; AT responses can consume MQTT URCs.
         */
        break;

    case COMM_STATE_MODEM_SOFT_RESET:
#if ENABLE_MODEM_SOFT_RESET
        print("[COMM] MODEM_SOFT_RESET start last_error=%s fail_count=%u\r\n",
              comm_manager_error_name(s_last_error),
              s_consecutive_soft_reset_fail);

        app_log_critical(APP_LOG_FATAL,
                         "MODEM",
                         "SOFT_RESET_STARTED",
                         "modem soft reset started",
                         s_last_error,
                         s_consecutive_soft_reset_fail,
                         "AT_CFUN_RESET");

        if (Modem_SoftReset())
        {
            app_log_push(APP_LOG_INFO,
                         "MODEM",
                         "SOFT_RESET_DONE",
                         "modem soft reset completed",
                         0,
                         0);

            /*
             * After modem reset, IMEI/topics/session need refreshing.
             */
            imei[0] = '\0';

            s_last_error = COMM_ERR_NONE;
            s_consecutive_soft_reset_fail = 0;

            clear_all_failure_counters();
            recovery_clear();
            reset_backoff();

            enter_backoff(COMM_STATE_MODEM_SYNC,
                          COMM_ERR_NONE,
                          COMM_BACKOFF_INITIAL_MS);
        }
        else
        {
            s_consecutive_soft_reset_fail++;

            app_log_critical(APP_LOG_FATAL,
                             "MODEM",
                             "SOFT_RESET_FAILED",
                             "modem did not respond after soft reset",
                             s_last_error,
                             s_consecutive_soft_reset_fail,
                             "MODEM_SYNC_RETRY");

            /*
             * If modem reset itself cannot recover repeatedly, restart the
             * controller. This is last resort; normal MQTT/SIM/APN outages
             * should not directly reboot the MCU.
             */
            if (s_consecutive_soft_reset_fail >= MODEM_RESET_MCU_RESET_THRESHOLD)
            {
                app_log_critical(APP_LOG_FATAL,
                                 "MCU",
                                 "RESET_AFTER_MODEM_RESET_FAILS",
                                 "MCU reset after repeated modem reset recovery failures",
                                 s_last_error,
                                 s_consecutive_soft_reset_fail,
                                 "NVIC_SYSTEM_RESET");

                print("[COMM] MCU reset after repeated modem reset failures\r\n");
                osDelay(1000);
                app_gateway_set_reset_intent(APP_RESET_INTENT_MODEM_RECOVERY);
                NVIC_SystemReset();
            }

            enter_backoff(COMM_STATE_MODEM_SYNC,
                          s_last_error,
                          COMM_BACKOFF_MAX_NETWORK_MS);
        }
#else
        print("[COMM] soft reset disabled, going to MODEM_SYNC\r\n");

        enter_backoff(COMM_STATE_MODEM_SYNC,
                      s_last_error,
                      COMM_BACKOFF_MAX_NETWORK_MS);
#endif
        break;

    case COMM_STATE_BACKOFF:
        if (time_reached(s_next_action_tick))
        {
            set_state(s_retry_state);
        }
        break;

    case COMM_STATE_DEGRADED:
    default:
        app_log_push(APP_LOG_WARN,
                     "COMM",
                     "DEGRADED_STATE",
                     "unexpected degraded/default state",
                     s_last_error,
                     0);

        enter_backoff(COMM_STATE_MODEM_SYNC,
                      s_last_error,
                      COMM_BACKOFF_MAX_NETWORK_MS);
        break;
    }
}

uint8_t comm_manager_is_online(void)
{
    return (s_state == COMM_STATE_ONLINE) ? 1U : 0U;
}

void comm_manager_report_publish_result(uint8_t ok)
{
    if (ok)
    {
        s_consecutive_publish_fail = 0;
        s_consecutive_mqtt_disconnect_fail = 0;
        s_health.last_successful_publish_tick = HAL_GetTick();
        s_last_error = COMM_ERR_NONE;
        recovery_clear();
        reset_backoff();
        return;
    }

    s_health.mqtt_publish_fail_count++;
    s_consecutive_publish_fail++;
    s_last_error = COMM_ERR_MQTT_PUBLISH_FAILED;
    recovery_mark_failure(COMM_ERR_MQTT_PUBLISH_FAILED);

    char publish_msg[96];
    snprintf(publish_msg,
             sizeof(publish_msg),
             "publish failed stage=%s",
             MQTT_LastPublishStage());

    print("[COMM] MQTT publish failed consecutive=%u total=%lu stage=%s\r\n",
          s_consecutive_publish_fail,
          (unsigned long)s_health.mqtt_publish_fail_count,
          MQTT_LastPublishStage());

    if (s_consecutive_publish_fail < THRESHOLD_3X)
    {
        app_log_push(APP_LOG_WARN,
                     "MQTT",
                     "PUBLISH_FAILED_SOFT",
                     publish_msg,
                     s_consecutive_publish_fail,
                     s_health.mqtt_publish_fail_count);
        return;
    }

    if (s_consecutive_publish_fail < THRESHOLD_5X)
    {
        if (s_consecutive_publish_fail == THRESHOLD_3X)
        {
            log_threshold_failure(APP_LOG_ERROR,
                                  "MQTT",
                                  "PUBLISH_FAILED_3X",
                                  publish_msg,
                                  s_consecutive_publish_fail,
                                  s_health.mqtt_publish_fail_count,
                                  "MQTT_RECONNECT");
        }

        enter_backoff(COMM_STATE_MQTT_CONNECT,
                      COMM_ERR_MQTT_PUBLISH_FAILED,
                      COMM_BACKOFF_MAX_MQTT_MS);
        return;
    }

    if (s_consecutive_publish_fail < MQTT_PUBLISH_SOFT_RESET_THRESHOLD)
    {
        if (s_consecutive_publish_fail == THRESHOLD_5X)
        {
            log_threshold_failure(APP_LOG_FATAL,
                                  "MQTT",
                                  "PUBLISH_FAILED_5X",
                                  publish_msg,
                                  s_consecutive_publish_fail,
                                  s_health.mqtt_publish_fail_count,
                                  "GPRS_ATTACH");
        }

        /*
         * Publish failure is MQTT/PDP side. Rebuild PDP before modem reset.
         */
        enter_backoff(COMM_STATE_GPRS_ATTACH,
                      COMM_ERR_MQTT_PUBLISH_FAILED,
                      COMM_BACKOFF_MAX_NETWORK_MS);
        return;
    }

    request_soft_reset(COMM_ERR_MQTT_PUBLISH_FAILED,
                       "SOFT_RESET_TRIGGERED_PUBLISH",
                       publish_msg,
                       s_consecutive_publish_fail,
                       s_health.mqtt_publish_fail_count);
}

void comm_manager_report_mqtt_disconnected(void)
{
    s_health.mqtt_disconnect_count++;
    s_consecutive_mqtt_disconnect_fail++;

    if (s_consecutive_mqtt_disconnect_fail >= MQTT_DISCONNECT_SOFT_RESET_THRESHOLD)
    {
        request_soft_reset(COMM_ERR_MQTT_DISCONNECTED,
                           "SOFT_RESET_TRIGGERED_DISCONNECT",
                           "MQTT/PDP disconnected repeatedly, soft resetting modem",
                           s_consecutive_mqtt_disconnect_fail,
                           s_health.mqtt_disconnect_count);
        return;
    }

    app_log_critical(APP_LOG_ERROR,
                     "MQTT",
                     "DISCONNECTED",
                     "MQTT/PDP disconnected, falling back to GPRS attach",
                     s_consecutive_mqtt_disconnect_fail,
                     s_health.mqtt_disconnect_count,
                     "GPRS_ATTACH");

    /*
     * Disconnect is not direct modem reset.
     * Start from GPRS attach because PDP/MQTT session may be gone.
     */
    enter_backoff(COMM_STATE_GPRS_ATTACH,
                  COMM_ERR_MQTT_DISCONNECTED,
                  COMM_BACKOFF_MAX_NETWORK_MS);
}

comm_state_t comm_manager_get_state(void)
{
    return s_state;
}

comm_error_t comm_manager_get_last_error(void)
{
    return s_last_error;
}

const comm_health_t *comm_manager_get_health(void)
{
    return &s_health;
}

const char *comm_manager_state_name(comm_state_t state)
{
    switch (state)
    {
    case COMM_STATE_BOOT:             return "BOOT";
    case COMM_STATE_MODEM_SYNC:       return "MODEM_SYNC";
    case COMM_STATE_SIM_CHECK:        return "SIM_CHECK";
    case COMM_STATE_NETWORK_WAIT:     return "NETWORK_WAIT";
    case COMM_STATE_GPRS_ATTACH:      return "GPRS_ATTACH";
    case COMM_STATE_MQTT_CONNECT:     return "MQTT_CONNECT";
    case COMM_STATE_MQTT_SUBSCRIBE:   return "MQTT_SUBSCRIBE";
    case COMM_STATE_ONLINE:           return "ONLINE";
    case COMM_STATE_MODEM_SOFT_RESET: return "MODEM_SOFT_RESET";
    case COMM_STATE_BACKOFF:          return "BACKOFF";
    case COMM_STATE_DEGRADED:         return "DEGRADED";
    default:                          return "UNKNOWN";
    }
}

const char *comm_manager_error_name(comm_error_t error)
{
    switch (error)
    {
    case COMM_ERR_NONE:                   return "NONE";
    case COMM_ERR_AT_TIMEOUT:             return "AT_TIMEOUT";
    case COMM_ERR_SIM_NOT_READY:          return "SIM_NOT_READY";
    case COMM_ERR_NETWORK_NOT_REGISTERED: return "NETWORK_NOT_REGISTERED";
    case COMM_ERR_GPRS_FAILED:            return "GPRS_FAILED";
    case COMM_ERR_MQTT_CONNECT_FAILED:    return "MQTT_CONNECT_FAILED";
    case COMM_ERR_MQTT_SUBSCRIBE_FAILED:  return "MQTT_SUBSCRIBE_FAILED";
    case COMM_ERR_MQTT_PUBLISH_FAILED:    return "MQTT_PUBLISH_FAILED";
    case COMM_ERR_MQTT_DISCONNECTED:      return "MQTT_DISCONNECTED";
    default:                              return "UNKNOWN";
    }
}
