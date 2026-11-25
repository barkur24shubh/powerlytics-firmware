/* Host test for Core/Src/app_rule_action.c.
 *   cc -Wall -ICore/Inc -o /tmp/rule_action_host_test scripts/rule_action_host_test.c Core/Src/app_rule_action.c
 */
#include <stdio.h>
#include "app_rule_action.h"

static int fails = 0, checks = 0;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { fails++; printf("  FAIL: %s\n", msg); } } while (0)

static void scenario(const char *name) { printf("%s\n", name); }

int main(void)
{
  app_act_t a;
  uint8_t st;

  scenario("A1 a raise holds the relay and asks for the state to be written");
  app_act_init(&a);
  app_act_raise(&a, 0, 0x1111, 0);
  CHECK(app_act_next(&a, 0, &st) && st == 0, "pending write of 0");
  CHECK(!app_act_next(&a, 1, &st), "the other relay is untouched");

  scenario("A2 a refused write is retried until it is accepted");
  CHECK(app_act_next(&a, 0, &st), "still pending after a refusal (driver chatter guard)");
  CHECK(app_act_next(&a, 0, &st), "still pending");
  app_act_done(&a, 0);
  CHECK(!app_act_next(&a, 0, &st), "done once the driver accepted");

  scenario("A3 an outside command to the other state is refused while owned, to the same state accepted");
  CHECK(!app_act_command_allowed(&a, 0, 1), "TURN_ON refused while the rule holds it OFF");
  CHECK(app_act_command_allowed(&a, 0, 0), "TURN_OFF accepted");
  CHECK(app_act_command_allowed(&a, 1, 1), "the other relay is free");

  scenario("A4 the owner's clear restores the state and frees the relay");
  CHECK(app_act_clear(&a, 0, 0x1111, 1) == 1, "released by the owner");
  CHECK(app_act_next(&a, 0, &st) && st == 1, "restore write of 1");
  app_act_done(&a, 0);
  CHECK(app_act_command_allowed(&a, 0, 1) && app_act_command_allowed(&a, 0, 0), "free again");

  scenario("A5 a clear with no restore state leaves the relay alone");
  app_act_init(&a);
  app_act_raise(&a, 0, 0x2222, 0);
  app_act_done(&a, 0);
  CHECK(app_act_clear(&a, 0, 0x2222, -1) == 1, "released");
  CHECK(!app_act_next(&a, 0, &st), "nothing to write");

  scenario("A6 another rule's clear does not free a relay it does not own");
  app_act_init(&a);
  app_act_raise(&a, 0, 0xAAAA, 0);
  CHECK(app_act_clear(&a, 0, 0xBBBB, 1) == 0, "not the owner");
  CHECK(!app_act_command_allowed(&a, 0, 1), "still held");

  scenario("A7 the last raise wins; the first rule's clear then does nothing");
  app_act_init(&a);
  app_act_raise(&a, 0, 0xAAAA, 0);
  app_act_done(&a, 0);
  app_act_raise(&a, 0, 0xBBBB, 1);
  CHECK(app_act_next(&a, 0, &st) && st == 1, "second rule's state written");
  app_act_done(&a, 0);
  CHECK(app_act_clear(&a, 0, 0xAAAA, 1) == 0, "first rule no longer owns it");
  CHECK(app_act_command_allowed(&a, 0, 1) && !app_act_command_allowed(&a, 0, 0), "held at 1 by the second rule");

  scenario("A8 a removed rule releases the relay without touching its state");
  app_act_init(&a);
  app_act_raise(&a, 1, 0xCCCC, 0);
  app_act_done(&a, 1);
  app_act_release(&a, 1, 0xCCCC);
  CHECK(!app_act_next(&a, 1, &st), "no write");
  CHECK(app_act_command_allowed(&a, 1, 1), "free");

  scenario("A9 releasing a rule that never owned it changes nothing");
  app_act_init(&a);
  app_act_raise(&a, 0, 0xDDDD, 0);
  app_act_release(&a, 0, 0xEEEE);
  CHECK(!app_act_command_allowed(&a, 0, 1), "still owned by 0xDDDD");

  scenario("A10 out of range relays are ignored, never crash");
  app_act_init(&a);
  app_act_raise(&a, 200, 1, 1);
  CHECK(app_act_command_allowed(&a, 200, 1) == 1, "unknown relay is not blocked");
  CHECK(app_act_clear(&a, 200, 1, 0) == 0, "no clear");
  CHECK(!app_act_next(&a, 200, &st), "no write");

  scenario("A11 a clear while a raise write is still pending replaces it with the restore");
  app_act_init(&a);
  app_act_raise(&a, 0, 0x1234, 0);
  CHECK(app_act_clear(&a, 0, 0x1234, 1) == 1, "cleared before the write happened");
  CHECK(app_act_next(&a, 0, &st) && st == 1, "the relay goes straight to the restore state");

  printf("\n%d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
