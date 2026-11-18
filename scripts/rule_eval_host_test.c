/* Host tests for the analog alarm rule state machine (Core/Src/app_rule_eval.c)
 * using plant-style scenarios.
 *
 *   cc -std=c11 -Wall -Wextra -ICore/Inc scripts/rule_eval_host_test.c Core/Src/app_rule_eval.c -lm -o /tmp/rule_test && /tmp/rule_test
 */
#include "app_rule_eval.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_fail;
static int g_pass;

#define CHECK(cond, ...) do { \
  if (cond) { g_pass++; } else { g_fail++; printf("  FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

typedef struct {
  int raises;
  int recovers;
  uint32_t first_raise_ms;
  uint32_t last_recover_ms;
  float last_threshold;
  float raise_value;
  const char *last_type;
} tally_t;

static powerlytics_rule_cfg_t rule(uint8_t type, float low, float high, float hyst,
                                   uint32_t dur, uint32_t rec)
{
  powerlytics_rule_cfg_t r;
  memset(&r, 0, sizeof(r));
  r.type = type;
  r.enabled = 1U;
  r.low = low;
  r.high = high;
  r.hysteresis = hyst;
  r.duration_ms = dur;
  r.recovery_duration_ms = rec;
  return r;
}

/* One sample; queue_ok says whether the event queue accepted the event. */
static app_rule_ev_t feed(const powerlytics_rule_cfg_t *cfg, app_rule_runtime_t *rt,
                          float v, uint32_t now, int queue_ok, tally_t *t)
{
  app_rule_out_t out;
  app_rule_ev_t ev = app_rule_step(cfg, rt, v, now, &out);
  if (ev != RULE_EV_NONE && queue_ok) {
    app_rule_commit(rt, ev, now);
    if (ev == RULE_EV_RAISE) {
      if (t->raises == 0) t->first_raise_ms = now;
      t->raises++;
      t->raise_value = v;
    } else {
      t->recovers++;
      t->last_recover_ms = now;
    }
    t->last_threshold = out.threshold;
    t->last_type = out.event_type;
  }
  return ev;
}

/* Tank level: H warning 80, HH critical 95, hysteresis 2, on-delay 5 s,
 * off-delay 10 s, sampled every 1 s. Fill to 97, hold, drain to 75. */
static void s1_tank_h_hh(void)
{
  printf("S1 tank level H/HH with on/off delay\n");
  powerlytics_rule_cfg_t h = rule(POWERLYTICS_RULE_THRESHOLD_HIGH, 0, 80, 2, 5000, 10000);
  powerlytics_rule_cfg_t hh = rule(POWERLYTICS_RULE_THRESHOLD_HIGH, 0, 95, 2, 5000, 10000);
  app_rule_runtime_t rh = {0}, rhh = {0};
  tally_t th = {0}, thh = {0};
  uint32_t t = 0;
  float level = 70.0f;
  for (; level < 97.0f; level += 0.5f, t += 1000) { feed(&h, &rh, level, t, 1, &th); feed(&hh, &rhh, level, t, 1, &thh); }
  for (int i = 0; i < 30; i++, t += 1000) { feed(&h, &rh, 97, t, 1, &th); feed(&hh, &rhh, 97, t, 1, &thh); }
  for (; level > 75.0f; level -= 0.5f, t += 1000) { feed(&h, &rh, level, t, 1, &th); feed(&hh, &rhh, level, t, 1, &thh); }
  for (int i = 0; i < 20; i++, t += 1000) { feed(&h, &rh, 75, t, 1, &th); feed(&hh, &rhh, 75, t, 1, &thh); }
  CHECK(th.raises == 1 && th.recovers == 1, "H raises=%d recovers=%d", th.raises, th.recovers);
  CHECK(thh.raises == 1 && thh.recovers == 1, "HH raises=%d recovers=%d", thh.raises, thh.recovers);
  /* 80.5 at t=21 s; on-delay 5 s -> raise at 26 s */
  CHECK(th.first_raise_ms == 26000, "H raised at %u ms (want 26000)", th.first_raise_ms);
  CHECK(th.last_threshold == 80.0f, "H threshold %.2f", th.last_threshold);
  CHECK(app_rule_is_active(&rh) == 0 && app_rule_is_active(&rhh) == 0, "both clear at end");
}

/* Noisy transmitter: 80 +/- 0.8 around an 80 limit for 10 minutes.
 * Hysteresis 2 must give exactly one alarm and no chatter. */
static void s2_noise_chatter(void)
{
  printf("S2 noisy signal around the limit\n");
  powerlytics_rule_cfg_t h = rule(POWERLYTICS_RULE_THRESHOLD_HIGH, 0, 80, 2, 0, 0);
  app_rule_runtime_t rt = {0};
  tally_t tl = {0};
  for (uint32_t i = 0; i < 600; i++) {
    float noise = (float)((i * 7919U) % 17U) / 10.0f - 0.8f; /* -0.8 .. +0.8 */
    feed(&h, &rt, 80.0f + noise, i * 1000U, 1, &tl);
  }
  CHECK(tl.raises == 1 && tl.recovers == 0, "raises=%d recovers=%d (want 1/0)", tl.raises, tl.recovers);

  printf("S2b same noise with hysteresis 0 (shows why a deadband is needed)\n");
  powerlytics_rule_cfg_t h0 = rule(POWERLYTICS_RULE_THRESHOLD_HIGH, 0, 80, 0, 0, 0);
  app_rule_runtime_t r0 = {0};
  tally_t t0 = {0};
  for (uint32_t i = 0; i < 600; i++) {
    float noise = (float)((i * 7919U) % 17U) / 10.0f - 0.8f;
    feed(&h0, &r0, 80.0f + noise, i * 1000U, 1, &t0);
  }
  printf("  info: hysteresis 0 gave %d raises in 10 min (chatter guard)\n", t0.raises);
  CHECK(t0.raises <= 4, "chatter guard should hold a no-deadband alarm, got %d raises", t0.raises);
}

/* Motor inrush: current spikes above the trip level for 3 s on every start.
 * On-delay 5 s must ignore it; a real overload of 8 s must raise. */
static void s3_spike_vs_overload(void)
{
  printf("S3 start-up spike vs real overload\n");
  powerlytics_rule_cfg_t oc = rule(POWERLYTICS_RULE_THRESHOLD_HIGH, 0, 30, 3, 5000, 0);
  app_rule_runtime_t rt = {0};
  tally_t tl = {0};
  uint32_t t = 0;
  for (int start = 0; start < 5; start++) {
    for (int i = 0; i < 3; i++, t += 1000) feed(&oc, &rt, 60, t, 1, &tl);
    for (int i = 0; i < 30; i++, t += 1000) feed(&oc, &rt, 20, t, 1, &tl);
  }
  CHECK(tl.raises == 0, "spikes raised %d alarms", tl.raises);
  for (int i = 0; i < 8; i++, t += 1000) feed(&oc, &rt, 45, t, 1, &tl);
  CHECK(tl.raises == 1 && tl.raise_value == 45.0f, "overload raises=%d", tl.raises);
}

/* Suction pressure low-low with recovery that bounces inside the off-delay. */
static void s4_low_pressure_bounce(void)
{
  printf("S4 low pressure, recovery interrupted\n");
  powerlytics_rule_cfg_t ll = rule(POWERLYTICS_RULE_THRESHOLD_LOW, 1.5f, 0, 0.3f, 2000, 10000);
  app_rule_runtime_t rt = {0};
  tally_t tl = {0};
  uint32_t t = 0;
  for (int i = 0; i < 5; i++, t += 1000) feed(&ll, &rt, 1.0f, t, 1, &tl);
  CHECK(tl.raises == 1, "raise");
  for (int i = 0; i < 6; i++, t += 1000) feed(&ll, &rt, 2.0f, t, 1, &tl);   /* recovering */
  feed(&ll, &rt, 1.7f, t, 1, &tl); t += 1000;                                  /* between limit and band */
  for (int i = 0; i < 9; i++, t += 1000) feed(&ll, &rt, 2.0f, t, 1, &tl);
  CHECK(tl.recovers == 0, "recovered early (bounce must restart the off-delay)");
  for (int i = 0; i < 3; i++, t += 1000) feed(&ll, &rt, 2.0f, t, 1, &tl);
  CHECK(tl.recovers == 1, "recovers=%d", tl.recovers);
  CHECK(tl.raises == 1, "raises=%d (bounce must not re-raise)", tl.raises);
}

/* Cold-room band 2..8 C, hysteresis 0.5: goes high, then straight to low
 * (defrost fault) with no stop in the band: still one alarm. */
static void s5_band_swing(void)
{
  printf("S5 out of range high -> low without passing through normal\n");
  powerlytics_rule_cfg_t band = rule(POWERLYTICS_RULE_OUT_OF_RANGE, 2, 8, 0.5f, 0, 0);
  app_rule_runtime_t rt = {0};
  tally_t tl = {0};
  feed(&band, &rt, 5, 0, 1, &tl);
  feed(&band, &rt, 9, 1000, 1, &tl);
  CHECK(tl.raises == 1 && tl.last_threshold == 8.0f, "high raise thr=%.1f", tl.last_threshold);
  feed(&band, &rt, 1, 2000, 1, &tl);
  CHECK(tl.raises == 1 && tl.recovers == 0, "swing raises=%d recovers=%d", tl.raises, tl.recovers);
  feed(&band, &rt, 2.2f, 3000, 1, &tl); /* inside band but not past hysteresis */
  CHECK(tl.recovers == 0, "recovered inside the deadband");
  feed(&band, &rt, 3, 4000, 1, &tl);
  CHECK(tl.recovers == 1, "recovers=%d", tl.recovers);
}

/* Avoid a resonance band 1450..1550 rpm (in_range). */
static void s6_in_range(void)
{
  printf("S6 in_range forbidden speed band\n");
  powerlytics_rule_cfg_t res = rule(POWERLYTICS_RULE_IN_RANGE, 1450, 1550, 20, 3000, 0);
  app_rule_runtime_t rt = {0};
  tally_t tl = {0};
  uint32_t t = 0;
  for (float rpm = 1400; rpm <= 1600; rpm += 10, t += 1000) feed(&res, &rt, rpm, t, 1, &tl);
  /* passing through the band at 10 rpm/s takes ~10 s: one alarm, and it clears above 1570 */
  CHECK(tl.raises == 1 && tl.recovers == 1, "raises=%d recovers=%d", tl.raises, tl.recovers);
  app_rule_runtime_t r2 = {0};
  tally_t t2 = {0};
  t = 0;
  for (float rpm = 1400; rpm <= 1600; rpm += 50, t += 1000) feed(&res, &r2, rpm, t, 1, &t2);
  CHECK(t2.raises == 0, "fast run-through raised %d (on-delay should ignore it)", t2.raises);
}

/* Sensor fault while the alarm is active: NaN must not clear it. */
static void s7_sensor_fault(void)
{
  printf("S7 bad reading (NaN) during an alarm\n");
  powerlytics_rule_cfg_t h = rule(POWERLYTICS_RULE_THRESHOLD_HIGH, 0, 100, 5, 0, 0);
  app_rule_runtime_t rt = {0};
  tally_t tl = {0};
  feed(&h, &rt, 120, 0, 1, &tl);
  for (int i = 1; i < 10; i++) feed(&h, &rt, NAN, (uint32_t)i * 1000U, 1, &tl);
  feed(&h, &rt, INFINITY, 11000, 1, &tl);
  CHECK(tl.raises == 1 && tl.recovers == 0 && app_rule_is_active(&rt), "NaN changed the alarm");
  feed(&h, &rt, 50, 12000, 1, &tl);
  CHECK(tl.recovers == 1, "real recovery after the fault");
  app_rule_runtime_t r2 = {0};
  tally_t t2 = {0};
  feed(&h, &r2, NAN, 0, 1, &t2);
  CHECK(t2.raises == 0 && r2.state == RULE_STATE_NORMAL, "NaN raised from normal");
}

/* Event queue full when the alarm should be raised and when it clears. */
static void s8_queue_full(void)
{
  printf("S8 event queue full at raise and at recovery\n");
  powerlytics_rule_cfg_t h = rule(POWERLYTICS_RULE_THRESHOLD_HIGH, 0, 80, 2, 0, 0);
  app_rule_runtime_t rt = {0};
  tally_t tl = {0};
  for (int i = 0; i < 5; i++) feed(&h, &rt, 90, (uint32_t)i * 1000U, 0, &tl); /* queue full */
  CHECK(tl.raises == 0 && !app_rule_is_active(&rt), "went active without an event");
  feed(&h, &rt, 91, 5000, 1, &tl);
  CHECK(tl.raises == 1 && tl.raise_value == 91.0f, "retry raised=%d", tl.raises);
  for (int i = 0; i < 3; i++) feed(&h, &rt, 70, 6000U + (uint32_t)i * 1000U, 0, &tl);
  CHECK(tl.recovers == 0 && app_rule_is_active(&rt), "cleared without an event");
  feed(&h, &rt, 70, 9000, 1, &tl);
  CHECK(tl.recovers == 1 && !app_rule_is_active(&rt), "retry recovered=%d", tl.recovers);
}

/* HAL tick wraps after 49.7 days in the middle of an on-delay. */
static void s9_tick_wrap(void)
{
  printf("S9 tick counter wrap during the on-delay\n");
  powerlytics_rule_cfg_t h = rule(POWERLYTICS_RULE_THRESHOLD_HIGH, 0, 80, 2, 5000, 0);
  app_rule_runtime_t rt = {0};
  tally_t tl = {0};
  uint32_t t = 0xFFFFF000U;
  for (int i = 0; i < 4; i++, t += 1000U) feed(&h, &rt, 90, t, 1, &tl);
  CHECK(tl.raises == 0, "raised early across wrap");
  for (int i = 0; i < 3; i++, t += 1000U) feed(&h, &rt, 90, t, 1, &tl);
  CHECK(tl.raises == 1, "no raise across wrap");
}

static void s10_validation(void)
{
  printf("S10 rule validation\n");
  const char *why;
  powerlytics_rule_cfg_t r;
  r = rule(POWERLYTICS_RULE_OUT_OF_RANGE, 2, 8, 3, 0, 0);
  CHECK(!app_rule_cfg_valid(&r, &why) && strcmp(why, "invalid_rule_hysteresis") == 0, "band hysteresis too wide accepted");
  r = rule(POWERLYTICS_RULE_OUT_OF_RANGE, 8, 2, 0, 0, 0);
  CHECK(!app_rule_cfg_valid(&r, &why) && strcmp(why, "invalid_rule_band") == 0, "low>high accepted");
  r = rule(POWERLYTICS_RULE_IN_RANGE, 5, 5, 0, 0, 0);
  CHECK(!app_rule_cfg_valid(&r, &why), "empty in_range band accepted");
  r = rule(POWERLYTICS_RULE_THRESHOLD_HIGH, 0, 80, -1, 0, 0);
  CHECK(!app_rule_cfg_valid(&r, &why) && strcmp(why, "invalid_rule_hysteresis") == 0, "negative hysteresis accepted");
  r = rule(POWERLYTICS_RULE_THRESHOLD_HIGH, 0, NAN, 0, 0, 0);
  CHECK(!app_rule_cfg_valid(&r, &why) && strcmp(why, "invalid_rule_number") == 0, "NaN limit accepted");
  r = rule(POWERLYTICS_RULE_THRESHOLD_HIGH, 0, 80, 2, 0, 0);
  CHECK(app_rule_cfg_valid(&r, &why), "valid high rejected");
  r = rule(POWERLYTICS_RULE_RISING_EDGE, 0, 0, 0, 0, 0);
  CHECK(app_rule_cfg_valid(&r, &why), "edge rule rejected");
  r = rule(POWERLYTICS_RULE_OUT_OF_RANGE, 2, 8, 2.9f, 0, 0);
  CHECK(app_rule_cfg_valid(&r, &why), "narrow but valid band rejected");
}

/* Gateway resets while the alarm is active and the level is still high:
 * the restored rule must not raise again, and must clear when it drops. */
static void s11_restore_after_reset(void)
{
  printf("S11 reset with an active alarm\n");
  powerlytics_rule_cfg_t h = rule(POWERLYTICS_RULE_THRESHOLD_HIGH, 0, 80, 2, 5000, 5000);
  app_rule_runtime_t rt;
  tally_t tl = {0};
  app_rule_restore_active(&rt, 0);
  for (int i = 0; i < 20; i++) feed(&h, &rt, 90, (uint32_t)i * 1000U, 1, &tl);
  CHECK(tl.raises == 0, "duplicate raise after reset");
  for (int i = 20; i < 70; i++) feed(&h, &rt, 70, (uint32_t)i * 1000U, 1, &tl);
  CHECK(tl.recovers == 0, "restored alarm recovered before the hold time");
  for (int i = 70; i < 90; i++) feed(&h, &rt, 70, (uint32_t)i * 1000U, 1, &tl);
  CHECK(tl.recovers == 1, "no recovery after reset");
}

/* Disabled rule and a 0 ms rule firing on the crossing sample. */
static void s12_misc(void)
{
  printf("S12 disabled rule, zero on-delay\n");
  powerlytics_rule_cfg_t h = rule(POWERLYTICS_RULE_THRESHOLD_HIGH, 0, 80, 2, 0, 0);
  h.enabled = 0;
  app_rule_runtime_t rt = {0};
  tally_t tl = {0};
  feed(&h, &rt, 99, 0, 1, &tl);
  CHECK(tl.raises == 0, "disabled rule raised");
  h.enabled = 1;
  app_rule_runtime_t r2 = {0};
  tally_t t2 = {0};
  feed(&h, &r2, 70, 0, 1, &t2);
  feed(&h, &r2, 85, 1000, 1, &t2);
  CHECK(t2.raises == 1 && t2.raise_value == 85.0f && t2.first_raise_ms == 1000, "0 ms rule raised at %u", t2.first_raise_ms);
}

/* Slow sampling vs on-delay: with 10 s samples a 5 s on-delay raises on the
 * second sample above the limit (10 s), never before. */
static void s13_slow_sampling(void)
{
  printf("S13 sample period longer than the on-delay\n");
  powerlytics_rule_cfg_t h = rule(POWERLYTICS_RULE_THRESHOLD_HIGH, 0, 80, 2, 5000, 0);
  app_rule_runtime_t rt = {0};
  tally_t tl = {0};
  feed(&h, &rt, 90, 0, 1, &tl);
  CHECK(tl.raises == 0, "raised before the on-delay");
  feed(&h, &rt, 90, 10000, 1, &tl);
  CHECK(tl.raises == 1 && tl.first_raise_ms == 10000, "raise at %u", tl.first_raise_ms);
}

static void s14_same_meaning(void)
{
  printf("S14 rule identity across a redeploy\n");
  powerlytics_rule_cfg_t a = rule(POWERLYTICS_RULE_THRESHOLD_HIGH, 0, 80, 2, 5000, 0);
  powerlytics_rule_cfg_t b = a;
  b.duration_ms = 9000;
  b.severity = POWERLYTICS_SEVERITY_CRITICAL;
  CHECK(app_rule_same_meaning(&a, &b), "duration/severity change must keep the alarm");
  b.high = 85;
  CHECK(!app_rule_same_meaning(&a, &b), "a new limit is a new alarm");
  b = a;
  b.enabled = 0;
  CHECK(!app_rule_same_meaning(&a, &b), "disabling is a change");
}


/* Floating analog input (bench AI_1: 240..1150 at random, sampled every
 * 50 ms) against high 850 / hysteresis 100: a few events, then one held
 * alarm; it clears once the input sits low for the hold time. */
static void s15_floating_input(void)
{
  printf("S15 floating input (chattering alarm)\n");
  powerlytics_rule_cfg_t h = rule(POWERLYTICS_RULE_THRESHOLD_HIGH, 0, 850, 100, 0, 0);
  app_rule_runtime_t rt = {0};
  tally_t tl = {0};
  uint32_t seed = 12345U, t = 0;
  for (int i = 0; i < 12000; i++, t += 50U) { /* 10 minutes */
    seed = seed * 1103515245U + 12345U;
    float v = 240.0f + (float)((seed >> 16) % 911U);
    feed(&h, &rt, v, t, 1, &tl);
  }
  printf("  info: %d raises, %d recoveries in 10 min\n", tl.raises, tl.recovers);
  CHECK(tl.raises <= 3 && tl.recovers <= 2, "floating input flooded: %d raises", tl.raises);
  CHECK(app_rule_is_active(&rt), "alarm should be held active while chattering");
  for (int i = 0; i < 1100; i++, t += 50U) feed(&h, &rt, 500, t, 1, &tl); /* 55 s low */
  CHECK(app_rule_is_active(&rt), "held recovery released early");
  for (int i = 0; i < 200; i++, t += 50U) feed(&h, &rt, 500, t, 1, &tl);
  CHECK(!app_rule_is_active(&rt), "held recovery never released");
  /* After a real recovery the guard starts afresh: a clean alarm raises at once. */
  int before = tl.raises;
  feed(&h, &rt, 900, t, 1, &tl);
  CHECK(tl.raises == before + 1, "fresh raise after chatter recovery");
}

/* Real process that legitimately cycles (batch heater every 2 min) is not
 * treated as chattering. */
static void s16_cycling_not_chatter(void)
{
  printf("S16 slow legitimate cycling is not chatter\n");
  powerlytics_rule_cfg_t h = rule(POWERLYTICS_RULE_THRESHOLD_HIGH, 0, 80, 2, 0, 0);
  app_rule_runtime_t rt = {0};
  tally_t tl = {0};
  uint32_t t = 0;
  for (int cycle = 0; cycle < 10; cycle++) {
    for (int i = 0; i < 30; i++, t += 1000U) feed(&h, &rt, 90, t, 1, &tl);
    for (int i = 0; i < 90; i++, t += 1000U) feed(&h, &rt, 70, t, 1, &tl);
  }
  CHECK(tl.raises == 10 && tl.recovers == 10, "raises=%d recovers=%d", tl.raises, tl.recovers);
}

int main(void)
{
  s1_tank_h_hh();
  s2_noise_chatter();
  s3_spike_vs_overload();
  s4_low_pressure_bounce();
  s5_band_swing();
  s6_in_range();
  s7_sensor_fault();
  s8_queue_full();
  s9_tick_wrap();
  s10_validation();
  s11_restore_after_reset();
  s12_misc();
  s13_slow_sampling();
  s14_same_meaning();
  s15_floating_input();
  s16_cycling_not_chatter();
  printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
