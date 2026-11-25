# Device identity (firmware 1.0.20)

Platform side: `docs/ids.md` and `docs/program/device-identity-proposal.md` in `powerlytics-rebuild-actuation` (decision D-023).

## One id

A unit has one identity, its **deviceId**: the platform's own id for it (the `Device._id`), 24 lowercase hex characters. It is the
second segment of every MQTT topic, the MQTT client id and `device_id` in the configuration. The modem IMEI and the MCU unique ID
are **attributes**, reported in `hello` as `hardwareIds {imei, mcuUid}`; they are not the identity once a deviceId is known.

## Where the unit gets it (first match wins, `app_id_choose`)

1. the **identity record** written at manufacture
2. the **`device_id` of the configuration** the platform deployed
3. nothing: a cellular unit uses its **IMEI** for first contact, as every firmware before 1.0.20 did

A unit that learns its deviceId from its first configuration (case 3 to 2) **restarts once**, 12 s after the configuration is applied
so the deployment acknowledgement leaves on the old topic first. The reset reason is `IDENTITY_CHANGE`. After that its topics are
`d2c/<deviceId>/…` and the platform answers on `c2d/<deviceId>/commands`.

## The record and the USB commands

Two 4 KB slots in the free part of the external flash: `0x06C000` and `0x06D000` (configuration slots are `0x060000` and `0x064000`, the
OTA backup starts at `0x080000`). Each holds `{magic "IDEN", version, seq, deviceId, crc32}`; the newest valid copy wins, so a power cut
during a write keeps the previous one. It is separate from the configuration, so a deploy, a configuration error or an update never
wipes it.

On the USB debug port (115200 8N1, one command per line):

| Command | Does |
|---|---|
| `ID` | prints the id in use, its source, the record, the config id, the IMEI and the MCU unique ID |
| `ID SET <24 hex>` | writes the record (refused if it is not a deviceId, or a different record exists) |
| `ID CLEAR` | erases both slots |

## What it does not do

- It does not authenticate: the id is a name. The broker still uses one shared login; per-unit credentials and the topic ACLs
  (`d2c/${clientid}/#`) are the next phase (FH-003). The client id now equals the deviceId, which is what that rule needs.
- The connectivity manager still waits for a modem IMEI before it prepares topics, so a unit with **no modem** needs its own connectivity
  path before it can use the record. The identity code itself does not need an IMEI.
- No bootstrap for devices that are not made here (a customer's ESP32): they would claim a deviceId with their hardware id and a claim code.

## Tests

`scripts/identity_host_test.c` (25 checks): the shape of a deviceId, the order of the sources, the MCU unique ID format, the record
(crc, magic, version, erased flash, a record holding a non-id). Build and run:

```sh
cc -std=c11 -Wall -Wextra -ICore/Inc -o /tmp/identity_host_test scripts/identity_host_test.c Core/Src/app_identity_core.c && /tmp/identity_host_test
```

## Size

Flash +3.8 KB, RAM +0.3 KB over 1.0.19.

## Bench

Not yet verified on the bench unit: the ST-Link dropped off USB during the flash. After a replug, flash with `scripts/flash_board.sh` or
`STM32_Programmer_CLI`, then check the boot log for `[ID] topics use <id> (source=…)` and try `ID`, `ID SET`, `ID CLEAR`. **Deploy the platform
change first**: a production platform that predates it does not recognise deviceId topics.
