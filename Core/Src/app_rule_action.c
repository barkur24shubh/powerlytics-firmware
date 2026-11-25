#include "app_rule_action.h"

#include <string.h>

void app_act_init(app_act_t *a)
{
  if (a) memset(a, 0, sizeof(*a));
}

void app_act_raise(app_act_t *a, uint8_t relay, uint16_t owner, uint8_t state)
{
  if (!a || relay >= APP_ACT_MAX_RELAYS) return;
  app_act_relay_t *r = &a->relay[relay];
  r->owned = 1U;
  r->owner = owner;
  r->held_state = state ? 1U : 0U;
  r->pending = 1U;
  r->pending_state = r->held_state;
}

uint8_t app_act_clear(app_act_t *a, uint8_t relay, uint16_t owner, int8_t restore_state)
{
  if (!a || relay >= APP_ACT_MAX_RELAYS) return 0U;
  app_act_relay_t *r = &a->relay[relay];
  if (!r->owned || r->owner != owner) return 0U;
  r->owned = 0U;
  r->owner = 0U;
  if (restore_state >= 0) {
    r->pending = 1U;
    r->pending_state = restore_state ? 1U : 0U;
  } else {
    r->pending = 0U;
  }
  return 1U;
}

void app_act_release(app_act_t *a, uint8_t relay, uint16_t owner)
{
  if (!a || relay >= APP_ACT_MAX_RELAYS) return;
  app_act_relay_t *r = &a->relay[relay];
  if (!r->owned || r->owner != owner) return;
  r->owned = 0U;
  r->owner = 0U;
  r->pending = 0U;
}

uint8_t app_act_command_allowed(const app_act_t *a, uint8_t relay, uint8_t requested)
{
  if (!a || relay >= APP_ACT_MAX_RELAYS) return 1U;
  const app_act_relay_t *r = &a->relay[relay];
  if (!r->owned) return 1U;
  return ((requested ? 1U : 0U) == r->held_state) ? 1U : 0U;
}

uint8_t app_act_next(const app_act_t *a, uint8_t relay, uint8_t *state)
{
  if (!a || relay >= APP_ACT_MAX_RELAYS || !a->relay[relay].pending) return 0U;
  if (state) *state = a->relay[relay].pending_state;
  return 1U;
}

void app_act_done(app_act_t *a, uint8_t relay)
{
  if (!a || relay >= APP_ACT_MAX_RELAYS) return;
  a->relay[relay].pending = 0U;
}
