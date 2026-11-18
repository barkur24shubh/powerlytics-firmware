#ifndef APP_RULE_EVAL_H
#define APP_RULE_EVAL_H

/* Analog alarm rule state machine (threshold_high, threshold_low,
 * out_of_range, in_range). Pure logic: no HAL, no queue, no clock reads,
 * so it is unit-tested on the host (scripts/rule_eval_host_test.c).
 *
 * Contract with the caller:
 *   ev = app_rule_step(cfg, rt, value, now, &out);
 *   if (ev != RULE_EV_NONE && <event queued>) app_rule_commit(rt, ev);
 * A raise or recovery only changes the rule's state once the event is
 * queued. If the queue is full the rule stays pending and the next sample
 * raises it again, so an alarm is never active on the device without the
 * cloud being told (before 2026-11-18 a full queue lost the event and the
 * rule went active anyway, so the matching recovery closed nothing). */

#include <stdint.h>
#include "modbus_config.h"

typedef enum {
  RULE_STATE_NORMAL = 0,
  RULE_STATE_PENDING,          /* condition true, waiting for durationMs */
  RULE_STATE_ACTIVE,           /* alarm raised */
  RULE_STATE_RECOVERY_PENDING, /* back inside the hysteresis band, waiting for recoveryDurationMs */
} app_rule_state_t;

typedef enum {
  RULE_EV_NONE = 0,
  RULE_EV_RAISE,
  RULE_EV_RECOVER,
} app_rule_ev_t;

typedef struct {
  uint8_t  state;
  uint32_t entered_ms;
  float    previous_value; /* value of the previous valid sample */
  uint8_t  has_previous;
  uint8_t  raises_in_window;  /* raises since chatter_since */
  uint8_t  chattering;        /* recovery held for RULE_CHATTER_HOLD_MS */
  uint32_t chatter_since;
} app_rule_runtime_t;

/* Chattering alarm guard (ISA-18.2 style): a rule raised
 * RULE_CHATTER_RAISES times within RULE_CHATTER_WINDOW_MS is "chattering";
 * its recovery then needs the value back inside the hysteresis band for at
 * least RULE_CHATTER_HOLD_MS (or the rule's own recoveryDurationMs if
 * longer). A noisy or floating input gives a few events, then one held
 * alarm, instead of a raise/recover pair every second that floods the
 * uplink and starves telemetry (bench 2026-11-18: ~1 event/s, telemetry
 * every 15 s instead of 5 s). */
#define RULE_CHATTER_RAISES    3U
#define RULE_CHATTER_WINDOW_MS 60000UL
#define RULE_CHATTER_HOLD_MS   60000UL

typedef struct {
  const char *event_type; /* AI_THRESHOLD_HIGH ... or AI_RECOVERY */
  float threshold;        /* the bound that was crossed */
  float previous;         /* previous sample value */
} app_rule_out_t;

/* 1 when the rule can behave sensibly, else 0 and *reason (if given) says
 * why: invalid_rule_number, invalid_rule_band, invalid_rule_hysteresis. */
uint8_t app_rule_cfg_valid(const powerlytics_rule_cfg_t *cfg, const char **reason);

/* 1 for the analog rule types this module evaluates. */
uint8_t app_rule_is_analog(uint8_t type);

/* 1 when two rule definitions mean the same alarm (state can carry over). */
uint8_t app_rule_same_meaning(const powerlytics_rule_cfg_t *a, const powerlytics_rule_cfg_t *b);

/* One sample. A NaN/inf value (bad sensor or failed read) changes nothing:
 * it neither raises nor clears an alarm. */
app_rule_ev_t app_rule_step(const powerlytics_rule_cfg_t *cfg,
                            app_rule_runtime_t *rt,
                            float value,
                            uint32_t now_ms,
                            app_rule_out_t *out);

/* Apply the transition app_rule_step reported, once its event is queued.
 * Returns 1 when this raise made the rule chattering. */
uint8_t app_rule_commit(app_rule_runtime_t *rt, app_rule_ev_t ev, uint32_t now_ms);

/* Put a rule straight into the active state (restored after a reset). */
void app_rule_restore_active(app_rule_runtime_t *rt, uint32_t now_ms);

static inline uint8_t app_rule_is_active(const app_rule_runtime_t *rt)
{
  return (rt && (rt->state == RULE_STATE_ACTIVE || rt->state == RULE_STATE_RECOVERY_PENDING)) ? 1U : 0U;
}

#endif
