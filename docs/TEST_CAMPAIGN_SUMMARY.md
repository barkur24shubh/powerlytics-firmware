# Test campaign summary: B1-V4-C gateway and Powerlytics platform (2026-11-14 → 2026-11-19)

The one page to read first. It lists what was tested, what was wrong, what changed and what is still open.

- **Bench unit:** B1-V4-C, IMEI 863110085107293.
- **Firmware:** 1.0.4 → **1.0.18**.
- **Detailed evidence:** test by test in [`TEST_CAMPAIGN.md`](TEST_CAMPAIGN.md) (this repository).
- **Platform record:** issue register ISS-115 … ISS-133 and change-log entries 12–30 in `powerlytics-rebuild` (`docs/issues/register.md`, `docs/program/change-log.md`, `docs/program/firmware-hardware-register.md`).

## State at the end of the campaign (2026-11-19)

| Item | State |
|---|---|
| Firmware | **1.0.18** on `main` (`16c06bf`), released to R2 and activated. The bench unit runs it, installed over the air. **Not yet rolled out to other units.** |
| Platform (backend + frontend) | All changes on `main` (`84a2ddc`) and deployed to production |
| Bench option bytes | Brown-out reset level 3 (2.7 V) and bootloader write protection **on**; readout protection **off** (bench only) |
| Bench telemetry interval | 1 s (best effort) |

---

## ⚠️ Critical items

### Fixed critical problems (would have hurt in the field)

| # | Problem | Impact before the fix | Fixed in |
|---|---|---|---|
| C-1 | SHA-256 bug: inputs whose length mod 64 is 56–63 hashed wrongly | About 1 in 8 config sizes were always refused (`hash_mismatch`). **OTA images of those sizes would also have failed**, because the bootloader uses the same code | 1.0.6 |
| C-2 | Config kept in one flash sector, erased then written | A power cut during a save left the unit with **no config and no device key** until redeployed | 1.0.6 (A/B copies in external flash) |
| C-3 | The application region overlapped the config sector | A larger build or OTA image could overwrite the config | 1.0.6 (linker limit 352 KB) |
| C-4 | Broker certificate not verified (`authmode 0`) | Any server could pose as the broker and command every unit | 1.0.13 |
| C-5 | **Device API key sent in every log message** to the broker | Key exposed to anyone reading the broker | 1.0.14 + platform |
| C-6 | **Shared broker username and password printed in clear over USB** (and on the SD log) on every connect | Anyone with a USB cable on one unit had the login for **the whole fleet** | 1.0.18 |
| C-7 | A steady stream of relay commands starved telemetry; the platform then marked the unit offline and refused commands | No telemetry for minutes; commands refused while the unit was fine | 1.0.7 + platform |
| C-8 | Relay commands arriving during an OTA silently lost | Operator thinks a relay switched when it did not | Platform refuses with `OTA_IN_PROGRESS` |
| C-9 | Relay safe state stored but never sent to the unit; no chatter guard | Relays held their last state on link loss; a relay could be switched 20× in 2 s | 1.0.8 + platform |
| C-10 | No network time from the operator → clock never set → **every relay command refused** (`device_clock_invalid`); after a backup loss a fake date was marked valid | Units on such networks could not be controlled; data stamped 1970 or months off | 1.0.16 (NTP fallback, clock from `issuedAt`, no fake valid date) |
| C-11 | Sequence store wrote one flash sector on every telemetry | That sector would wear out in ~4 years at 5 s, ~1 year at 1 s | 1.0.17 (16-sector ring) |
| C-12 | Alarms: platform opened both H and HH on one event; device lost raises or recoveries on a full queue and forgot active alarms on reset or deploy | Wrong or stuck alarms | 1.0.12 + platform |
| C-13 | Most device failures never reached the cloud; routine modem problems logged as FATAL | Blind to real faults, flooded with false critical alerts | 1.0.14 |
| C-14 | A platform outage sent users to the login and then to a home page showing **"0 devices / No devices yet"** | Customers would think their devices were deleted | Platform |
| C-15 | A watchdog lock-up raised no alert; open alerts never escalated, so "prolonged offline" never turned critical | Lock-ups invisible; offline units never escalated | Platform |
| C-16 | Hourly/daily rollups used only the last value of each window | **Peaks lost in history** (bench: max 983 recorded, real 1042) | Platform; **run the recompute script once** (below) |

### Open critical risks (not fixed)

| # | Risk | What to do |
|---|---|---|
| R-1 | **One shared broker login for every unit**, compiled into every image (G4). Before 1.0.18 it was also printed over USB, so treat it as exposed | Build per-unit broker logins (G4), then **rotate the shared password** |
| R-2 | **Readout protection is off** on units unless production programming is done | Follow `docs/PRODUCTION_PROGRAMMING.md` for every shipped unit (BOR 3, bootloader write protection, RDP Level 1, **never Level 2**) |
| R-3 | **Telemetry is not buffered while offline** (100 % loss during an outage; only logs and 8 events are kept) | G5, SD-card store-and-forward, parked until a card is fitted on the bench |
| R-4 | **No downgrade guard**: any admin can send a unit back to an older firmware, including 1.0.17 (prints the broker login) | Require an explicit "allow downgrade" confirmation (owner decision pending) |
| R-5 | Release images are built from the Debug configuration (`-O0 -g3`) | Optimised release build after a full regression run |
| R-6 | 1.0.18 is not on the fleet yet | Roll out through the OTA console after the platform deploy (done) |

---

## ⚠️ Device contract changes (every one was announced; older firmware keeps working unless noted)

| Change-log | Change | Type |
|---|---|---|
| 14 | Config envelope limit 6 144 B (was 8 192); error acks matched by `deploymentId`; `configHash` in telemetry | Limit + additive |
| 16 | Config carries `safeState` and `relayLinkLossSafeMs`; hello `presenceMs`; platform refuses commands to a unit silent > 3 × min(interval, presence) | Additive + interpretation |
| 20 | Device events matched to alert rules by `threshold`; event `reason`, `suppressed`, `context` kept; missed configs re-sent on hello | Interpretation + additive |
| 21 | Hello `security {tls, rdp, bor}` accepted | Additive |
| 22 | MQTT logs **without `apiKey`** accepted and attributed by topic IMEI; `seq` in logs; logs stored once | Interpretation + additive. **The platform had to be deployed before 1.0.14** (done) |
| 23 | `telemetryInterval` may be **1000 ms** (best effort; floor was 5 s); health `kind full/heartbeat`, `mem`, `events`, `logs`, `tls`, `rtcValid`; QoS 0 for telemetry and health at intervals ≤ 2 s (1.0.15) | Limit + additive + QoS |
| 25 | A unit with an unset clock takes the actuation `issuedAt` as the time (1.0.16): `issuedAt` must stay the server send time | Interpretation |
| 28 | Reset reason `WATCHDOG`/`WINDOW_WATCHDOG` raises "Device restarted after a lock-up" | Interpretation |
| 30 | Identical `ota_ack` repeats ignored; a hello after a restart mid-download fails the update (`interrupted_by_restart`) | Interpretation |

---

## Firmware releases during the campaign

| Version | Date | What changed |
|---|---|---|
| 1.0.4 | 11-17 | Relay commands run on arrival, acks never lost; clock and boot fixes |
| 1.0.5 | 11-17 | Correct reset reasons (power-on vs brown-out, OTA, fault, watchdog); random bootId; backup-register map fixed |
| 1.0.6 | 11-17 | **SHA-256 padding fix (C-1)**; config A/B in external flash (C-2); app region limit (C-3); over-limit configs refused with a reason; `too_large`; telemetry period from cycle start; `configHash` in telemetry |
| 1.0.7 | 11-17 | Relay acks no longer starve telemetry (C-7); ack outbox 8 → 24 |
| 1.0.8 | 11-18 | Relay safe states (boot and link loss), 500 ms chatter guard, readable refusals, 30 s presence keepalive |
| 1.0.9 | 11-18 | Relay output change reported at once |
| 1.0.10 | 11-18 | `bootId` in telemetry; SD log copies the cloud snapshot; downlink parse arena (large configs no longer crash) |
| 1.0.11 | 11-18 | Correct event recovery; no reused event ids after power loss; event drops reported |
| 1.0.12 | 11-18 | Alarm rules rewritten: no lost events, state kept across deploy/reset, chatter guard, rule validation, edge debounce, context keys |
| 1.0.13 | 11-18 | **Broker certificate verified (C-4)**; security posture in hello |
| 1.0.14 | 11-18 | **No API key in logs (C-5)**; failures reach the cloud; correct log levels; reliable offline log store; missing W25Q reported |
| 1.0.15 | 11-19 | Health every 5 min plus a 25 s heartbeat, memory/log/event fields; 1 s best-effort telemetry (QoS 0, interval pacing) |
| 1.0.16 | 11-19 | Clock: no fake valid date, NTP fallback, clock from a command's `issuedAt` (C-10) |
| 1.0.17 | 11-19 | Sequence store as a 16-sector ring (C-11) |
| 1.0.18 | 11-19 | **Broker credentials no longer printed on USB (C-6)** |

Also released in the run-up (11-14 → 11-16): 1.0.1–1.0.3, OTA through the RSA proxy, bootloader flash pin fix, downlinks never dropped, OTA rollbacks reported.

## Platform changes (backend and frontend), by area

| Area | Change (change-log entry) |
|---|---|
| Relays | Controls no longer stick on "Sending…"; stale relay reports ignored (12). Commands refused during OTA, any message counts as presence, history paged, late acks kept (15). Safe state / link-loss settings in the UI, silent units refused (16). "Uncertain" follows the reporting period; relay history on the Controls tab; physical result recorded (17) |
| Boot & identity | Admin "Boot & Identity" card: firmware, reset reason, boot count, boot id, mismatches (13); missed-telemetry count (18); security badges TLS/RDP/BOR (21, 29) |
| Config | 6 KB limit, error acks by deploymentId, diff before deploy, sampling/debounce/counter-width settings (14); missed configs re-sent on hello, stale deployments superseded (20) |
| Telemetry & live | Correct gap detection (18); live updates from every API instance, token renewal instead of sign-out, stream back-off (19); 1 s option with a warning (23) |
| Alarms & alerts | H/HH matched by threshold, event reason/context kept (20); watchdog-reset alert and alert escalation (28) |
| Logs & health | Keyless MQTT logs, each log stored once, future clocks clamped (22); health history (30 days) and admin Health card with trends (23); log times before 2025 replaced (25) |
| Audit | Relay commands (sent and refused), port, role, invite, workspace, alert, channel and staff actions audited with before/after; filters and CSV export (24) |
| History & export | Rollups from device window stats plus a recompute script; streamed CSV export; device events expire after 365 days (26) |
| UI | One number format; one stale rule; outage screen instead of "0 devices" (27) |
| OTA | Acks recorded once across instances; restart mid-download fails the update at once (30) |

---

## Test results by section

✅ pass (or fixed and retested) · ⚠️ partly · ❌ open · ⏸ parked / not run

| § | Area | Result | Main findings |
|---|---|---|---|
| 1 | Boot and identity | ✅ | Reset reasons fixed (power-on was reported as brown-out); bootId unique; boot count survives power loss. ⏸ boot without SIM/antenna/Modbus |
| 2 | Onboarding | ⚠️ | Manufacture → claim → config → telemetry works. ⏸ G1 (a decommissioned unit can never be re-claimed), G2 (transfer half-built), G3 (prompt) |
| 3 | Config downlink | ✅ | C-1, C-2, C-3 fixed; limits agree; deploy in 7.8 s; drift alert works |
| 4 | Downlink speed | ✅ | Click → relay p50 0.5 s; 496/496 executed; telemetry starvation fixed; burst 20 in 2 s in order. ⏸ commands during reconnect (clean session) |
| 5 | Relays SW → HW | ✅ | Pins follow commands; duplicate and expired commands handled; safe states and chatter guard added; physical SIM-removal test passed |
| 6 | Relays HW → SW | ✅ | Local change seen in 5.4 s even at a 1 h interval; relay history and physical result added. ⏸ contact feedback (hardware) |
| 7–10 | I/O and Modbus | ⏸ | Waiting for the RS485 adapter; DI_7/DI_8 pulse counters not implemented in firmware |
| 11 | Telemetry pipeline | ⚠️ | Gaps, replays, SD seq theft and large configs fixed. ❌ no offline buffering (R-3, G5) |
| 12 | Live data | ✅ | ~0.4 s device → screen; multi-instance and token-expiry problems fixed |
| 13 | Events | ✅ | Recovery semantics, event ids after power loss and drop reporting fixed. ⏸ physical edge timing (hardware) |
| 14 | Alarm rules | ✅ | 15 plant scenarios pass on 1.0.12 (52 host checks) |
| 15 | Health | ✅ | 2.8 → 0.5 MB/day; trends in admin |
| — | 1 s telemetry | ✅ best effort | 1 s: 41–44 readings/min, p50 0.4 s, 1.2 % lost (good signal); 5 s: 0 lost. 500 ms not possible on this modem |
| 16 | Device logs | ✅ | C-5, C-13 fixed; offline log store reliable |
| 17 | Audit log | ✅ | Every operator action audited; export |
| 19 | Time | ✅ | 5 ppm drift; C-10 fixed |
| 20 | Watchdog | ✅ | Real lock-up reset in 35 s, cause reported and stored; alert added |
| 21 | Storage | ✅ | C-11 fixed; SD handling safe (no card on the bench). Open: SD rotation when full; hot-inserted card used only after a reboot |
| 23 | Security | ⚠️ | C-4 fixed; cross-workspace access refused; viewers cannot act. ❌ R-1 shared broker login; ⚠️ commands without `expiresAt` and configs have no replay check (follows R-1) |
| 24 | Data history | ✅ | C-16 fixed; streamed export; retention checked |
| 25 | UI | ✅ | Number format, stale rule, outage screen (C-14); phone width and empty states pass |
| 27 | Production programming | ✅ procedure | Procedure written and applied on the bench (BOR, bootloader write protection; 15 min soak clean); C-6 fixed. R-2, R-5 open |
| — | OTA end to end | ✅ | Update, interrupted download, rollback and re-update all work; duplicate records and slow interruption detection fixed; R-4 open |
| 18, 22, 26, 28+ | — | ⏸ | Scope not recorded in the repository; to be confirmed with the owner |

## Parked items (agreed not now)

| Id | What |
|---|---|
| G1 | Return a decommissioned unit to stock so it can be claimed again |
| G2 | Finish or remove workspace transfer |
| G3 | Decommission prompt should say it is permanent |
| G4 | Per-unit broker logins and topic ACLs (then rotate the shared login) |
| G5 | SD-card store-and-forward and backfill, plus platform backfill support |
| — | 7–10 I/O and Modbus tests (RS485 adapter); relay contact feedback; DI pulse counters; boot without SIM/antenna |
| — | Proposals: `latchMode` on the device; 4–20 mA sensor-fault detection (NAMUR NE43); more than 2 rules per point |

## To do after this campaign

1. **Roll out firmware 1.0.18** to the fleet through the OTA console (platform is already deployed).
2. **Run the rollup recompute once** to correct 90 days of history (C-16), from `apps/api-powerlytics`:
   `pnpm exec ts-node --transpile-only src/scripts/recompute-rollups.ts` (dry run), then add `--execute`.
3. **Production programming** for every unit before shipping (`docs/PRODUCTION_PROGRAMMING.md`).
4. Decide on the **downgrade guard** (R-4) and schedule **G4** (per-unit broker logins), then rotate the shared broker password.
5. Fit an SD card on the bench to start **G5**.
