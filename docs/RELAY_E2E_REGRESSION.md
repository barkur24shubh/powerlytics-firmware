# Relay end-to-end regression (UI → backend → MQTT → device → relay → ack → UI)

Bench unit: B1-V4-C, IMEI 863110085107293, relays RO_1 (PB15, feedback PD12) and RO_2 (PB14, feedback PD11).

## How a relay command flows

1. UI toggle → `POST /…/devices/:id/actuations` `{portKey, action: TURN_ON|TURN_OFF, idempotencyKey}`.
2. Backend (`device-core/src/actuation/actuationService.ts`): refuses if the device is not ONLINE (409), rate-limits one command per port per second (429), dedupes by idempotency key, marks older in-flight commands for the port SUPERSEDED, publishes `{type:"actuation", actuationId, portKey, action, issuedAt, expiresAt(+60 s)}` to `c2d/<imei>/commands…` at QoS 1, then sets SENT.
3. Device: modem URC hook queues the downlink (`[MQTT RX] downlink queued`), runtime pops it first in the loop, `handle_actuation_cjson` checks `expiresAt` against the RTC, drives the relay GPIO (`[RELAY_FB] … requested=… feedback=…`), publishes **one** `actuation_ack` (`ACKED` / `FAILED` / `EXPIRED`) on `d2c/<imei>/message…`.
4. Backend `ackActuation`: ACKED updates `ports.$.actuationConfig.currentState`; late acks for SUPERSEDED, and ACKED for EXPIRED, are ignored. Commands never acked show as unconfirmed after 2 min.

## Where to look (correlate everything by actuationId)

| Layer | Evidence |
|---|---|
| UI | toggle state, pending spinner, error toast, network response status |
| Backend | `fly logs -a bananapin-api-powerlytics`, actuation list API (status per command) |
| Device | USB log: `[MQTT RX] downlink queued`, `[MQTT RX][ERR] downlink dropped reason=…`, `[ACT] acknowledgement sending actuationId=… status=…`, `[RELAY_FB]` |
| Hardware | `[RELAY_FB] feedback=` (contact feedback pin), relay click / LED |

Pass = every command sent reaches a final status, the device log shows exactly one execution and ack per actuationId, feedback matches the request, and the UI ends in the state of the last command.

## Scenarios

### A. Basic function
| # | Scenario | Expected |
|---|---|---|
| A1 | RO_1 ON, then OFF | ACKED each, feedback follows, UI follows within ~2 s |
| A2 | RO_2 ON, then OFF | same |
| A3 | RO_1 ON does not move RO_2 | RO_2 feedback unchanged |
| A4 | ON when already ON | ACKED, no change |

### B. Timing and load
| # | Scenario | Expected |
|---|---|---|
| B1 | Two commands on one port < 1 s apart | 2nd refused 429 (UI disables toggle while pending) |
| B2 | RO_1 and RO_2 within 1 s | both delivered and acked |
| B3 | 20 commands, ~1.5 s apart, alternating | 20/20 acked, final state = last command |
| B4 | Commands while telemetry runs every 500 ms | no loss (current bench build) |
| B5 | Commands while DI events are firing | no loss |
| B6 | Command during a config deployment | delivered after/around config, no loss |
| B7 | Command during an OTA download | defined behaviour (reject or run), never silently lost |

### C. Delivery faults
| # | Scenario | Expected |
|---|---|---|
| C1 | Device offline (antenna/power off) | UI refuses: DEVICE_OFFLINE, nothing published |
| C2 | Device drops off after SENT, back after > 60 s | broker redelivers; device answers EXPIRED; relay unchanged |
| C3 | Same, back in < 60 s | runs, ACKED |
| C4 | Ack publish fails | relay switched but UI unconfirmed; next telemetry corrects state |
| C5 | QoS 1 redelivery of the same actuationId | runs at most once (or harmlessly twice for ON/OFF) |
| C6 | Double-click / same idempotency key | one command |
| C7 | New command while the previous is pending | previous SUPERSEDED, its late ack ignored |

### D. State truth
| # | Scenario | Expected |
|---|---|---|
| D1 | Device reboot with relay ON | documented boot state; UI shows what the device reports |
| D2 | Contact does not follow coil (no load / failed relay) | today: ACKED anyway (ack ignores feedback) — gap |
| D3 | Telemetry relay state vs last ack | UI reconciles to telemetry |
| D4 | Two browsers on the same device | both update live |

### E. Validation
| # | Scenario | Expected |
|---|---|---|
| E1 | Non-output port / unknown port / bad action / non-ACTIVE device / other workspace | 400 / 404 / 400 / 400 / 404 |
| E2 | Device gets unknown portKey or missing fields | FAILED with reason |
| E3 | Device clock invalid | FAILED device_clock_invalid |

### F. Soak
| F1 | One command every 5 s for 30 min under 500 ms telemetry | 100 % acked; record p50/p95 command→ack latency |

## Known gaps found while writing this (2026-11-16)

1. **Ack is sent once, never retried.** If that publish fails the relay has switched but the UI shows unconfirmed.
2. **Ack ignores relay feedback.** ACKED means "GPIO driven", not "contact closed".
3. **Device clock is wrong** (RTC 2026-08-27 on 2026-11-16, no network time sync). `expiresAt` is compared with it, so expiry never triggers (C2 fails) and telemetry/event timestamps are off by months.
4. Expiry assumes the RTC is IST (fixed −5:30).
5. **MQTT connects with clean session = 1** (`AT+CMQTTCONNECT=…,60,1,…`): the broker discards any command sent while the unit is reconnecting.
6. **Any failed publish starts a 15 s cooldown** in which every publish, acks included, is skipped and not retried.
7. **Every normal publish waits out a 3 s minimum gap** (`MQTT_PUBLISH_MIN_GAP_MS`) on the runtime task, and the runtime task is also the one that executes downlinks, so a command can wait several seconds behind telemetry.
8. Always check which image is on the unit before a run. On 2026-11-16 the bench unit was running 1.0.2 (`2026-11-15-ota-confirm-fix`, no downlink-queue fix) because an OTA to 1.0.2 had been run after the 1.0.3-based bench build was flashed; the missed commands seen then were the 1.0.2 downlink bug, not the bench build.

## Results

Fill per run: date, firmware build, telemetry interval, scenario, commands sent / acked / lost, notes.

### 2026-11-16 23:52–23:59 IST — bench build (1.0.3 + no event cap, 500 ms override), cloud interval 5 s, production UI/API

| Scenario | Sent | Reached device | Acked | Result |
|---|---|---|---|---|
| A1 RO_1 off/on | 2 | 2 | 2 | pass |
| A2 RO_2 off/on, A3 RO_1 untouched | 2 | 2 | 2 | pass |
| B1 double-click RO_1 | 1 (UI blocked the 2nd) | 1 | 1 | device OK; **UI then showed ON while relay was OFF** (cause not yet confirmed) |
| B2 RO_1 + RO_2 within 1 s | 2 | 2 | 2 | device OK; **UI left RO_1 stuck on "Sending…" for good** |
| B3 12 commands, 6 s apart, alternating | 12 | 12 | 12 | pass; final state matches |

Timing: POST 1.3–1.4 s (waits for broker PUBACK); POST → device queue ≈ 0.3–1 s; execute ≈ 10 ms after queue; ack published 1.3–2.9 s later (3 s min publish gap); UI shows result on the next 5–6 s poll.

Bugs found:
- **UI (RelayControlsPanel):** both relays share one `useSendActuationMutation`; React Query only runs per-call `onSuccess` for the last `mutate`, so the earlier relay never gets its commandId and never leaves "Sending…". The 20 s give-up never fires either, because the reconcile effect only re-runs when the polled list changes.
  Fixed locally on backend branch `fix/relay-controls-pending` (per-call `mutateAsync`, 1 s tick for the give-up, same-frame double-click guard, acked state written into the device cache). Retested 2026-11-17 00:05 on the local UI (localhost:3003 → local api-powerlytics → real EMQX): B2 both relays cleared and correct; B1 double-click sent exactly one command and the tile ended in the relay's real state.

### 2026-11-17 00:12 IST — firmware reliability build (ack outbox + retry, gap wait serves downlinks, duplicate-id re-ack, feedback after 30 ms, network clock → RTC), local UI

- Clock: `[CLOCK] RTC set from network time 2026-11-17T00:11:45+05:30 (drift was -7056553 s)` — 81 days fixed, matches the host clock to the second.
- 6/6 commands delivered, executed, acked; UI final state matches.
- queue → execute: 6.1, 1.8, 0.7, 3.0, 0.4 s, 0.6 s — the command still waits for the telemetry publish that is in progress (telemetry runs back to back at the 500 ms bench override).
- ack queued → published: 1.1–2.5 s (four AT steps); no 3 s gap any more.
- **D1 fails:** after the reboot both relay outputs were OFF (GPIOB ODR = 0) while the UI showed both ON. Nothing restores relay state at boot or reports it afterwards.
- Feedback after 30 ms still OFF while driven ON: hardware/wiring, not timing.


### 2026-11-17 00:20 IST — relay state reported after reboot (D1)

- Firmware now always reports `RO_n.last` in telemetry, even when the relay point is disabled in config (the platform never gates relay reports by port status, `telemetryService.isInactiveMeasurement`).
- D1 **pass**: RO_1 ON (PB15 high, UI ON) → reboot → relays come up OFF → first telemetry 9 s after ONLINE reports `RO_1.last=0` → UI shows OFF "reported just now".
- Boot to ONLINE: 43.7 s (was 57 s) after cutting fixed waits: USB grace 8 → 1 s, modem settle 8 → 3 s, runtime start 5 → 0.5 s, comm start 5 → 0.5 s. Remaining big item: flash-log region scans, about 8 s each, three times at boot (cause not yet found; SPI runs at full speed).
- Open: a telemetry snapshot built just before a relay switch and published after the ack can briefly show the old state until the next report; the backend could ignore reports older than `lastCommandAt`.

- **Hardware/firmware:** relay feedback pins PD12/PD11 read OFF (IDR bits = 1, active-low) even 8 s after RO_1 is driven ON (PB15 high). Either the contact is not closing on the bench or the feedback circuit is not wired; and the firmware reads feedback in the same millisecond it drives the coil, so the logged value is meaningless anyway.

### 2026-11-17 00:38–00:51 IST — firmware 1.0.4 (commits 7a1534a, f774334, 44afc12), cloud interval 5 s, local UI

| Scenario | Result |
|---|---|
| 10 commands, 3 s apart, alternating (fast path) | 10/10 switched on arrival (0 ms after the block ended); acks 1.1–2.5 s |
| B6 config deploy + both relays at once | config (3 116 B) queued, both relays switched on arrival; acks at +1.1 s and +2.1 s, before the config finished applying (after 44afc12; before it the acks waited ~7 s) |
| Soak: 24 clicks, 5 s apart, alternating | 23 commands sent (one click hit a tile still shown as pending, button disabled by design); 23/23 switched on arrival; 23/23 acks published, median 1.25 s, max 4.2 s; final UI state matches the relays |
| E1 validation | covered by `actuationService.test.ts` (400 bad action / non-output port / non-ACTIVE, 404 unknown port / device, 409 offline, 429 rate limit, idempotency, cross-device isolation) |
| D1 reboot | relays come up OFF; first telemetry after reconnect reports them and the UI follows |

Open: the UI learns an ack by polling every 5 s, so "Sending…" can last up to ~10 s after the relay switched; pushing actuation status over the existing device stream would make it immediate. Not run (deferred by the owner): C1–C3 offline/reconnect, relay contact feedback.
