# Test campaign: B1-V4-C (IMEI 863110085107293)

One section at a time: test, record, agree fixes with the owner, fix, retest.
Tags: [FW] device only · [HIL] needs physical stimulus · [E2E] device to UI · [SW] backend/UI only.

## Parked: found, agreed not to implement now

| Id | What | Parked on | Why it matters |
|---|---|---|---|
| G1 | Return a decommissioned unit to stock so it can be claimed again (admin action, wipes workspace settings and key, new claim code; history stays with the old workspace, reads must filter by workspace) | 2026-11-17 | A returned, repaired or resold unit cannot be onboarded; also blocks the claim-to-first-value timing test |
| G2 | Finish workspace transfer (accept/complete, UI, check the target workspace), or remove the half-built route | 2026-11-17 | The route stores a request nothing ever acts on |
| G3 | Decommission prompt should say the step is permanent | 2026-11-17 | Users may think the unit can be reused |
| — | Boot with no SIM / antenna / Modbus / secondary MCU | 2026-11-17 | Needs bench hardware changes |
| — | Boot-to-online time budget (now ~44 s) | 2026-11-17 | No target agreed |

The tests named `GAP:` in `packages/device-core/src/onboarding.flow.test.ts` (backend) describe G1 and G2 as they are today; flip them when the gaps are fixed.

## 1. Boot and identity (2026-11-17)

Evidence: USB logs of 2026-11-17 00:21–00:47 (boots 1755–1758), the platform's stored record for the unit (boot 1762 at 17:11 IST), and the code.

| Test | Result | Notes |
|---|---|---|
| [FW] Cold boot to online within budget | pass (43.7 s, no budget set) | Budget still to be agreed. ~24 s of it is flash-log scans. |
| [E2E] Hello once per boot with model, hw, fw, channels, bootId, resetReason | pass | Platform stored model b1-v4-c, hw B1-V4, fw 1.0.4 build 2026-11-17-command-reliability, all 21 channels, no mismatches. |
| [FW] Reset reason correct per type | **pass on 1.0.5** (fail on 1.0.4) | See F1, F2 and the retest below. |
| [FW] bootCount increments and survives power loss | **pass** | Increments (1755…1762). It continued across the 17:11 power-up, but needs a known full power-off to confirm VBAT keeps it. See F3. |
| [E2E] Mismatching hello blocks config deploy, UI explains | pass (code + tests) | 409 with the mismatch list; the deploy panel shows the message. The device page never shows mismatches before a deploy is tried (F5). |
| [FW] IMEI read from the modem | pass | `AT+CGSN`, digits only, ≥14 chars; nothing hard-coded. |
| [HIL] Boot with no SIM / antenna / Modbus / secondary MCU | not run | Needs hardware changes on the bench. |

### Findings

- **F1 Power-on is reported as BROWNOUT.** On STM32F4 a power-on sets PORRSTF *and* BORRSTF, and `reset_reason_from_csr` checks BOR before POR. The 17:11 power-up was stored as `resetReason: BROWNOUT`. Fix: POR wins; BROWNOUT only when BOR is set without POR.
- **F2 Software resets are not told apart.** OTA install, config-apply reset, fault reset (HardFault etc.) and a debugger reset all report `SOFTWARE`. The fault record (BKP3R…) and the OTA state are already known at boot. Fix: report `OTA`, `CONFIG_APPLY`, `FAULT_<name>`, `WATCHDOG` (with stuck task, already in BKP16/17) when known.
- **F3 bootId is not unique.** It is `<bootCount>-<HAL_GetTick() at that point>`, and the tick is the same every boot (`…-00001C20`, `…-00001C1B`). If the backup domain is ever lost, bootCount restarts and bootIds repeat, so the platform misses the restart (it detects restarts by bootId change). Fix: add the MCU unique ID hash plus a random/RTC part, e.g. `<bootCount>-<RTC seconds hex>`.
- **F4 Backup register overlap.** `RTC_BKP_DR3/DR4` hold the code-time stamp and are also the fault magic/code (`BKP3R/BKP4R`). Harmless today because code-time set is off by default, but either feature can corrupt the other. Fix: move one of them.
- **F5 UI does not show boot or identity info.** Last boot time, reset reason, boot count and identity mismatches are stored but no screen shows them. (Backend/UI change, not firmware.)
### Fixes (firmware 1.0.5, build 2026-11-17-boot-identity)

- F1: power-on now wins over brown-out; PIN_RESET only when no other flag is set.
- F2: the firmware writes a reset intent to backup register DR15 before resetting itself (OTA install, OTA rollback, modem-recovery MCU reset, fault handler). The next boot reads and clears it, but only uses it when the reset was a software reset. Reasons: `POWER_ON`, `BROWNOUT`, `PIN_RESET`, `WATCHDOG`, `WINDOW_WATCHDOG`, `LOW_POWER`, `SOFTWARE`, `OTA_INSTALL`, `OTA_ROLLBACK`, `MODEM_RECOVERY`, `FAULT_<name>`. A config apply does not reset the unit, so it has no reason of its own.
- F3: `bootId` = `<bootCount>-<32 bits from the hardware RNG>`; it falls back to unique ID + tick if the RNG fails.
- F4: code-time stamp moved from DR3/DR4 to DR18/DR19. Map: DR1 RTC magic, DR2 boot count, DR3–13 fault record, DR14 boot stage, DR15 reset intent, DR16/17 watchdog task, DR18/19 code time.
- F5: admin device page "Boot & Identity" card (backend branch `feature/admin-boot-info`).

### Retest on 1.0.5 (2026-11-17 19:28–19:43 IST, platform record via local API)

| Reset | Reported | bootId |
|---|---|---|
| SWD flash / debugger reset | `SOFTWARE` ✅ | `1763-14ECA860` |
| Simulated fault (intent `FAULT\|HARDFAULT` written over SWD, then reset) | `FAULT_HARDFAULT` ✅ | `1769-E07E031B` |
| Simulated OTA install (intent `OTA_INSTALL` over SWD, then reset) | `OTA_INSTALL` ✅ | `1770-48EFA5D7` |
| Power cycle (owner, 19:58) | `POWER_ON` ✅ (CSR 0x0E000003: POR+BOR+PIN) | `1771-147C9CC1` |
| Watchdog | not run (section 20) | |

bootCount went 1770 → 1771 across a real power-on reset (PORRSTF set), so the backup domain keeps it: **bootCount survives power loss ✅**.
Admin device page "Boot & Identity" card checked on the local admin app: firmware 1.0.5 and build, b1-v4-c · B1-V4, last boot, POWER_ON, boot count 1771, boot id, uptime, hello time, "Unit matches its registration".

**Section 1 status:** done, except the boot-without-SIM/antenna/Modbus/secondary-MCU test (needs hardware changes) and a real watchdog reset (section 20). No boot-time budget is set yet; boot to online is ~44 s.

Also seen: the USB log loses the first boot lines (`[RST] …`) now that the USB grace is 1 s; the boot reason is still in `[BOOT_LOG]` and on the platform.

- **Seen in passing (section 11):** the local API logs "Telemetry sequence gap detected" on every message with previousSeq = receivedSeq: the local and production API both process each message. The message is wrong (a duplicate, not a gap), and whether the message is stored twice needs checking in section 11.

## 2. Onboarding and provisioning (2026-11-17)

All [SW] items were run against the real services on an in-memory database (`packages/device-core/src/onboarding.flow.test.ts` on backend branch `feature/admin-boot-info`, plus the existing `deviceService.lifecycle.test.ts`). The bench unit was not touched: the local API writes to the production database, and a decommission cannot be undone today.

| Test | Result | Notes |
|---|---|---|
| [SW] Manufacture → claim → configure → first telemetry, no manual DB edits | **pass** | Admin manufactures with IMEI → claim code → ACTIVE (PLATFORM) → hello matches → first deploy creates the device key and ships it in the config → ack APPLIED. Broker login is shared, so there is no per-unit broker step (see section 23). |
| [E2E] Time from claim to first value on screen | **blocked** | Needs a unit that can be claimed. The only bench unit is ACTIVE in production, and a decommissioned unit can never be claimed again (G1). |
| [SW] Two users claim the same unit at once | **pass** | Single conditional write; the second gets 409 and no second entitlement (existing test). |
| [SW] Decommission, then re-claim: nothing inherited | **fail: not possible** | Decommission works (key refused, no config, no relay commands, telemetry dropped), but there is no way back (G1). |
| [SW] Transfer between workspaces end to end | **fail: not built** | Only a request record is written (G2). |

### Findings

- **G1 A decommissioned unit is lost for good.** It stays attached to the old workspace, a new claim code is only given to unclaimed units, and deleting it then registering the IMEI again is refused (409). A returned, repaired or resold unit cannot be onboarded.
- **G2 Transfer is half-built.** `POST …/devices/:id/transfer` stores a `DeviceTransfer` row and nothing reads it: no accept, no completion, no UI. The target workspace id is not checked (any id, even the same workspace).
- **G3 The decommission prompt is misleading.** It says the unit "stops being used by this workspace — a platform admin can fully remove it". It does not say the step is permanent, and nothing can give the unit back.

## 3. Config downlink (2026-11-17)

Bench unit on 1.0.5, local UI/API, real broker. Crafted configs were published straight to `c2d/<imei>/commands_test` from a bench script (a dummy `apiKey`); after each accepted crafted config the real one was redeployed from the UI.

| Test | Result | Notes |
|---|---|---|
| [E2E] Valid config applied, acked, reported back | **pass** | 5 s → 10 s interval: click → sent 0.2 s → on the unit 0.5 s → applied 5.6 s → APPLIED on the platform 1.4 s (7.8 s total). Health reported the new hash. `configId` is fixed per device, so the hash (in health) is what changes, not the configId. |
| [FW] Config survives power-cycle without redeploy | **pass** | Revision 12 still reported after today's resets and the owner's power cycle. |
| [FW] Invalid config rejected with exact reason, last good stays | **partly** | Rejected with the exact reason: bad JSON (`invalid_json_format`), wrong hash (`hash_mismatch`), bad hash format, missing hash, missing config, missing configId. **Accepted when it should be refused:** 31 points (limit 24) silently cut to 24 and APPLIED; a 100 ms interval silently clamped and APPLIED (C4). |
| [FW] Largest allowed config, no heap failure | **fail** | 6 000 B and 8 000 B applied, but at 8 000 B the heap low-water fell to 1 568 B. 8 192 B (the backend's own maximum) is dropped on the unit (`reader_buffer_small`) with no reply (C5). |
| [HIL] Power loss during config flash write | **not run, fails by design** | One flash sector, erased then written (C2). |
| [E2E] Offline config held, delivered on reconnect, expires after 24 h | pass (backend tests) | `configDeploymentService.test.ts`, 36/36. Not run on hardware. |
| [E2E] Newer deployment overtakes older; late ack doesn't roll back | pass (backend tests) | Same suite. |
| [E2E] Running-config hash differs → drift alert | **pass** | Crafted config → `CONFIG_DRIFT` raised within 2 min; redeploy → resolved. |
| [E2E] Every optional setting changes behaviour | **partly** | Interval does, but not to the set value (C8). Sample interval, event cap, debounce and counter width cannot be set in the UI (wire defaults 50 ms / 8); the event cap is ignored by firmware since 1.0.4. |
| [SW] UI shows a diff before deploy; saved vs deployed vs applied clear | **partly** | Deploy panel says "changes saved after the last applied deployment are not on the device yet" and the last applied time, but not what changed (the backend already has a diff, `diffDeployments`) (C9). |

### Findings

- **C1 (critical) Some config sizes can never be applied.** A valid config whose envelope is 3 087–3 092 bytes is rejected with `hash_mismatch` every time; 3 080, 3 084, 3 096, 6 000 and 8 000 bytes apply. The unit receives the right length, so bytes are changed on the way in, depending on where they fall. Repeatable. Root cause not found yet; the same receive path carries every downlink (relay, OTA, config). Next step: a diagnostic build that hashes the received payload in 256-byte blocks so the bad block can be compared with what was sent.
- **C2 (high) Config is lost on power failure during a save.** One flash sector (sector 7) is erased, then written. Power loss or a write error in between leaves no valid config: the unit boots with defaults (no points, no device key) until redeployed. Fix: two sectors (or two W25Q slots) with a sequence number; keep the old one until the new one verifies. The 128 KB erase also stalls the CPU ~1–2 s.
- **C3 (high) The app may grow into the config sector.** The linker gives the app 480 KB from 0x08008000, which includes the config sector at 0x08060000. The image is ~251 KB today (ends near 0x08046C68), so ~100 KB of headroom, but nothing stops a larger build (or OTA image) from overlapping it. Fix: limit the app region to 0x08060000 in the linker script and the OTA max image size.
- **C4 Over-limit values are silently changed, then reported APPLIED.** Too many points are cut, an out-of-range interval is clamped. Fix: refuse with `too_many_points` / `interval_out_of_range` and keep the last good config.
- **C5 Size limits disagree.** Backend allows an envelope up to 8 192 B; the unit drops ≥ ~8 190 B with no reply, so the deployment times out with no reason, and at 8 000 B the heap is nearly exhausted. Fix: backend limit ~6 KB, and the unit replies `too_large`.
- **C6 Error acks are matched by configId only.** The unit sends the deploymentId, but the platform ignores it for error acks and takes the latest SENT/PENDING one. Use the deploymentId.
- **C7 OTA rollback also restores the old config.** The bootloader backs up and restores the whole 480 KB region, config sector included; a config deployed during an OTA trial is undone (the drift alert would flag it).
- **C8 Telemetry comes every interval + publish time.** Set 5 s → every 10–14 s; set 10 s → every 16–19 s. The next send is timed from the end of the previous publish. (Belongs to section 11.)
- **C9 UI gaps.** No diff before deploy; sample interval, debounce, counter width and event cap not editable.
- **C10 Telemetry cannot say which config produced it.** It carries the fixed configId, not the hash or revision.

The bench unit is back on the real config (deployment `84db05ec…`, APPLIED and reported). Its telemetry interval is now 10 s (was 5 s).

### Section 3 fixes (firmware 1.0.6 + bootloader, backend branch `feature/admin-boot-info`), retested 2026-11-17 21:15–21:40 IST

Root cause of C1: **not the radio link.** The board received every byte correctly (block hashes matched the sender except the last block); `Core/Src/sha256.c` did not add the extra padding block when 56–63 bytes were left over, so every input whose length mod 64 is 56–63 hashed wrongly (136 of 1 101 lengths in a host sweep). The bootloader compiles the same file, so OTA images of those sizes would also have failed. Fixed, with `scripts/sha256_host_test.c` (NIST vectors; the old code fails the 56-byte one).

| Finding | Fix | Retest |
|---|---|---|
| C1 sizes refused | SHA-256 padding | 3 087–3 092 B all APPLIED ✅ |
| C2 config lost on power cut | Two copies (A/B) in W25Q at 0x060000/0x064000 with sequence + CRC, header written last; internal sector 7 read once to migrate; one lock for all W25Q users | migrated to slot A seq 1; saves alternate A/B with rising seq ✅; SWD reset between "parsed" and "saved" → boot "Load from FLASH: VALID" ✅ |
| C3 app may overwrite config sector | Linker app region 352 KB (ends 0x08060000); OTA max image 352 KB in app and bootloader (manifest `app_max_size` unchanged for old bootloaders) | builds at 321 KB ✅ |
| C4 over-limit silently changed | Refused with reason | 31 points → `too_many_points`, 100 ms → `interval_out_of_range`, unknown kind → `unknown_point_kind` ✅ |
| C5 limits disagree | Platform limit 6 144 B; board answers `too_large` with the configId | 8 192 B and 11 000 B → `too_large` ✅ |
| C6 error acks by configId | Platform uses the deploymentId when valid (imei-checked) | backend tests ✅ |
| C7 rollback restores old config | Config no longer in the region the bootloader backs up | by design ✅ |
| C8 period = interval + publish | Wait counted from cycle start | 10 s setting → 9.9–10.4 s ✅ (was 16–19 s) |
| C9 UI gaps | Deploy panel lists what will change; edit page: input sampling, debounce, counter width | Seen in the UI: panel listed `sampleIntervalMs 50 → 100` and `DI_1 debounceMs (none) → 20`; deployed, board applied 100 ms sampling ✅ |
| C10 telemetry can't name its config | `configHash` (16 hex) in telemetry, stored by the platform | seen in telemetry ✅; platform storage covered by tests |

Also learned: the bootloader must be built with `OTA_SIGNING_PUBLIC_KEY_HEX` set (16 KB with the Ed25519 code); built without it, every OTA install is refused. The bench bootloader was rebuilt with the key derived from the local signing key. The earlier missing USB log lines were because the owner also had the port open.

## 4. Downlink speed and priority (2026-11-17, firmware 1.0.6)

Method: commands sent through the real API from the signed-in local UI (alternating RO_1/RO_2, each relay toggling); "relay switched" is the board's `[ACT] acknowledgement queued actuationId=…` line minus 30 ms (it is logged 30 ms after the GPIO is driven); the API and the USB log share this Mac's clock. The actuation list API returns only the newest 100 commands, so platform status is known for the last 100 of a run; board execution is checked for every command.

| Test | Result | Notes |
|---|---|---|
| [E2E] Click → physical action p50/p95/p99 | **pass** | 10 s interval, 40 cmds: 0.52 / 0.77 / 0.84 s (max 0.86). Soak at 5 s, 496 cmds: 0.47 / 0.73 / 1.07 s (max 2.23). Click → ACKED: 1.87–1.89 / 2.11–2.56 / 2.29–2.84 s. The relay usually switches before the API call has even returned (0.44–0.49 s). |
| [E2E] Latency at 5 s / 60 s / 1 h interval | partly | 5 s and 10 s same (commands run on arrival, independent of telemetry). 60 s / 1 h not run yet. |
| [E2E] Zero missed commands with traffic, 500+ | **partly** | 496 sent, **496/496 executed on the board, none lost**; last 100: 93 ACKED, 7 SUPERSEDED (ack took > 2.6 s, the next command for that relay had already come). **But 4 more were refused by the platform as "Device is offline" (22:00)** — see E1/E2. |
| [FW] Downlink beats pending uplink | **pass, too well** | Commands run on arrival even mid-publish. But see E1: a steady stream of commands starves telemetry. |
| [E2E] Burst of 20 in 2 s, order kept, all acked | not run yet | Needs direct MQTT (the API allows one command per relay per second); script ready (`burst.mjs`). |
| [E2E] Command during config flash write | earlier pass (1.0.4, B6) | Config save now ~1.2 s on W25Q; to rerun on 1.0.6. |
| [E2E] Command during reconnect | known gap | Clean MQTT session: commands sent before the unit resubscribes are dropped by the broker (seen today at 21:26). Parked earlier (persistent session). |
| [E2E] Command during Modbus poll | not run | No Modbus slave on the bench. |
| [E2E] Command during OTA download | **fail (code review)** | The OTA download runs inside the task that executes downlinks; relay commands arriving meanwhile are queued, not run, and lost if the OTA then resets the unit to install; the platform shows them TIMED_OUT 2 min later. The platform does not refuse commands while an OTA is in progress. |

### Findings

- **E1 (high) A steady stream of relay commands starves telemetry and health.** Every publish, priority acks included, resets the timer that the telemetry path waits on for its 3 s gap (`mqtt_handler.c` `s_last_mqtt_publish_attempt_tick`); with an ack every ~1.2 s the gap never came. Board log: `cycle work 409029 ms` — no telemetry and no health for ~7 min during the soak. Fix: cap the wait (telemetry goes out at most one gap after it is due), or keep acks from pushing back normal publishes.
- **E2 The platform marked the unit offline while it was acking commands.** Presence comes only from telemetry/health/hello; with a 5 s interval the grace is 10 s, so E1 made it refuse 4 commands as "Device is offline". Fix: any inbound message (acks included) counts as presence.
- **E3 Commands during an OTA are silently lost.** Fix: platform refuses relay commands while the device has an OTA in a non-terminal state (409 `OTA_IN_PROGRESS`, UI shows why); optionally the unit answers FAILED `ota_in_progress`.
- **E4 The actuation list API returns only the newest 100**, with no paging; history of a busy relay cannot be read back (also limits section 6 "relay history").
- **E5 SUPERSEDED hides a successful command.** 7 commands executed and were acked, but the platform shows SUPERSEDED because the next command arrived first; the ack is ignored. Accurate in intent, but the history then never shows that they ran.

### Section 4 fixes (firmware 1.0.7, backend `feature/admin-boot-info`), retested 2026-11-17 22:21–22:44 IST

| Finding | Fix | Retest |
|---|---|---|
| E1 telemetry starved | Normal publishes keep 3 s from the previous *normal* publish and only 500 ms from an ack; telemetry waits at most 3 s for its slot; **the ack flush sends only the acks queued when it starts** (the real cause: `while (count > 0)` never returned while commands kept arriving); ack outbox 8 → 24 | 20 cmd/min for 5 min: telemetry median 9.2 s, max 15 s at a 5 s setting (was none for 7 min); health continued |
| E2 offline while acking | Platform: any well-formed message marks the unit seen (`markSeen`, ≤ every 2 s) | 0 offline refusals in 400 commands |
| E3 commands lost during OTA | Platform: 409 `OTA_IN_PROGRESS` while OTA is downloading/verifying/installing/rolling back (stale after 30 min) | backend tests (no OTA run on the bench yet) |
| E4 history capped at 100 | `limit` and `before` paging on the actuation list (still an array) | all 300 commands of a run read back |
| E5 late acks discarded | Kept as `lateAck` on the superseded command | 13 late acks recorded in the 20/min run |

| Run (1.0.7) | Executed on board | Acked | Click → relay p50 / p95 / p99 | Click → ack p50 / p95 |
|---|---|---|---|---|
| 100 cmds, 1 every 3 s, 5 s telemetry | 100/100 | 100/100 (87 ACKED + 13 late) | 0.55 / 0.94 / 1.25 s | 3.9 / 6.9 s |
| 20 cmds, 1 every 2 s, **1 h** telemetry | 20/20 | 20/20 | 0.68 / 1.05 / 1.35 s | 2.2 / 3.0 s |
| Burst: 20 in 2 s on one relay (direct MQTT) | 20/20, in order, final state = last command | 20/20, in order, last after ~25 s | — | — |
| Config deploy + 6 commands at once | 6/6 | 6/6 | the command arriving mid-save waited ~2 s for the save | config APPLIED 7.5 s |
| 300 cmds, 1 every 1.2 s (overload) | all that reached it | ack queue overflowed (8 slots then; 24 now) | — | ~9.5 s |

**Capacity note:** each ack costs ~1.15 s of modem time (four AT steps), so the unit can confirm at most ~0.85 commands/s while also sending telemetry. Beyond that, commands still run on arrival but acks queue up and the oldest are dropped when 24 are waiting; the relay state in telemetry still corrects the UI. Faster acks would need a shorter publish sequence or several acks per message (a contract change).

**Flashing note:** SWD writes started failing ("failed to erase memory", later a verify mismatch). Cause: the watchdog firing during the longer multi-sector erase. Fix: write `0x1800` to `DBGMCU_APB1_FZ` (`0xE0042008`) in the same CLI call before `-w`; one verify mismatch also needed `freq=950`.

## 5. Relays, software → hardware (2026-11-17/18)

Pins read over SWD (GPIOB ODR: RO_1 = PB15, RO_2 = PB14); commands sent direct over MQTT unless noted.

| Test | Result |
|---|---|
| ON/OFF/TOGGLE each relay, pin matches | **pass** (pins follow every acked command; contact/feedback not checked: feedback hardware parked) |
| Same actuationId twice | **pass**: redelivered ON after an OFF was re-acked, not re-run; duplicate TOGGLE flipped once |
| Expired command | **pass**: EXPIRED `command_expired`, pin unchanged |
| Unknown port / bad action | **fail**: both FAILED `relay_action_failed` (not readable); missing fields → `missing_port_or_action` ✅ |
| Two users at once (API, same relay, opposite actions) | **pass**: one SENT, the other 429; platform state OFF = pin OFF |
| Not run yet | offline refusal on hardware, boot/watchdog/connection-loss state, chatter |

Findings so far:
- **R1** unknown port and unsupported action share the generic reason `relay_action_failed`; should be `unknown_port` / `unsupported_action`.
- **R2** duplicate protection remembers only the last 8 ids and forgets them at reboot.
- **R3** `safeState` is stored on the platform but never sent to the unit; relays always start OFF and hold their state on connection loss.
- **R4** no chatter protection on the unit: 20 switches in 2 s all executed (direct MQTT); only the platform's 1 command/port/s limit protects the relay.
- **R5** ACKED means "pin driven", not "contact closed" (feedback hardware parked).
- **R6** a unit that has just died or is rebooting stays ONLINE for 5–10 min (offline sweep every 5 min, 5 min floor), so commands sent then are accepted and lost (seen at 21:26 and during the soak).

### Section 5 fixes (firmware 1.0.8, backend `feature/admin-boot-info`), retested 2026-11-18

| Finding | Fix | Retest |
|---|---|---|
| R1 unreadable refusals | `unknown_port`, `unsupported_action` | RO_9 → `unknown_port`, BLINK → `unsupported_action` ✅ |
| R2 8 remembered ids | 32 (commands also expire after 60 s) | — |
| R3 safe state never sent | Config carries `safeState` (when ON) and `relayLinkLossSafeMs`; unit applies safe states at boot and after that long offline; stored in spare config bits (layout unchanged); UI "Safe state" + "If the connection is lost" | RO_2 safe ON + 1 min: reset → `RO_2 safe state ON (boot)`, pin high ✅. Connection loss: a wrap bug (`tick \| 1` start time) made it fire at boot — fixed and re-verified; **physical antenna-off test still to do** |
| R4 no chatter guard | 500 ms minimum between changes of one relay (`min_switch_interval`); same-state commands accepted | ON then OFF 200 ms later → refused; burst 20 in 2 s → 12 accepted, 8 refused ✅ |
| R5 contact not checked | parked (feedback hardware) | — |
| R6 dead unit looks online | Idle keepalive 30 s (was 240 s), `hello.presenceMs`; platform refuses when silent > 3 × min(interval, presenceMs), ≥ 20 s | command 27 s into a reboot → 409 "not heard from recently" ✅; hello carries `presenceMs:30000` ✅ |

Tests already passing on 1.0.7 (pins follow ON/OFF/TOGGLE, duplicate id re-acked not re-run, expired command not run, missing fields reason, two users at once) were not affected.

Still open in section 4: commands during reconnect (parked: clean session; seen again when the soak started while the unit was still rebooting), Modbus poll (no slave on the bench), OTA refusal on hardware (section 22).
