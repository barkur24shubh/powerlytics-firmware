# Powerlytics Embedded Firmware

Production STM32F407 IoT gateway firmware.

## Project

- MCU: STM32F407VETx
- IDE: STM32CubeIDE
- Main project file: `BALCKPILL_BASED_GENERIC_GATEWAY.ioc`
- Firmware artifacts: `artifacts/`

## Firmware Artifacts

- `artifacts/BALCKPILL_BASED_GENERIC_GATEWAY.bin`
- `artifacts/BALCKPILL_BASED_GENERIC_GATEWAY.hex`
- `artifacts/BALCKPILL_BASED_GENERIC_GATEWAY.elf`
- `artifacts/BALCKPILL_BASED_GENERIC_GATEWAY.map`
- `artifacts/SHA256SUMS`

## Build

Build from the `Debug` directory using STM32CubeIDE generated makefiles:

```sh
make all
```

The committed artifacts are from the stabilized build prepared before OTA development.
