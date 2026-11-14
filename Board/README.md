# Powerlytics Board Profiles

Powerlytics firmware keeps the cloud/device protocol common and selects
hardware details through a board profile.

The active board defaults to `b1-beta-c` in
`Core/Inc/board_profile.h`. Future builds can override it with:

```sh
./scripts/build_board.sh b1-beta-c
```

`scripts/build_board.sh` writes the ignored local selector
`Core/Inc/board_active_config.h`, clean-builds the firmware, and packages
board-specific outputs under `artifacts/boards/<board-slug>/`.

Planned B1 SKUs:

| SKU | AI | DI | Relay | RS485 | Connectivity | Status |
| --- | --: | --: | --: | --: | --- | --- |
| `B1-BETA-C` | 10 | 8 | 5 | 1 | Cellular | Implemented current beta board |
| `B1-V1-C` | 0 | 0 | 2 | 1 | Cellular | Registered, profile pending |
| `B1-V2-C` | 2 | 2 | 0 | 1 | Cellular | Registered, profile pending |
| `B1-V3-C` | 4 | 4 | 2 | 1 | Cellular | Registered, profile pending |
| `B1-V4-C` | 10 | 6 | 2 | 1 | Cellular | Implemented BV4C board |
| `B1-V1-E` | 0 | 0 | 2 | 1 | Ethernet + Wi-Fi | Registered, profile pending |
| `B1-V2-E` | 2 | 2 | 0 | 1 | Ethernet + Wi-Fi | Registered, profile pending |
| `B1-V3-E` | 4 | 4 | 2 | 1 | Ethernet + Wi-Fi | Registered, profile pending |
| `B1-V4-E` | 10 | 8 | 2 | 1 | Ethernet + Wi-Fi | Registered, profile pending |

Build and flash examples:

```sh
./scripts/build_board.sh b1-beta-c
./scripts/flash_board.sh b1-beta-c
```

The older `./scripts/flash_ota_pair.sh` command remains available as an alias
for `./scripts/flash_board.sh b1-beta-c`.

Board profiles own:

- product, variant, and hardware revision identity
- connectivity capabilities: cellular, Wi-Fi, Ethernet
- relay, digital input, analog input, and counter counts
- GPIO, ADC, UART, SPI, SDIO, flash-CS, RS485-DE, and LED mappings

Each board must define `BOARD_CONNECTIVITY_PRIMARY` to select the active
firmware backend:

```c
#define BOARD_CONNECTIVITY_PRIMARY BOARD_CONNECTIVITY_CELLULAR
```

The generic connectivity layer in `Core/Inc/connectivity.h` is the only API the
application layer should call. Board-specific implementations live behind that
interface, for example cellular today and Ethernet/Wi-Fi later.

Application code should use logical names such as `AI_1`, `DI_1`, `RO_1`, and
common telemetry/config/event schemas. It should not depend on MCU pins such as
`PA0` or `GPIO_PIN_3` directly.
