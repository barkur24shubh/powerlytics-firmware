# Device identity (firmware 1.0.21)

One id names a unit from the day it is programmed to the end of its life: the **deviceId**, 24 lowercase hex characters.

- **Created** by the platform when the unit is registered (the admin shows it).
- **Written** into the unit's flash once, when it is programmed: `ID SET <deviceId>` over the USB debug port.
- **Stored** in two 4 KB slots of the external W25Q at `0x06C000` / `0x06D000` (record: magic, version, seq, id, CRC32). They are outside the app image, the configuration slots and the OTA backup, so OTA updates and new firmware keep it.
- **Permanent:** a second `ID SET` with a different id is refused and there is no clear command. Re-provisioning a unit means erasing the external flash chip.
- **Used for:** the second segment of every MQTT topic (`d2c/<deviceId>/…`, `c2d/<deviceId>/…`) and the MQTT client id.

Nothing else is an identity. The modem IMEI, the MCU unique ID and the MAC are attributes reported in `hello` (`hardwareIds`) and never name the unit. A deployed configuration's `device_id` is only checked: a different one is logged and ignored.

**A unit with no stored deviceId does not connect.** It logs `[ID] NOT PROVISIONED`, stays off the network and still runs its local rules.

USB commands (115200, the CDC debug port): `ID` prints `deviceId=… provisioned=yes|NO mcuUid=…`; `ID SET <deviceId>` writes it once, then restart the unit.

Provisioning a new unit: register it on the platform, copy the Device ID from the admin, flash the firmware, `ID SET <id>`, restart.
