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

## Build

Build from the `Debug` directory using STM32CubeIDE generated makefiles:

```sh
make all
```

For OTA, prefer attaching signed release binaries to GitHub Releases, or force-add
a specific sanitized artifact after review.
