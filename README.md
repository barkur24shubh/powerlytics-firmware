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

## Build

Build from the `Debug` directory using STM32CubeIDE generated makefiles:

```sh
make all
```

For OTA, prefer attaching signed release binaries to GitHub Releases, or force-add
a specific sanitized artifact after review.
