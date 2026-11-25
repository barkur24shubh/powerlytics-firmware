#ifndef APP_RULE_ACTION_H
#define APP_RULE_ACTION_H

/* A rule that drives a relay (FH-018 local action, firmware 1.0.19).
 *
 * Pure logic: no HAL, no relay driver, no clock, so it is unit-tested on the
 * host (scripts/rule_action_host_test.c). The engine feeds it the rule's raise
 * and clear, the relay driver is asked to do what it says.
 *
 * Safety rules, in one place:
 *  - A raise takes the relay: it is held at the rule's state ("owned") for as
 *    long as the alarm is active. The wanted state is retried every tick until
 *    the relay driver accepts it (the driver refuses a change inside its
 *    500 ms chatter guard), so a protective action is never lost.
 *  - A command from outside (the platform) that would put the relay in the
 *    OTHER state while it is owned is refused with "rule_active". A command to
 *    the held state is accepted. Hardware or firmware safety is above this and
 *    is not touched.
 *  - A clear gives the relay back: the rule's restore state (if it has one) is
 *    applied, then the relay is free. Only the owner's clear does this, so two
 *    rules on one relay do not release each other: the last raise wins.
 *  - A rule that disappears while owning a relay (deleted, disabled, changed)
 *    releases it WITHOUT touching the state: nobody knows what is safe. */

#include <stdint.h>

#define APP_ACT_MAX_RELAYS 8U

typedef struct {
  uint8_t  owned;         /* 1 while a raise holds the relay */
  uint8_t  held_state;    /* the state the owner holds it at */
  uint8_t  pending;       /* a state still to be written */
  uint8_t  pending_state;
  uint16_t owner;         /* identity of the rule that owns it */
} app_act_relay_t;

typedef struct {
  app_act_relay_t relay[APP_ACT_MAX_RELAYS];
} app_act_t;

void app_act_init(app_act_t *a);

/* relay is 0-based. */
void app_act_raise(app_act_t *a, uint8_t relay, uint16_t owner, uint8_t state);

/* restore_state < 0: leave the relay where it is. Returns 1 when this clear
 * released the relay (the caller was the owner). */
uint8_t app_act_clear(app_act_t *a, uint8_t relay, uint16_t owner, int8_t restore_state);

/* The rule is gone: free the relay, change nothing. */
void app_act_release(app_act_t *a, uint8_t relay, uint16_t owner);

/* 1 when an outside command may set this relay to `requested`. */
uint8_t app_act_command_allowed(const app_act_t *a, uint8_t relay, uint8_t requested);

/* 1 and *state when the relay still needs to be written; call
 * app_act_done once the driver has accepted it. */
uint8_t app_act_next(const app_act_t *a, uint8_t relay, uint8_t *state);
void    app_act_done(app_act_t *a, uint8_t relay);

#endif
