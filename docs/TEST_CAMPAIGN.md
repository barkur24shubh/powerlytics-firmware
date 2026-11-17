# Test campaign: B1-V4-C (IMEI 863110085107293)

One section at a time: test, record, agree fixes with the owner, fix, retest.
Tags: [FW] device only · [HIL] needs physical stimulus · [E2E] device to UI · [SW] backend/UI only.

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
