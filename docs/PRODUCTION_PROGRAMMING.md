# Production programming (B1-V4-C, STM32F405)

Written 2026-11-19 (test campaign section 27). Every unit that leaves for a customer is programmed and locked this way. Bench and development boards stay unlocked.

## Why

A bench board as delivered has every protection off:

| Setting | Bench (measured) | Risk if shipped like this |
|---|---|---|
| Readout protection (RDP) | Level 0 | Anyone with an ST-Link reads the whole flash: firmware, broker host, the shared MQTT login compiled in, the OTA public key |
| Brown-out reset (BOR) | Off (resets only at 1.8–2.1 V) | A sagging supply runs the CPU and flash below spec (2.7 V): corrupted config, logs or OTA image instead of a clean reset |
| Bootloader write protection | Off | A firmware bug or bad image can erase sectors 0–1 and brick the unit beyond OTA repair |
| Watchdog | Started by firmware (software watchdog) | Acceptable: it starts within the first second of boot (section 20) |

## Steps (STM32CubeProgrammer CLI, SWD)

`P` = path to `STM32_Programmer_CLI`.

1. **Firmware.** Use the signed release built by the GitHub workflow from a tagged commit (`artifacts/boards/b1-v4-c/`): bootloader at `0x08000000`, application at `0x08008000`.
   ```
   $P -c port=SWD mode=UR -e all -w bootloader.bin 0x08000000 -w application.bin 0x08008000 -v
   ```
2. **Brown-out reset level 3 (2.7 V) and bootloader write protection (sectors 0–1).**
   ```
   $P -c port=SWD mode=UR -ob BOR_LEV=0 nWRP0=0 nWRP1=0
   ```
   (`BOR_LEV`: 0 = level 3, 2.70 V; 3 = off. `nWRPn = 0` protects sector n.)
3. **Readout protection Level 1** (last, after everything is written and verified):
   ```
   $P -c port=SWD mode=UR -ob RDP=0xBB
   ```
   **Never Level 2 (`0xCC`)**: it is permanent, disables SWD for good and removes any way to recover a unit except OTA.
4. **Check.**
   ```
   $P -c port=SWD mode=HOTPLUG -ob displ
   ```
   Expect `RDP 0xBB (Level 1)`, `BOR_LEV 0x0`, `WRP0`/`WRP1` active. Then power the unit with SIM and antenna and check the admin device page, Boot & Identity → Security: **TLS verified, RDP 1, BOR on** (reported in the hello since firmware 1.0.13).
5. **Record** serial number, IMEI, firmware version and the check result in the production log.

## Recovering a locked unit (returns, repairs)

Setting `RDP=0xAA` drops back to Level 0 and **mass-erases the whole flash** (firmware and config). Write protection must be removed first (`nWRP0=1 nWRP1=1`). Then reprogram from step 1. The unit's identity (IMEI) is in the modem, its registration on the platform; nothing unit-specific is lost except the local config, which the platform re-sends on the next hello.

## Notes

- USB debug output stays on in production: it is how a unit is diagnosed on site. Since firmware 1.0.18 it no longer shows the broker login (it was printed in clear in the connect command); payloads, API keys and configs were already hidden.
- The shared broker login was readable from every unit's USB port before 1.0.18 and from flash on any unlocked unit: rotate it when per-unit broker logins (parked G4) arrive.
- Release images are built from the Debug configuration (`-O0 -g3`). A size-optimised release configuration would be smaller and faster, but changes timing everywhere and needs a full regression run first (open).
