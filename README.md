# Powerlytics Embedded Firmware

Production STM32F407 IoT gateway firmware.

## Project

- MCU: STM32F407VETx
- IDE: STM32CubeIDE
- Main project file: `BALCKPILL_BASED_GENERIC_GATEWAY.ioc`
- Firmware artifacts: `artifacts/`

## Local Secrets

Production MQTT credentials are not committed to Git.

To build firmware that can connect to the live broker, copy:

```sh
cp Core/Inc/config.example.h Core/Inc/config.local.h
```

Then edit `Core/Inc/config.local.h` with the real APN/MQTT values. That file is ignored by Git.

## Firmware Artifacts

Compiled artifacts are intentionally not committed by default because firmware
binaries can contain embedded credentials. For release/OTA builds, generate
fresh artifacts from a clean configuration and publish them deliberately.

The artifacts currently committed under `artifacts/` were generated deliberately
from the local production config for deployment/OTA preparation.

Before publishing a new OTA version:

```sh
./scripts/set_firmware_version.sh 1.0.12 2026-08-06-rtc-build-time-boot-status
TZ=UTC ./scripts/flash_ota_pair.sh
```

`Core/Inc/app_firmware.h` is the embedded firmware identity source. The GitHub
Action validates that the uploaded OTA binary actually contains the workflow
firmware version before it publishes to GCS, so a stale binary cannot be
accidentally labelled as a newer release.

## OTA Bootloader

The device now uses a small first-stage bootloader:

- Bootloader flash: `0x08000000` to `0x08007FFF`
- Main application flash: starts at `0x08008000`
- OTA staging in W25Q flash: starts at `0x080000`
- OTA manifest in W25Q flash: starts at `0x000000`

For first provisioning, flash both images in this order:

```sh
STM32_Programmer_CLI -c port=SWD -w artifacts/bootloader/powerlytics_bootloader.elf -v
STM32_Programmer_CLI -c port=SWD -w artifacts/app/BALCKPILL_BASED_GENERIC_GATEWAY.elf -v -rst
```

For local development, use the helper so the bootloader and relocated app are
always built and flashed together:

```sh
./scripts/flash_ota_pair.sh
```

OTA firmware binaries must be generated from this project after the linker
change, because the app is linked to run from `0x08008000`.

## Recent Firmware Changes

Latest main-branch update:

- Firmware `1.0.12` removes GSM network-time reads from log upload and uses
  RTC build-time sync only. Boot logs now include `time_preserved` and report
  RTC status from the device RTC instead of hardcoding it.
- Firmware `1.0.11` adds a separate `GSM_TIME_VALIDATED` cloud log after
  cellular network/PDP readiness. It includes `gsmTs` only when SIM7600C
  `AT+CCLK?` returns a validated network UTC timestamp.
- Firmware `1.0.10` filters invalid SIM7600C default clock values such as
  `70/01/01`, so `gsmTs` reports `unknown` until real network time is available.
- Added a SIM7600C `AT+CCLK?` diagnostic read and included the parsed modem
  network UTC time as `gsmTs` in cloud log JSON. This is diagnostic-only and
  does not update the STM32 RTC yet.
- Released firmware `1.0.8` from the latest stable code for OTA retesting.
- Reduced OTA TCP range size and moved OTA request/header buffers out of the
  runtime task stack to improve modem download stability.
- Increased runtime task stack headroom for OTA download/install operations.
- Disabled raw AT command transcript logging by default. Terminal logs now show
  high-level events such as MQTT connected, subscribed, published, config
  applied, and GPRS connected.
- Removed full JSON/API-key payload prints from terminal debug paths.
- Disabled per-channel ADC value prints by default to keep runtime logs readable.
- Added SD-card status into cloud diagnostic log module status.
- Added embedded firmware version into cloud diagnostic logs and boot-log
  message text for easier remote traceability.
- Simplified SD-card error logs to timestamp, severity, and clear diagnostic
  message only.
- Filtered noisy transport/debug lines from SD-card logs, including AT traces,
  raw modem echoes, ADC dumps, and repeated flash-upload plumbing logs.
- Built/flashed using UTC build time so RTC boot sync writes UTC timestamps when
  the device RTC is older than the firmware build time.

## Build

Build from the `Debug` directory using STM32CubeIDE generated makefiles:

```sh
make all
```

For OTA, prefer attaching signed release binaries to GitHub Releases, or force-add
a specific sanitized artifact after review.
