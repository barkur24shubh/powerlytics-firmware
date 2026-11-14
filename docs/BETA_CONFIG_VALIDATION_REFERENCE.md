# Powerlytics Beta Config Validation Reference

## 1. Purpose

This document is the backend validation reference for every config variation currently accepted and operational in the beta firmware.

It is intentionally broader than a normal deployment config. The complete reference object below is a field-coverage catalog and may exceed the device's 4096-byte MQTT downlink limit. Backend tests must select the required points, serialize the resulting inner `config`, calculate its SHA-256, and send that smaller config to the device.

## 2. Firmware Limits

| Item | Limit |
|---|---:|
| Complete MQTT downlink | Less than 4096 bytes |
| Points | 24 |
| Rules per point or Modbus read | 2 |
| Modbus slaves | 4 |
| Reads per Modbus slave | 6 |
| Physical beta DI channels | 8 |
| Physical beta AI channels | 10 |

The beta board has one physical RS-485 interface. The four supported Modbus slave definitions are devices sharing that bus, not four independent RS-485 ports.

## 3. Downlink Envelope

```json
{
  "type": "config",
  "configId": "backend-generated-config-id",
  "hash": "sha256-of-compact-inner-config",
  "config": {}
}
```

Rules:

- `configId` must also appear inside `config` and must match the outer value.
- `hash` exists only in the outer envelope.
- Hash input is `JSON.stringify(config)` encoded as UTF-8.
- Hash output is exactly 64 lowercase hexadecimal characters.
- Do not send `debugPrint`, `modbusSlaves`, `eventPolicy`, cooldown, reminder, or event-cap fields.

## 4. Comprehensive Inner Config Reference

The following object demonstrates all operational point and rule variations. Remove unused entries before deployment and then calculate the hash.

```json
{
  "v": 1,
  "device_id": "device-object-id",
  "configId": "backend-validation-001",
  "imei": "863110085129610",
  "apiKey": "device-api-key",
  "telemetryInterval": 60000,
  "points": [
    {
      "key": "DI_1",
      "kind": "digital",
      "enabled": true,
      "digital": { "activeHigh": true },
      "rules": [
        { "type": "rising_edge", "enabled": true, "severity": "info" },
        { "type": "falling_edge", "enabled": true, "severity": "info" }
      ]
    },
    {
      "key": "DI_2",
      "kind": "digital",
      "enabled": true,
      "digital": { "activeHigh": false },
      "rules": [
        { "type": "rising_edge", "enabled": true, "severity": "warning" },
        { "type": "falling_edge", "enabled": true, "severity": "critical" }
      ]
    },
    {
      "key": "DI_3",
      "kind": "digital",
      "enabled": true,
      "digital": { "activeHigh": true },
      "rules": []
    },
    {
      "key": "DI_4",
      "kind": "digital",
      "enabled": false,
      "digital": { "activeHigh": true },
      "rules": []
    },
    {
      "key": "AI_1",
      "kind": "analog",
      "enabled": true,
      "scale": 1.0,
      "offset": 0.0,
      "rules": [
        {
          "type": "threshold_high",
          "enabled": true,
          "severity": "warning",
          "durationMs": 1000,
          "recoveryDurationMs": 1000,
          "high": 80.0,
          "hysteresis": 2.0
        }
      ]
    },
    {
      "key": "AI_2",
      "kind": "analog",
      "enabled": true,
      "scale": 0.01,
      "offset": -10.0,
      "rules": [
        {
          "type": "threshold_low",
          "enabled": true,
          "severity": "critical",
          "durationMs": 500,
          "recoveryDurationMs": 2000,
          "low": 20.0,
          "hysteresis": 1.0
        }
      ]
    },
    {
      "key": "AI_3",
      "kind": "analog",
      "enabled": true,
      "scale": 1.0,
      "offset": 0.0,
      "rules": [
        {
          "type": "out_of_range",
          "enabled": true,
          "severity": "warning",
          "durationMs": 1000,
          "recoveryDurationMs": 1000,
          "low": 10.0,
          "high": 90.0,
          "hysteresis": 2.0
        }
      ]
    },
    {
      "key": "AI_4",
      "kind": "analog",
      "enabled": true,
      "scale": 1.0,
      "offset": 0.0,
      "rules": [
        {
          "type": "in_range",
          "enabled": true,
          "severity": "info",
          "durationMs": 0,
          "recoveryDurationMs": 0,
          "low": 40.0,
          "high": 60.0,
          "hysteresis": 1.0
        }
      ]
    },
    {
      "key": "AI_5",
      "kind": "analog",
      "enabled": false,
      "scale": 1.0,
      "offset": 0.0,
      "rules": []
    },
    {
      "key": "MI_1",
      "kind": "modbus",
      "slaves": [
        {
          "unique_slave_id": "slave-coils",
          "slaveId": 1,
          "serial": {
            "baudRate": 9600,
            "dataBits": 8,
            "stopBits": 1,
            "parity": "none"
          },
          "polling": {
            "intervalMs": 1000,
            "timeoutMs": 2000
          },
          "reads": [
            {
              "readId": "fc1-coil",
              "kind": "digital",
              "func": 1,
              "start": 0,
              "qty": 1,
              "dataType": "bool",
              "bitIndex": 0,
              "scale": 1.0,
              "offset": 0.0,
              "rules": []
            },
            {
              "readId": "fc2-discrete",
              "kind": "digital",
              "func": 2,
              "start": 1,
              "qty": 1,
              "dataType": "bit",
              "bitIndex": 0,
              "scale": 1.0,
              "offset": 0.0,
              "rules": []
            }
          ]
        },
        {
          "unique_slave_id": "slave-holding",
          "slaveId": 2,
          "serial": {
            "baudRate": 19200,
            "dataBits": 8,
            "stopBits": 1,
            "parity": "even"
          },
          "polling": {
            "intervalMs": 2000,
            "timeoutMs": 1500
          },
          "reads": [
            { "readId": "fc3-u16", "kind": "analog", "func": 3, "start": 0, "qty": 1, "dataType": "uint16", "endianness": "none", "scale": 1.0, "offset": 0.0, "rules": [] },
            { "readId": "fc3-i16", "kind": "analog", "func": 3, "start": 1, "qty": 1, "dataType": "int16", "endianness": "none", "scale": 0.1, "offset": 0.0, "rules": [] },
            { "readId": "fc3-u32", "kind": "counter", "func": 3, "start": 2, "qty": 2, "dataType": "uint32", "endianness": "ABCD", "scale": 1.0, "offset": 0.0, "rules": [] },
            { "readId": "fc3-i32", "kind": "analog", "func": 3, "start": 4, "qty": 2, "dataType": "int32", "endianness": "BADC", "scale": 1.0, "offset": 0.0, "rules": [] },
            {
              "readId": "fc3-f32",
              "kind": "analog",
              "func": 3,
              "start": 6,
              "qty": 2,
              "dataType": "float32",
              "endianness": "CDAB",
              "scale": 1.0,
              "offset": 0.0,
              "rules": [
                { "type": "threshold_high", "enabled": true, "severity": "critical", "durationMs": 1000, "recoveryDurationMs": 1000, "high": 250.0, "hysteresis": 5.0 }
              ]
            },
            { "readId": "fc3-raw", "kind": "raw", "func": 3, "start": 8, "qty": 1, "dataType": "raw16", "endianness": "none", "scale": 1.0, "offset": 0.0, "rules": [] }
          ]
        },
        {
          "unique_slave_id": "slave-input",
          "slaveId": 3,
          "serial": {
            "baudRate": 38400,
            "dataBits": 8,
            "stopBits": 2,
            "parity": "odd"
          },
          "polling": {
            "intervalMs": 5000,
            "timeoutMs": 2500
          },
          "reads": [
            { "readId": "fc4-abcd", "kind": "analog", "func": 4, "start": 0, "qty": 2, "dataType": "float32", "endianness": "ABCD", "scale": 1.0, "offset": 0.0, "rules": [] },
            { "readId": "fc4-badc", "kind": "analog", "func": 4, "start": 2, "qty": 2, "dataType": "float32", "endianness": "BADC", "scale": 1.0, "offset": 0.0, "rules": [] },
            { "readId": "fc4-cdab", "kind": "analog", "func": 4, "start": 4, "qty": 2, "dataType": "float32", "endianness": "CDAB", "scale": 1.0, "offset": 0.0, "rules": [] },
            { "readId": "fc4-dcba", "kind": "analog", "func": 4, "start": 6, "qty": 2, "dataType": "float32", "endianness": "DCBA", "scale": 1.0, "offset": 0.0, "rules": [] },
            { "readId": "fc4-bitfield", "kind": "raw", "func": 4, "start": 8, "qty": 1, "dataType": "bitfield", "endianness": "none", "scale": 1.0, "offset": 0.0, "rules": [] },
            { "readId": "fc4-enum", "kind": "enum", "func": 4, "start": 9, "qty": 1, "dataType": "enum", "endianness": "none", "scale": 1.0, "offset": 0.0, "rules": [] }
          ]
        },
        {
          "unique_slave_id": "slave-rules",
          "slaveId": 247,
          "serial": {
            "baudRate": 115200,
            "dataBits": 8,
            "stopBits": 1,
            "parity": "none"
          },
          "polling": {
            "intervalMs": 10000,
            "timeoutMs": 3000
          },
          "reads": [
            {
              "readId": "mb-high",
              "kind": "analog",
              "func": 4,
              "start": 0,
              "qty": 1,
              "dataType": "uint16",
              "endianness": "none",
              "scale": 0.1,
              "offset": -5.0,
              "rules": [
                { "type": "threshold_high", "enabled": true, "severity": "warning", "durationMs": 1000, "recoveryDurationMs": 1000, "high": 80.0, "hysteresis": 2.0 }
              ]
            },
            {
              "readId": "mb-low",
              "kind": "analog",
              "func": 4,
              "start": 1,
              "qty": 1,
              "dataType": "uint16",
              "endianness": "none",
              "scale": 1.0,
              "offset": 0.0,
              "rules": [
                { "type": "threshold_low", "enabled": true, "severity": "critical", "durationMs": 500, "recoveryDurationMs": 1000, "low": 10.0, "hysteresis": 1.0 }
              ]
            },
            {
              "readId": "mb-range",
              "kind": "analog",
              "func": 4,
              "start": 2,
              "qty": 1,
              "dataType": "uint16",
              "endianness": "none",
              "scale": 1.0,
              "offset": 0.0,
              "rules": [
                { "type": "out_of_range", "enabled": true, "severity": "warning", "durationMs": 1000, "recoveryDurationMs": 1000, "low": 20.0, "high": 80.0, "hysteresis": 2.0 },
                { "type": "in_range", "enabled": false, "severity": "info", "durationMs": 0, "recoveryDurationMs": 0, "low": 40.0, "high": 60.0, "hysteresis": 1.0 }
              ]
            }
          ]
        }
      ]
    }
  ]
}
```

## 5. Physical Port Coverage

The reference uses representative DI and AI channels. The backend may send the same schemas for all board channels:

| Family | Canonical keys |
|---|---|
| Digital inputs | `DI_1` through `DI_8` |
| Analog inputs | `AI_1` through `AI_10` |
| Modbus container | `MI_1` |

Omitting a physical DI or AI point does not remove that channel from telemetry. Firmware reports omitted board channels using its default configuration. To disable a channel, send its point explicitly with `"enabled": false`.

## 6. Rule Coverage

### 6.1 Digital Rules

| Type | Trigger |
|---|---|
| `rising_edge` | Logical state changes from `0` to `1`. |
| `falling_edge` | Logical state changes from `1` to `0`. |

If `rules` is empty for an enabled physical DI, firmware creates default rising and falling informational events.

### 6.2 Analog Rules

| Type | Required thresholds | Trigger |
|---|---|---|
| `threshold_high` | `high` | Value rises above `high`. |
| `threshold_low` | `low` | Value falls below `low`. |
| `out_of_range` | `low`, `high` | Value is below `low` or above `high`. |
| `in_range` | `low`, `high` | Value enters the inclusive range. |

Common operational fields:

| Field | Values |
|---|---|
| `enabled` | `true`, `false` |
| `severity` | `info`, `warning`, `critical` |
| `durationMs` | Continuous trigger duration before activation. `0` means immediate evaluation. |
| `recoveryDurationMs` | Continuous normal duration before recovery. `0` means immediate recovery. |
| `hysteresis` | Threshold margin used to prevent rapid activation/recovery oscillation. |

Rules are event definitions. No separate `eventPolicy`, cooldown, reminder, or configurable deduplication property is used.

## 7. Modbus Function Coverage

| Function | Meaning | Recommended point kind |
|---:|---|---|
| `1` | Read coils | `digital` |
| `2` | Read discrete inputs | `digital` |
| `3` | Read holding registers | `analog`, `counter`, `enum`, `raw` |
| `4` | Read input registers | `analog`, `counter`, `enum`, `raw` |

The firmware is currently a Modbus reader. Write functions are not part of this config contract.

## 8. Modbus Datatype Coverage

| `dataType` | Registers | Notes |
|---|---:|---|
| `uint16` | 1 | Unsigned 16-bit value. |
| `int16` | 1 | Signed 16-bit value. |
| `uint32` | 2 | Unsigned 32-bit value. |
| `int32` | 2 | Signed 32-bit value. |
| `float32` | 2 | IEEE-754 32-bit floating point. |
| `bool` | 1 | Extracts one bit using `bitIndex`. |
| `bit` | 1 | Same bit-extraction behavior as `bool`. |
| `bitfield` | 1 | Currently reported as a raw 16-bit value. |
| `enum` | 1 | Currently reported as a raw 16-bit value. |
| `raw16` | 1 | Raw 16-bit value. |

Do not request 64-bit decoding in the current beta firmware. Although four-register quantities can be parsed, the runtime decoder currently supports values up to 32 bits.

## 9. Endianness Coverage

| Value | Byte order |
|---|---|
| `none` | Normal/default handling. |
| `ABCD` | Normal 32-bit order. |
| `BADC` | Byte swap within each word. |
| `CDAB` | Word swap. |
| `DCBA` | Full byte reversal. |

Endianness matters for two-register 32-bit values. Use `none` for ordinary 16-bit values.

## 10. Canonical Fields and Accepted Aliases

Backend should emit the canonical field only. Firmware currently accepts the aliases for compatibility.

| Canonical field | Accepted aliases |
|---|---|
| `unique_slave_id` | `uniqueSlaveId`, `uniqueSlaveID` |
| `slaveId` | `slave_id`, `address` |
| `serial.baudRate` | `serial.baud`, `serial.baud_rate` |
| `polling.intervalMs` | `interval`, `pollIntervalMs` |
| `polling.timeoutMs` | `timeout`, `timeout_ms` |
| `reads` | `registers` |
| `readId` | `read_id`, `id` |
| `func` | `function`, `functionCode` |
| `start` | `address`, `startAddress` |
| `qty` | `quantity`, `registerCount` |

Function strings such as `fc_1`, `fc_2`, `fc_3`, `fc_4`, `coil`, `discrete`, `holding`, and `input` are accepted, but backend should emit numeric `1`, `2`, `3`, or `4` consistently.

## 11. Fields Deliberately Excluded

The following are excluded from the recommended backend-owned config because they are locally controlled, legacy, or not operational in the current beta firmware:

- `debugPrint`, `debugLogs`, `debug.print`
- Top-level `modbusSlaves`
- `eventPolicy`, cooldown and reminder fields
- `eventCapPerInterval` is still parsed by the current firmware as a legacy queue-protection limit, but backend should not send it in the new contract. It remains at the firmware default until the planned event-queue redesign removes or replaces it.
- `sendFullSnapshot`
- `sampleIntervalMs`; beta ADC DMA sampling is locally fixed at 50 ms
- `deploymentId`
- Descriptive `name`, `tag`, and `unit`
- Modbus `retries`
- `digital.edge`, `counterWidth`, and `rolloverAt`
- Rule `latchMode`, `debounceMs`, and `contextKeys`

## 12. Backend Validation Checklist

- Reject more than 24 points.
- Reject more than four Modbus slaves.
- Reject more than six reads per slave.
- Reject more than two rules per point/read.
- Reject duplicate point keys, slave IDs, slave identifiers, or read IDs.
- Require `device_id`, inner `configId`, `imei`, and `apiKey`.
- Ensure inner and outer `configId` match.
- Use only `DI_1`-`DI_8`, `AI_1`-`AI_10`, and `MI_1` for this beta board.
- Use Modbus slave IDs from `1` to `247`.
- Use function codes `1`, `2`, `3`, or `4`.
- Keep `bitIndex` between `0` and `15` in the current firmware.
- Use `qty: 1` for 16-bit values and `qty: 2` for 32-bit values.
- Keep the final compact downlink below 4096 bytes.
- Calculate the hash only after the final inner config has been constructed.
