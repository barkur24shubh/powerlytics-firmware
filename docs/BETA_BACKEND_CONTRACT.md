# Powerlytics Beta Backend Contract

For an exhaustive backend config field and variation checklist, see [BETA_CONFIG_VALIDATION_REFERENCE.md](BETA_CONFIG_VALIDATION_REFERENCE.md).

This document defines the backend/device contract for the current Powerlytics beta firmware. It intentionally avoids hardware pin mapping and board layout details. The same contract should remain stable across future Powerlytics board variants; board variants only change which channels exist in firmware.

## 1. Protocol Scope

The device communicates with the backend over MQTT.

The backend sends:

- Configuration downlinks
- Relay actuation commands
- OTA update commands

The device sends:

- Interval telemetry
- Low-latency events
- Config/actuation/OTA acknowledgements
- Runtime health
- Runtime logs

All JSON payloads are compact JSON. Unknown fields should be ignored by the device unless they are part of validation for a required object.

## 2. MQTT Topics

Current beta firmware uses test topics so messages do not reach the production backend handlers.

| Direction | Purpose | Topic |
|---|---|---|
| C2D | Commands, config, OTA, actuation | `c2d/{imei}/commands_test` |
| D2C | Interval telemetry | `d2c/{imei}/telemetry_test` |
| D2C | Low-latency events | `d2c/{imei}/event_test` |
| D2C | Config/actuation/OTA ACKs and keepalive | `d2c/{imei}/message_test` |
| D2C | Runtime health | `d2c/{imei}/health_test` |
| D2C | Runtime logs | `d2c/{imei}/logs_test` |

`{imei}` is the modem IMEI read by the device. The production suffix change should be handled as a firmware/build setting before production rollout.

## 3. Time Format

Device timestamps are ISO-8601 strings using IST offset:

```json
"2026-07-29T01:34:44+05:30"
```

The backend should parse the offset instead of assuming `Z`/UTC. The event and telemetry timestamps are generated from the device RTC.

## 4. Config Downlink

Topic:

```text
c2d/{imei}/commands_test
```

Required outer format:

```json
{
  "type": "config",
  "configId": "test-beta-config-001",
  "hash": "e2501ac1df06d184f9958c943dfef19a5407c336988f47cc496744da29cd21e2",
  "config": {
    "v": 1,
    "device_id": "device-object-id",
    "configId": "test-beta-config-001",
    "imei": "863110085129610",
    "deploymentId": "deployment-001",
    "apiKey": "device-api-key",
    "telemetryInterval": 60000,
    "telemetry": {
      "sampleIntervalMs": 50,
      "sendFullSnapshot": true,
      "eventCapPerInterval": 8
    },
    "points": []
  }
}
```

Validation currently enforced by firmware:

| Field | Required | Notes |
|---|---:|---|
| `type` | yes | Must be `"config"`. |
| outer `configId` | yes | Used for ACK and flash verify. |
| outer `config` | yes | Must be an object. |
| outer `hash` | yes | Exactly 64 lowercase hexadecimal characters. Must equal SHA-256 of the compact inner `config` JSON. Do not put `hash` inside `config`. |
| `config.device_id` | yes | Non-empty. |
| `config.configId` | yes | Must match outer `configId` after save/reload. |
| `config.apiKey` | yes | Non-empty. Device will skip telemetry until valid config/API key exists. |
| `config.imei` | recommended | Used in telemetry payload. |

Config ACKs are published on `d2c/{imei}/message_test`.

Success:

```json
{
  "type": "deployment_ack",
  "configId": "test-beta-config-001",
  "status": "APPLIED"
}
```

Errors:

```json
{
  "status": "error",
  "configId": "test-beta-config-001",
  "type": "parse_to_struct_failed"
}
```

Known error `type` values:

| Error | Meaning |
|---|---|
| `invalid_json_format` | Payload was not valid JSON. |
| `missing_configId` | Outer `configId` missing. |
| `missing_config_object` | Outer `config` object missing. |
| `missing_hash` | Outer `hash` is missing or empty. |
| `invalid_hash_format` | Hash is not exactly 64 lowercase hexadecimal characters. |
| `hash_mismatch` | Hash does not match the compact inner `config` JSON. |
| `config_too_large_for_hash` | Compact inner config exceeds the firmware validation buffer. |
| `legacy_modbusSlaves_not_supported` | Legacy top-level `modbusSlaves` was supplied. Move the slaves under a Modbus entry in `points[]`. |
| `parse_to_struct_failed` | Config schema did not parse into firmware struct. |
| `flash_write_error` | Durable flash save failed. |
| `flash_reload_error` | Saved config could not be loaded. |
| `flash_verify_error` | Reloaded config ID did not match downlink config ID. |
| `apply_verify_error` | Runtime config was not accepted after load. |

### 4.1 Config Hash Calculation

The firmware validates the outer hash before parsing, saving, or applying a config.

Hash rule:

1. Build the complete inner `config` object. It must not contain a `hash` field.
2. Serialize that object as compact UTF-8 JSON with no insignificant whitespace. Preserve object-key order and array order.
3. Calculate SHA-256 over the exact compact JSON bytes.
4. Encode the digest as 64 lowercase hexadecimal characters.
5. Put that result in the outer `hash` field.

Example:

```text
configJson = JSON.stringify(config)
request.hash = lowercase_hex(sha256(utf8(configJson)))
```

Node.js example:

```js
import { createHash } from "node:crypto";

const configJson = JSON.stringify(config);
const hash = createHash("sha256")
  .update(configJson, "utf8")
  .digest("hex");

const downlink = {
  type: "config",
  configId: config.configId,
  hash,
  config,
};
```

Do not hash `type`, outer `configId`, outer `hash`, or any other envelope field. Only the inner `config` object is hashed. The backend must publish the same `config` object whose compact serialization it hashed; changing a value, key order, array order, or number representation after hashing causes rejection.

## 5. Config Fields

### 5.1 Top-Level `config`

| Field | Type | Default | Notes |
|---|---|---|---|
| `v` | number | `1` | Protocol/config version. |
| `device_id` | string | none | Required. |
| `configId` | string | none | Required. Should match outer `configId`. |
| `imei` | string | empty | Device identity used in telemetry body. |
| `deploymentId` | string | empty | Stored for backend traceability. |
| `apiKey` | string | none | Required. |
| `telemetryInterval` | number | `60000` | Milliseconds when `points[]` exists. Valid range: `1000` to `3600000`. |
| `telemetry` | object | see below | Sampling/event options. |
| `points` | array | empty | Logical DI/AI/counter/relay/Modbus point definitions. |

### 5.2 `telemetry`

| Field | Type | Default | Notes |
|---|---|---|---|
| `sampleIntervalMs` | number | `50` | AI sampling interval. Valid range: `20` to `60000`. |
| `sendFullSnapshot` | boolean/string/number | firmware default | Parsed and stored for future behavior. Current beta telemetry sends all enabled board channels. |
| `eventCapPerInterval` | number | `8` | Max events queued per telemetry interval. |
| `maxEventsPerInterval` | number | `8` | Alias for `eventCapPerInterval`. |

### 5.3 `points[]`

`points[]` is the common way to name channels, set scaling, and attach rules. If a board channel is not present in `points[]`, firmware still reports it with a default key such as `DI_1` or `AI_1`.

```json
{
  "key": "AI_1",
  "name": "Line Current",
  "kind": "analog",
  "enabled": true,
  "scale": 1.0,
  "offset": 0.0,
  "unit": "mA",
  "rules": []
}
```

| Field | Type | Required | Notes |
|---|---|---:|---|
| `key` | string | yes | Max 15 chars. Used as telemetry key and event `source`. Supported aliases for channel matching: `AI_1`, `AI1`, `ai1`, `ai_1`, and same pattern for `DI`. |
| `name` | string | no | Human label, max 23 chars. |
| `kind` | string | yes | `digital`, `counter`, `analog`, `relay`, `modbus`. |
| `enabled` | boolean/string/number | no | Defaults true. Disabled points are not emitted. |
| `scale` | number | no | Calibrated value = raw value * `scale` + `offset`. |
| `offset` | number | no | See above. |
| `unit` | string | no | Max 15 chars. |
| `digital` | object | no | Digital/counter options. |
| `rules` | array | no | Max 2 rules per point. |
| `slaves` | array | no | Only used when `kind` is `modbus`. |

Supported `kind` values:

| Value | Meaning |
|---|---|
| `digital` | Boolean input with transitions and on-duration. |
| `counter` | Counter/pulse point. |
| `analog` | Numeric value with interval min/max/avg. |
| `relay` | Relay/control point. |
| `modbus` | Container point for Modbus slave definitions. |

### 5.4 `points[].digital`

```json
{
  "edge": "both",
  "activeHigh": true,
  "counterWidth": 32,
  "rolloverAt": 4294967295
}
```

| Field | Type | Default | Values |
|---|---|---|---|
| `edge` | string | `rising` in parser; DI default emits both if no rules are supplied | `rising`, `falling`, `both` |
| `activeHigh` | boolean/string/number | `true` | Inverts logical value when false. |
| `counterWidth` | number | `32` | `16` or `32`. |
| `rolloverAt` | number | `0xFFFFFFFF` | Used for counter wrap handling. |

For DI events, if no explicit rules are supplied, firmware creates default rising-edge and falling-edge event rules.

### 5.5 `rules[]`

Max 2 rules per point/read.

```json
{
  "type": "threshold_high",
  "enabled": true,
  "severity": "warning",
  "durationMs": 1000,
  "recoveryDurationMs": 1000,
  "high": 80.0,
  "hysteresis": 2.0,
  "latchMode": "none",
  "contextKeys": ["AI_2", "DI_1"]
}
```

| Field | Type | Default | Notes |
|---|---|---|---|
| `type` | string | none | Required for a usable rule. |
| `enabled` | boolean/string/number | `true` | Disabled rules are ignored. |
| `severity` | string | `info` | `info`, `warning`, `critical`. |
| `latchMode` | string | `none` | `none`, `latch_until_normal`, `latch_until_reset`. Parsed; current threshold state machine recovers on rule recovery condition. |
| `debounceMs` | number | `0` | Parsed for rules. DI ISR currently queues edge events based on actual edge detection. |
| `durationMs` | number | `0` | Analog threshold must remain crossed for this duration before event. |
| `recoveryDurationMs` | number | `0` | Analog recovery must remain normal for this duration before `AI_RECOVERY`. |
| `high` | number | `0` | High threshold. |
| `low` | number | `0` | Low threshold. |
| `hysteresis` | number | `0` | Applied around threshold recovery. |
| `contextKeys` | string array | empty | Max 2 keys. Stored for future event enrichment. |

Supported rule `type` values:

| Type | Applies To | Event Type |
|---|---|---|
| `rising_edge` | Digital | `DI_RISING_EDGE` |
| `falling_edge` | Digital | `DI_FALLING_EDGE` |
| `threshold_high` | Analog/Modbus analog | `AI_THRESHOLD_HIGH` |
| `threshold_low` | Analog/Modbus analog | `AI_THRESHOLD_LOW` |
| `out_of_range` | Analog/Modbus analog | `AI_OUT_OF_RANGE` |
| `in_range` | Analog/Modbus analog | `AI_IN_RANGE` |

Analog recovery sends:

```text
AI_RECOVERY
```

## 6. Modbus Config

Modbus configuration must be supplied through a `points[]` entry with `"kind": "modbus"` and a nested `slaves[]` array. Top-level `modbusSlaves[]` is not supported.

Example:

```json
{
  "key": "MI_1",
  "name": "Energy Meter",
  "kind": "modbus",
  "enabled": true,
  "slaves": [
    {
      "unique_slave_id": "slave-1",
      "name": "Main Meter",
      "slaveId": 1,
      "serial": {
        "baudRate": 9600,
        "dataBits": 8,
        "stopBits": 1,
        "parity": "none"
      },
      "polling": {
        "intervalMs": 1000,
        "timeoutMs": 2000,
        "retries": 3
      },
      "reads": [
        {
          "readId": "mi-r1",
          "name": "Voltage",
          "tag": "voltage",
          "kind": "analog",
          "func": 3,
          "start": 0,
          "qty": 2,
          "dataType": "float32",
          "endianness": "ABCD",
          "scale": 1.0,
          "offset": 0.0,
          "unit": "V",
          "rules": []
        }
      ]
    }
  ]
}
```

### 6.1 Slave Fields

| Field | Aliases | Type | Default/Limit |
|---|---|---|---|
| `unique_slave_id` | `uniqueSlaveId`, `uniqueSlaveID` | string | Defaults to numeric slave ID if empty. |
| `name` | | string | Optional. |
| `slave_id` | `slaveId`, `address` | number/string | Required, `1` to `247`. |
| `serial.baudRate` | `serial.baud`, `serial.baud_rate` | number | Default `9600`. |
| `serial.dataBits` | | number | Default `8`. |
| `serial.stopBits` | | number | `1` or `2`; default `1`. |
| `serial.parity` | | string | `none`, `even`, `odd`; default `none`. |
| `polling.intervalMs` | `interval`, `pollIntervalMs` | number | Default `1000`. |
| `polling.timeoutMs` | `timeout`, `timeout_ms` | number | Default `2000`. |
| `polling.retries` | | number | Default `3`. |
| `reads` | `registers` | array | Max 6 reads per slave. |

Max 4 slaves are stored.

### 6.2 Read Fields

| Field | Aliases | Type | Notes |
|---|---|---|---|
| `readId` | `read_id`, `id` | string | Required. |
| `name` | | string | Optional. |
| `tag` | | string | Optional. |
| `kind` | | string | `analog`, `digital`, `counter`, `enum`, `raw`. Default `analog`. |
| `func` | `function`, `functionCode` | string/number | Supports `1`, `2`, `3`, `4`, `fc_1`, `fc_2`, `fc_3`, `fc_4`, `holding`, `input`. |
| `start` | `address`, `startAddress` | number | Zero-based Modbus address used by firmware. |
| `bits` | `bitWidth`, `registerBits` | number | Converts `16`, `32`, `64` to register count `1`, `2`, `4`. |
| `qty` | `quantity`, `registerCount` | number | `1`, `2`, or `4`. |
| `dataType` | | string | See below. |
| `endianness` | | string | See below. |
| `bitIndex` | | number | `0` to `31`, for `bit`/`bool` style decoding. |
| `scale` | | number | Calibrated value = decoded raw * scale + offset. |
| `offset` | | number | See above. |
| `unit` | | string | Optional. |
| `rules` | | array | Max 2 rules, same rule schema as points. |

Supported `dataType` values:

| Value | Decode |
|---|---|
| `uint16` | Unsigned 16-bit register. Default. |
| `int16` | Signed 16-bit register. |
| `uint32` | Unsigned 32-bit from two registers. |
| `int32` | Signed 32-bit from two registers. |
| `float32` | IEEE-754 float from two registers. |
| `bool` | Bit extraction from first register. |
| `bit` | Bit extraction from first register. |
| `bitfield` | Parsed as raw 16-bit currently. |
| `enum` | Parsed as raw 16-bit currently. |
| `raw16` | Parsed as raw 16-bit. |

Supported `endianness` values for 32-bit reads:

| Value | Byte Order |
|---|---|
| `ABCD` | Big-endian register/byte order. |
| `BADC` | Byte-swapped words. |
| `CDAB` | Word-swapped. |
| `DCBA` | Full reverse. |
| `none` | Defaults to normal handling. |

## 7. Telemetry Payload

Topic:

```text
d2c/{imei}/telemetry_test
```

Shape:

```json
{
  "v": 1,
  "type": "telemetry",
  "imei": "863110085129610",
  "configId": "test-beta-config-001",
  "seq": 17,
  "ts": "2026-07-29T01:34:44+05:30",
  "values": {
    "DI_1": {
      "last": 0,
      "transitions": 1,
      "onDurationMs": 2787
    },
    "AI_1": {
      "last": 8.000,
      "min": 0.000,
      "max": 53.000,
      "avg": 2.965,
      "minTs": "2026-07-29T01:33:37+05:30",
      "maxTs": "2026-07-29T01:34:12+05:30"
    },
    "MI_1": [
      {
        "slaveId": "slave-1",
        "registers": [
          {
            "readId": "mi-r1",
            "last": 253.400,
            "min": 252.800,
            "max": 253.500,
            "avg": 253.151,
            "minTs": "2026-07-29T01:33:40+05:30",
            "maxTs": "2026-07-29T01:34:36+05:30"
          },
          {
            "readId": "mi-r2",
            "last": 1,
            "transitions": 20,
            "onDurationMs": 32297
          }
        ]
      }
    ]
  }
}
```

### 7.1 Telemetry Header

| Field | Type | Meaning |
|---|---|---|
| `v` | number | Payload schema version. Currently `1`. |
| `type` | string | Always `telemetry`. |
| `imei` | string | Device IMEI from config/device. |
| `configId` | string | Active config ID. |
| `seq` | number | Monotonic telemetry sequence persisted in external flash. |
| `ts` | string | Device RTC timestamp in IST offset format. |
| `values` | object | Channel/read values keyed by configured point key/default key. |

### 7.2 Digital Values

```json
{
  "last": 1,
  "transitions": 3,
  "onDurationMs": 12000
}
```

| Field | Meaning |
|---|---|
| `last` | Current logical state, after `activeHigh` handling. |
| `transitions` | Number of state changes in the telemetry interval. Resets after telemetry. |
| `onDurationMs` | Total ON time during the telemetry interval. Resets after telemetry. |

### 7.3 Analog Values

```json
{
  "last": 12.300,
  "min": 10.100,
  "max": 15.700,
  "avg": 12.950,
  "minTs": "2026-07-29T01:33:37+05:30",
  "maxTs": "2026-07-29T01:34:12+05:30"
}
```

| Field | Meaning |
|---|---|
| `last` | Last sampled calibrated value. |
| `min` | Minimum calibrated value during interval. |
| `max` | Maximum calibrated value during interval. |
| `avg` | Average calibrated value during interval. |
| `minTs` | Timestamp when interval min occurred. |
| `maxTs` | Timestamp when interval max occurred. |

### 7.4 Modbus Values

Modbus values are grouped under `MI_1` as an array of slave objects.

Analog Modbus read:

```json
{
  "readId": "mi-r1",
  "last": 253.400,
  "min": 252.800,
  "max": 253.500,
  "avg": 253.151,
  "minTs": "2026-07-29T01:33:40+05:30",
  "maxTs": "2026-07-29T01:34:36+05:30"
}
```

Counter Modbus read:

```json
{
  "readId": "mi-counter",
  "last": 12345,
  "count": 12,
  "delta": 12
}
```

Digital/enum/raw Modbus read:

```json
{
  "readId": "mi-r2",
  "last": 1,
  "transitions": 20,
  "onDurationMs": 32297
}
```

## 8. Event Payload

Topic:

```text
d2c/{imei}/event_test
```

Events are separate from telemetry. They are intended for low-latency alerting. The event includes `telemetrySeq`, which points to the next telemetry sequence number at the time the event was generated. The backend should use this with time proximity and `source`/`readId` to correlate an event with the nearest telemetry interval.

Shape:

```json
{
  "v": 1,
  "type": "event",
  "eventId": "DI_1-42",
  "eventSeq": 42,
  "telemetrySeq": 18,
  "imei": "863110085129610",
  "configId": "test-beta-config-001",
  "ts": "2026-07-29T01:34:20+05:30",
  "source": "DI_1",
  "eventType": "DI_RISING_EDGE",
  "severity": "info",
  "data": {
    "value": 1.000,
    "previousValue": 0.000,
    "threshold": 1.000
  }
}
```

For Modbus events, `data` can also include:

```json
{
  "slaveId": "slave-1",
  "readId": "mi-r1",
  "value": 90.000,
  "previousValue": 75.000,
  "threshold": 80.000
}
```

Event fields:

| Field | Type | Meaning |
|---|---|---|
| `eventId` | string | `{source}-{eventSeq}`. |
| `eventSeq` | number | Monotonic event sequence persisted in external flash. |
| `telemetrySeq` | number | Telemetry sequence that was next when the event was generated. |
| `source` | string | Point key or `MI_1` for Modbus analog threshold events. |
| `eventType` | string | Event type generated by rule engine. |
| `severity` | string | `info`, `warning`, `critical`. |
| `data.value` | number | Current value at event. |
| `data.previousValue` | number | Previous observed value. |
| `data.threshold` | number | Threshold edge/recovery reference. |
| `data.slaveId` | string | Present for Modbus events. |
| `data.readId` | string | Present for Modbus events. |

Event queue and throttling:

- Event JSON buffer: about 512 bytes per event.
- Event queue depth: 4 pending events.
- Max generated events per telemetry interval: `eventCapPerInterval`, default `8`.
- DI events are detected from interrupt edge handling, queued, and published through the priority publish path.
- AI/Modbus analog events are generated by threshold state machine evaluation.

## 9. Actuation Downlink

Topic:

```text
c2d/{imei}/commands_test
```

Request:

```json
{
  "type": "actuation",
  "actuationId": "act-001",
  "portKey": "RELAY_1",
  "action": "TURN_ON"
}
```

Supported `action` values:

| Action | Meaning |
|---|---|
| `TURN_ON` | Set relay output ON. |
| `TURN_OFF` | Set relay output OFF. |
| `TOGGLE` | Toggle current relay state. |

ACK topic:

```text
d2c/{imei}/message_test
```

ACK:

```json
{
  "type": "actuation_ack",
  "actuationId": "act-001",
  "status": "ACKED"
}
```

Failure:

```json
{
  "type": "actuation_ack",
  "actuationId": "act-001",
  "status": "FAILED"
}
```

## 10. OTA Downlink

Topic:

```text
c2d/{imei}/commands_test
```

Supported `type`:

- `ota`
- `firmware_update`

Request:

```json
{
  "type": "ota",
  "deploymentId": "ota-deploy-001",
  "firmwareVersion": "1.0.1",
  "deviceModel": "powerlytics-b1-beta-c",
  "artifactUrl": "http://bucket.example/firmware.bin",
  "sha256": "64-char-sha256-hex",
  "size": 123456
}
```

Aliases:

| Preferred | Alias |
|---|---|
| `firmwareVersion` | `targetVersion`, `version` |
| `artifactUrl` | `downloadUrl` |
| `sha256` | `checksumSha256` |
| `size` | `sizeBytes` |

OTA ACK/status topic:

```text
d2c/{imei}/message_test
```

OTA status shape:

```json
{
  "type": "ota_ack",
  "deploymentId": "ota-deploy-001",
  "status": "OTA_DOWNLOAD_STARTED",
  "stage": "DOWNLOAD",
  "progress": 1,
  "currentVersion": "1.0.0",
  "targetVersion": "1.0.1",
  "deviceModel": "powerlytics-b1-beta-c",
  "reason": "staging_to_external_flash"
}
```

Important OTA constraints:

- `deploymentId`, target version, `deviceModel`, artifact URL, SHA-256, and size are required.
- `deviceModel` must match the firmware model.
- Current firmware only accepts public `http://` artifact URLs.
- The artifact must fit both OTA staging and application max size.

Common OTA statuses:

| Status | Stage |
|---|---|
| `OTA_RECEIVED` | `RECEIVE` |
| `OTA_REJECTED` | `VALIDATE` |
| `OTA_DOWNLOAD_STARTED` | `DOWNLOAD` |
| `OTA_DOWNLOAD_FAILED` | `DOWNLOAD` |
| `OTA_DOWNLOAD_COMPLETE` | `DOWNLOAD` |
| `OTA_VERIFY_OK` | `VERIFY` |
| `OTA_INSTALLING` | `RESET` |
| `OTA_INSTALLED` | `BOOT` |
| `OTA_INSTALL_FAILED` | `BOOT` / `MANIFEST` |

## 11. Health Payload

Topic:

```text
d2c/{imei}/health_test
```

Shape:

```json
{
  "type": "health",
  "uptimeMs": 123456,
  "imei": "863110085129610",
  "fw": {
    "version": "1.0.0",
    "build": "build-id",
    "model": "powerlytics-b1-beta-c"
  },
  "commState": "ONLINE",
  "lastError": "NONE",
  "mqttOnline": true,
  "lastPublishAgoMs": 1000,
  "signal": {
    "valid": true,
    "rssi": 19,
    "ber": 99,
    "dbm": -75,
    "quality": "fair",
    "ageMs": 5000
  },
  "counters": {
    "atFail": 0,
    "simNotReady": 0,
    "networkRegFail": 0,
    "gprsFail": 0,
    "mqttConnectFail": 0,
    "mqttSubscribeFail": 0,
    "mqttPublishFail": 0,
    "mqttDisconnect": 0,
    "onlineEnter": 1
  },
  "logQueue": {
    "queued": 0,
    "dropped": 0
  }
}
```

## 12. Log Payload

Topic:

```text
d2c/{imei}/logs_test
```

Shape:

```json
{
  "type": "log",
  "seq": 12,
  "tsMs": 98765,
  "level": "ERROR",
  "module": "CONFIG",
  "event": "PARSE_FAILED",
  "message": "modbus config parse failed",
  "value1": 0,
  "value2": 0,
  "dropped": 0
}
```

Logs are best-effort. Firmware attempts to publish a log once and then removes it from the RAM queue to avoid duplicate logs starving telemetry.

## 13. Payload and Config Limits

| Limit | Value |
|---|---:|
| Max points | 24 |
| Max rules per point/read | 2 |
| Max context keys per rule | 2 |
| Max Modbus slaves | 4 |
| Max reads per Modbus slave | 6 |
| Telemetry JSON buffer | 3072 bytes |
| Event JSON buffer | 512 bytes |
| Event queue depth | 4 |
| Default telemetry interval | 60000 ms |
| Min telemetry interval | 1000 ms |
| Max telemetry interval | 3600000 ms |
| Default sample interval | 50 ms |
| Min sample interval | 20 ms |
| Max sample interval | 60000 ms |

String storage limits:

| Field | Max stored chars |
|---|---:|
| UUID/read/config IDs | 36 |
| `device_id` | 31 |
| `imei` | 23 |
| `deploymentId` | 31 |
| `hash` | 79 |
| `apiKey` | 159 |
| point/read `name` | 23 |
| read `tag` | 15 |
| point/read `unit` | 15 |
| context key | 23 |

Backend should keep config payloads compact. Large configs can fail from MCU heap/payload limits even if the JSON is logically valid.

UART debug printing is a local firmware build setting and is not part of the backend contract. The current beta firmware defaults `POWERLYTICS_UART_DEBUG_ENABLED` to `1U`.

## 14. Backend Handling Recommendations

- Treat telemetry and events as separate streams.
- Store `imei`, `configId`, `seq`, `eventSeq`, `telemetrySeq`, and `ts` as indexed fields.
- Correlate events to telemetry by `imei`, `configId`, `telemetrySeq`, `source`, `readId`, and timestamp proximity.
- Do not require every event to contain a full telemetry snapshot.
- Use telemetry for interval state and trend graphs.
- Use events for low-latency alerting, notification, and incident timelines.
- Parse timestamps with timezone offset. Do not strip `+05:30`.
- Keep point keys stable across config updates; backend dashboards should use keys as stable identifiers.
- For production, remove `_test` topic suffixes in firmware/build settings and update backend subscriptions accordingly.
