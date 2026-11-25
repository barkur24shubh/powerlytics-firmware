#include "app_rule_eval.h"

#include <math.h>
#include <string.h>

static uint8_t finite_f(float v)
{
  return (isfinite(v) != 0) ? 1U : 0U;
}

uint8_t app_rule_is_analog(uint8_t type)
{
  return (type == POWERLYTICS_RULE_THRESHOLD_HIGH ||
          type == POWERLYTICS_RULE_THRESHOLD_LOW ||
          type == POWERLYTICS_RULE_OUT_OF_RANGE ||
          type == POWERLYTICS_RULE_IN_RANGE) ? 1U : 0U;
}

uint8_t app_rule_cfg_valid(const powerlytics_rule_cfg_t *cfg, const char **reason)
{
  const char *why = NULL;

  if (!cfg) {
    why = "invalid_rule";
  } else if (POWERLYTICS_ACT_RELAY(cfg) == POWERLYTICS_ACT_INVALID) {
    why = "invalid_rule_action"; /* the action was named badly (not RO_1..RO_8, or no actRaise) */
  } else if (POWERLYTICS_ACT_RELAY(cfg) != 0U && !app_rule_is_analog(cfg->type)) {
    why = "invalid_rule_action"; /* an action needs an alarm that clears: edge rules have none */
  } else if (!app_rule_is_analog(cfg->type)) {
    why = NULL; /* edge rules carry no numbers */
  } else if (!finite_f(cfg->high) || !finite_f(cfg->low) || !finite_f(cfg->hysteresis)) {
    why = "invalid_rule_number";
  } else if (cfg->hysteresis < 0.0f) {
    why = "invalid_rule_hysteresis";
  } else if (cfg->type == POWERLYTICS_RULE_OUT_OF_RANGE || cfg->type == POWERLYTICS_RULE_IN_RANGE) {
    if (!(cfg->low < cfg->high)) {
      why = "invalid_rule_band";
    } else if (cfg->type == POWERLYTICS_RULE_OUT_OF_RANGE &&
               (cfg->hysteresis * 2.0f) > (cfg->high - cfg->low)) {
      /* The recovery band [low+h, high-h] would be empty: the alarm could
       * never clear. Exactly half the band is allowed (the platform allows
       * it too, "at most (high-low)/2"). */
      why = "invalid_rule_hysteresis";
    }
  }

  if (reason) *reason = why;
  return why ? 0U : 1U;
}

uint8_t app_rule_same_meaning(const powerlytics_rule_cfg_t *a, const powerlytics_rule_cfg_t *b)
{
  if (!a || !b) return 0U;
  if (a->type != b->type) return 0U;
  if (a->enabled != b->enabled) return 0U;
  if (!app_rule_is_analog(a->type)) return 1U;
  /* A different action is a different rule: the old relay hold must end. */
  if (a->act != b->act) return 0U;
  /* Durations and severity do not change whether the alarm holds. */
  return (a->high == b->high && a->low == b->low && a->hysteresis == b->hysteresis) ? 1U : 0U;
}

static void conditions(const powerlytics_rule_cfg_t *cfg, float value,
                       uint8_t *crossed, uint8_t *recovered,
                       float *threshold, const char **event_type)
{
  *crossed = 0U;
  *recovered = 0U;
  *threshold = 0.0f;
  *event_type = NULL;

  switch (cfg->type) {
    case POWERLYTICS_RULE_THRESHOLD_HIGH:
      *crossed = (value > cfg->high) ? 1U : 0U;
      *recovered = (value <= (cfg->high - cfg->hysteresis)) ? 1U : 0U;
      *threshold = cfg->high;
      *event_type = "AI_THRESHOLD_HIGH";
      break;
    case POWERLYTICS_RULE_THRESHOLD_LOW:
      *crossed = (value < cfg->low) ? 1U : 0U;
      *recovered = (value >= (cfg->low + cfg->hysteresis)) ? 1U : 0U;
      *threshold = cfg->low;
      *event_type = "AI_THRESHOLD_LOW";
      break;
    case POWERLYTICS_RULE_OUT_OF_RANGE:
      *crossed = (value < cfg->low || value > cfg->high) ? 1U : 0U;
      *recovered = (value >= (cfg->low + cfg->hysteresis) &&
                    value <= (cfg->high - cfg->hysteresis)) ? 1U : 0U;
      *threshold = (value < cfg->low) ? cfg->low : cfg->high;
      *event_type = "AI_OUT_OF_RANGE";
      break;
    case POWERLYTICS_RULE_IN_RANGE:
      *crossed = (value >= cfg->low && value <= cfg->high) ? 1U : 0U;
      *recovered = (value < (cfg->low - cfg->hysteresis) ||
                    value > (cfg->high + cfg->hysteresis)) ? 1U : 0U;
      *threshold = (value < cfg->low) ? cfg->low : cfg->high;
      *event_type = "AI_IN_RANGE";
      break;
    default:
      break;
  }
}

app_rule_ev_t app_rule_step(const powerlytics_rule_cfg_t *cfg,
                            app_rule_runtime_t *rt,
                            float value,
                            uint32_t now_ms,
                            app_rule_out_t *out)
{
  if (!cfg || !rt || !cfg->enabled || !app_rule_is_analog(cfg->type)) return RULE_EV_NONE;
  if (!finite_f(value)) return RULE_EV_NONE;

  uint8_t crossed, recovered;
  float threshold;
  const char *event_type;
  conditions(cfg, value, &crossed, &recovered, &threshold, &event_type);

  float previous = rt->has_previous ? rt->previous_value : value;
  rt->previous_value = value;
  rt->has_previous = 1U;

  if (out) {
    out->event_type = event_type;
    out->threshold = threshold;
    out->previous = previous;
  }

  /* A pending state is re-checked on the same sample, so with no duration
   * the event fires on the sample that crossed. Recovery must stay inside
   * the hysteresis band for the whole recovery time: a value back between
   * the band and the threshold cancels it. */
  if (rt->state == RULE_STATE_NORMAL && crossed) {
    rt->state = RULE_STATE_PENDING;
    rt->entered_ms = now_ms;
  }
  if (rt->state == RULE_STATE_PENDING) {
    if (!crossed) {
      rt->state = RULE_STATE_NORMAL;
      return RULE_EV_NONE;
    }
    return ((uint32_t)(now_ms - rt->entered_ms) >= cfg->duration_ms) ? RULE_EV_RAISE : RULE_EV_NONE;
  }

  if (rt->state == RULE_STATE_ACTIVE && recovered) {
    rt->state = RULE_STATE_RECOVERY_PENDING;
    rt->entered_ms = now_ms;
  }
  if (rt->state == RULE_STATE_RECOVERY_PENDING) {
    if (!recovered) {
      rt->state = RULE_STATE_ACTIVE;
      return RULE_EV_NONE;
    }
    uint32_t hold = cfg->recovery_duration_ms;
    if (rt->chattering && hold < RULE_CHATTER_HOLD_MS) hold = RULE_CHATTER_HOLD_MS;
    if ((uint32_t)(now_ms - rt->entered_ms) >= hold) {
      if (out) out->event_type = "AI_RECOVERY";
      return RULE_EV_RECOVER;
    }
  }
  return RULE_EV_NONE;
}

uint8_t app_rule_commit(app_rule_runtime_t *rt, app_rule_ev_t ev, uint32_t now_ms)
{
  if (!rt) return 0U;
  if (ev == RULE_EV_RAISE) {
    rt->state = RULE_STATE_ACTIVE;
    if (rt->raises_in_window == 0U || (uint32_t)(now_ms - rt->chatter_since) > RULE_CHATTER_WINDOW_MS) {
      rt->chatter_since = now_ms;
      rt->raises_in_window = 1U;
    } else if (rt->raises_in_window < 255U) {
      rt->raises_in_window++;
    }
    if (!rt->chattering && rt->raises_in_window >= RULE_CHATTER_RAISES) {
      rt->chattering = 1U;
      return 1U;
    }
  } else if (ev == RULE_EV_RECOVER) {
    rt->state = RULE_STATE_NORMAL;
    if (rt->chattering) {
      /* A held recovery is a real one: start counting afresh. */
      rt->chattering = 0U;
      rt->raises_in_window = 0U;
    }
  }
  return 0U;
}

void app_rule_restore_active(app_rule_runtime_t *rt, uint32_t now_ms)
{
  if (!rt) return;
  memset(rt, 0, sizeof(*rt));
  rt->state = RULE_STATE_ACTIVE;
  rt->entered_ms = now_ms;
  /* Whether it was chattering is not kept across a reset; hold the recovery
   * as if it were, so a noisy input does not burst events right after boot. */
  rt->chattering = 1U;
  rt->chatter_since = now_ms;
}
