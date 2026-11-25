# Rule actions: a rule drives a relay (firmware 1.0.19, FH-018)

A threshold or range rule may switch a relay while its alarm is active, with no cloud involved, and put it back when the
alarm clears. The platform side is in `powerlytics-rebuild-actuation` (change-log 39).

## Wire (inside a rule object of the configuration; additive, all optional)

| Field | Meaning |
|---|---|
| `actPort` | `"RO_1"` to `"RO_8"`: the relay to drive. A rule without it behaves exactly as before. |
| `actRaise` | `0` or `1`: the relay state while the alarm is active. Required with `actPort`. |
| `actClear` | `0` or `1`: the state to put it in when the alarm clears. Omitted: the relay is left where it is. |

Only analog threshold and range rules (`threshold_high`, `threshold_low`, `out_of_range`, `in_range`) may carry an action; an
edge rule has no recovery. A badly named action (`actPort` not `RO_1`..`RO_8`, no `actRaise`, or on an edge rule) refuses the
whole configuration with the reason `invalid_rule_action`. Older firmware ignores the three fields (it would then not act: the
platform only sends them to units that reported 1.0.19 or later in `hello`).

The rule's event gains `data.act` (`"RO_1=0"`: what this edge does to the relay). The relay's own state report shows what really
happened.

## Behaviour (`Core/Src/app_rule_action.c`, 26 host checks in `scripts/rule_action_host_test.c`)

- A raise **takes** the relay: it is held at `actRaise` while the alarm is active. The wanted state is written on every sample
  tick until the relay driver accepts it (its 500 ms chatter guard refuses a change too soon after the last), so a protective
  action is never lost.
- A command from outside (the platform) that would put a held relay in the **other** state is refused with the ack reason
  `rule_active`. A command to the held state is accepted.
- A **clear** gives the relay back: `actClear` is written if present, then the relay is free. Only the rule that holds a relay can
  release it; with two rules on one relay the last raise wins.
- A rule that disappears while holding a relay (deleted, disabled, changed, new configuration) releases it without touching its
  state: nobody knows what is safe.
- After a reset or a redeployment, a rule still in alarm (restored from backup SRAM) takes its relay again.

## Stored configuration stays valid

The configuration is stored as a raw struct whose length is checked on load. The action is packed in **one byte inside the
existing end padding** of `powerlytics_rule_cfg_t`, so `sizeof` stays 80 and every configuration stored by an earlier firmware loads
unchanged (a `_Static_assert` enforces it). A first draft with four separate fields grew the struct by 4 bytes and made the bench
unit refuse its stored configuration (no device key, no telemetry) after the update; it was caught on the bench and redesigned.
The rule hash used for alarm restore is unchanged for a rule without an action, so alarms kept across the update still match.

## Bench verification (2026-11-25, B1-V4-C, 1.0.19)

A temporary self-test rule (removed again): on a fresh raise the log showed `[RULE] AI_1 raise -> RO_2 ON` then `[RELAY] RO_2 ON`;
the event carried `"act":"RO_2=1"`; before the raise a command to either state was allowed, after it OFF was refused and ON
accepted; after a reset the alarm was restored and RO_2 was written ON again. Not yet tested: a rule delivered by a real
configuration deployment, a manual command refused over MQTT (`rule_active` ack), the clear and restore path on hardware
(covered by the host checks), contact feedback.
