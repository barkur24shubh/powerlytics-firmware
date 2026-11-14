# B1-V4-C Firmware Production-Readiness Audit

## Purpose

This document records the current B1-V4-C software architecture, the work migrated from the beta firmware, and the remaining risks before remote-site deployment. It is an audit and stabilization baseline; it does not approve the current debug build as production firmware.

## Current Repository Structure

- `Core/`: shared gateway application, RTOS tasks, cellular/MQTT, configuration, telemetry, rules, Modbus, storage, logging, watchdog, RTC, USB CDC, and board-independent I/O APIs.
- `Board/Common/`: common board definitions.
- `Board/powerlytics_b1_beta_c/`: original beta-board profile.
- `Board/powerlytics_b1_v4_c/`: B1-V4-C hardware profile and pin mapping.
- `SecondaryMCU/analog_acquisition_c031/`: isolated STM32C031 analog acquisition firmware.
- `scripts/build_board.sh`: builds one selected board independently.
- `scripts/flash_board.sh`: flashes the selected board artifact.
- `artifacts/boards/<board>/`: per-board firmware output.

The intended model is one shared codebase with separately compiled board variants. Hardware-dependent pin counts, polarities, peripherals, and capabilities belong in the board profile; application behavior belongs in `Core`.

## B1-V4-C Hardware Configuration

| Function | Current firmware mapping |
|---|---|
| Main MCU | STM32F407 |
| Connectivity | A7670C cellular modem on USART6, PC6/PC7 |
| Modem reset / power key | PD14 / PD15, active-low control |
| Modem power-key pulse | 1200 ms, followed by 8000 ms boot settling |
| Modbus | USART3 TX/RX on PD8/PD9 |
| RS485 DE/RE | PD10 |
| External flash | W25Q16 on SPI1 PB3/PB4/PB5, CS PD7 |
| SD card | SDIO, 1-bit mode |
| Relay outputs | RO1 PB15, RO2 PB14, active high |
| Relay feedback | RO1 PD12, RO2 PD11, active low |
| Digital inputs | DI1..DI6 on PE10..PE15, active low, no internal pull |
| Pulse counters | PE5 and PB9 |
| Heartbeat LED | PD6, active high |
| Communication/activity LED | PD5, active high |
| Secondary ADC UART | USART1 PB6/PB7, 115200 baud |
| Analog inputs | AI1..AI10 supplied by secondary MCU |
| Input-power monitor | Secondary PA12/ADC12, 33k/3k divider (12:1) |

## Secondary MCU Acquisition

The STM32C031 is intentionally a simple acquisition device:

- Samples AI1..AI9 on PA0..PA8.
- Samples AI10 on PA11/ADC11.
- Samples supply monitoring on PA12/ADC12.
- Captures 11 raw 12-bit values every 50 ms.
- Sends framed snapshots to the main MCU at 115200 baud.
- Performs no scaling, threshold detection, rule evaluation, or cloud logic.

The main MCU owns channel scaling, engineering-unit conversion, rules, telemetry aggregation, and cloud publishing. This is the desired responsibility split.

## Implemented Application Behavior

- Cloud configuration is received over MQTT and persisted in W25Q flash.
- Telemetry supports analog, digital, relay, pulse-counter, and Modbus points.
- Analog samples are accumulated at the configured sample interval, currently normally 50 ms.
- Digital input state and configured rule/event processing are handled on the main MCU.
- Modbus configuration supports serial settings, polling, reads, scaling, and telemetry aggregation.
- Relay commands are accepted over downlink and an acknowledgement is published.
- Relay feedback is sampled and logged, but acknowledgement is intentionally independent of feedback mismatch.
- W25Q flash stores configuration and deferred/offline critical logs.
- Reset causes, boot count, prior boot stage, heap, task stack, signal quality, and selected failures are logged.
- Power-monitor raw ADC, sensed millivolts, and reconstructed supply millivolts are available in health JSON.
- Heartbeat and communication LEDs are implemented; the activity LED represents successful publish/downlink activity, not every byte on the modem UART.

## Temporary Test Configuration Still Present

These items must not ship unchanged:

- IMEI/topic identity is hardcoded to `863110085129610`.
- MQTT topics use the `_test` suffix (`commands_test`, `telemetry_test`, `health_test`, and related topics).
- The main build is a debug build (`-O0`, `-g3`, `-DDEBUG`).
- Full telemetry JSON and detailed AT diagnostics can be printed locally.
- The working tree contains extensive uncommitted and untracked source, generated files, and firmware artifacts; there is no clean reproducible release point.
- Firmware version/build identity is not sufficient to distinguish every field image.

## Production Risks

### P0: Watchdog supervision can be bypassed

The IWDG is currently compiled on (`APP_RUNTIME_WATCHDOG_ENABLED=1`), so it is not simply disabled. However, many paths call `app_watchdog_force_refresh()`. A force refresh feeds the hardware watchdog without checking registered task health. Long modem waits, engine operations, LED code, startup operations, and the watchdog task itself can therefore keep the MCU alive even when an essential task has stalled.

The runtime timeout is also 180 seconds, which is too broad to detect many deadlocks promptly. Only runtime and Modbus are registered in `main`; LED has a watchdog definition but is not registered. The result can be a powered device that stops useful work and never self-recovers.

Required correction:

1. Remove force-feeding from normal runtime paths.
2. Permit force-feeding only in tightly bounded, documented boot/flash critical sections.
3. Make one supervisor the only normal IWDG feeder.
4. Feed only when all registered subsystems have advanced through meaningful checkpoints.
5. Monitor connectivity progress, MQTT transaction progress, runtime, Modbus, secondary ADC reception, flash operations, and any required event worker.
6. Persist the subsystem and state that blocked refresh, then validate recovery with intentional fault injection.

### P0: MQTT uplink and downlink share one stream without a URC dispatcher

Publish and subscribe processing consume the same modem UART DMA ring. Downlink parsing runs cooperatively before/after uplink, but publishing is a blocking multi-command AT transaction. While an uplink is in progress, unsolicited downlink URCs can be interleaved with expected publish responses. Wait/flush operations can consume or discard those URCs before `MQTT_Subscriber_Run()` handles them.

This matches the observed behavior: an uplink can succeed while a simultaneous downlink is missed. Publishing a downlink acknowledgement synchronously further extends the period during which incoming traffic is not independently serviced.

Required correction:

1. Give exactly one modem transport task ownership of the UART/ring and AT parser.
2. Parse every modem line/URC continuously and dispatch typed events.
3. Queue outbound MQTT operations rather than executing them inside command handlers.
4. Route `CMQTTRXPAYLOAD` into a bounded downlink queue even during publish/connect/recovery.
5. Eliminate broad RX flushes; discard only bytes belonging to a known transaction.
6. Add transaction IDs, timeout stages, queue depth/high-water metrics, and duplicate-command handling.
7. Stress-test simultaneous telemetry, events, logs, acknowledgements, and cloud commands.

### P0: Connectivity recovery is not yet proven as an end-to-end liveness system

The communication manager has sync, SIM, registration, GPRS, MQTT, subscribe, backoff, modem-reset, and MCU-reset states. That is a good foundation, but remote reliability depends on proven progress guarantees, not only retries. Current watchdog force-feeding can prevent the final MCU recovery, and blocking MQTT traffic can make a live modem appear unavailable.

Required correction:

- Define maximum allowed time in every state.
- Track last successful AT response, registration, broker connection, subscription, downlink, and confirmed publish.
- Escalate deterministically: retry transaction, restart MQTT, reactivate PDP, hardware-reset modem, then reset MCU.
- Rate-limit resets while still guaranteeing eventual escalation.
- Preserve pending telemetry/events across recovery.
- Test SIM loss, weak network, broker outage, DNS/TLS failure, modem silence, malformed URCs, and network return after hours.

### P0: Relay acknowledgement does not prove physical actuation

Relay feedback currently reports mismatch locally, but the cloud acknowledgement is intentionally sent as success independently of feedback. This prevents failed optocouplers, contacts, drivers, or wiring from being visible to the control plane. The feedback circuit also showed an invalid logic-low voltage in hardware testing.

Required correction after the hardware feedback circuit is corrected:

- Define acknowledgement states such as `ACCEPTED`, `ACTUATED`, `FAILED`, and `UNKNOWN`.
- Acknowledge receipt immediately, then publish final physical confirmation after debounce/settling.
- Include requested state, command GPIO state, raw feedback level, interpreted state, timeout, and fault code.
- Make commands idempotent using `actuationId` and persist recent results across reconnect/reset.

### P1: Periodic health publishing is disabled

`app_health_run_once()` is commented out because it competed with telemetry. Consequently, power-monitor information and other health fields may be implemented but not sent periodically. Disabling health to protect telemetry is itself evidence that transport scheduling needs separation.

Required correction: enqueue health at a lower priority with rate limiting and coalescing, rather than calling a blocking publish directly or disabling it.

### P1: Debug logging can alter real-time behavior

`print()` allocates a 1024-byte stack buffer and the UART path uses blocking `HAL_UART_Transmit(..., 1000)`. Full JSON and verbose AT traces can consume significant CPU time and stack, affect modem timing, and hide race conditions. USB transmission is nonblocking but drops output while busy, so logs are not complete evidence.

Required correction: compile-time log levels, a bounded asynchronous log queue, no full payload printing in release, counters for dropped logs, and a release build where logging cannot block control paths.

### P1: Configuration and identity are test-oriented

One hardcoded IMEI makes multiple deployed units share topics and command identity. `_test` topics can silently isolate production services or route commands incorrectly. Large cloud configurations have already exposed allocation and parse failures.

Required correction: unique hardware-derived/provisioned identity, schema versioning, payload-size limits, streaming or bounded parsing, atomic validate-then-commit, rollback to last-known-good configuration, and explicit deployment result codes.

### P1: Release traceability and reproducibility are weak

The active tree has many uncommitted/untracked files and generated artifacts. A field binary cannot yet be reliably reconstructed from a commit, board profile, compiler version, and configuration. Debug `-O0` output is being flashed during bring-up.

Required correction: establish a clean stabilization branch, commit board/secondary sources, exclude generated churn, create release build profiles, embed Git SHA/board/protocol versions, generate checksums/manifests, and archive the exact main-plus-secondary firmware pair.

### P1: Main/secondary protocol compatibility is implicit

The analog frame grew to 11 samples. Main and secondary firmware must currently be updated as a pair. A mismatched field pair may drop frames or map channels incorrectly.

Required correction: explicit protocol version, channel bitmap/count, payload length, CRC, sequence number, stale timeout, mismatch diagnostics, and a paired release manifest. The main MCU must fail visibly rather than publish stale or wrongly mapped values.

### P1: Data queues can overflow during blocking cellular work

The secondary sends snapshots every 50 ms while cellular AT operations may block for seconds. Queues are finite. Overflow/stale statistics exist in parts of the pipeline but are not part of a clear health contract.

Required correction: drain acquisition independently of cloud operations, use latest-value/coalescing semantics where appropriate, expose drops and sample age, and verify worst-case behavior during long outages and large publishes.

### P1: Offline delivery semantics need formal guarantees

Critical flash logging exists, but telemetry/event/acknowledgement durability and ordering need to be explicit. Some code treats an uncertain publish as delivered to avoid duplicates, which can lose messages. Other records can be replayed and duplicated.

Required correction: define at-least-once semantics, sequence IDs, deduplication keys, priority, retention limits, erase policy, and acknowledgement rules for each message class.

### P2: Analog and power-monitor accuracy are not calibrated

The main conversion assumes a fixed ADC reference and a nominal 12:1 divider. Resistor tolerance, ADC gain/offset, supply-reference variation, noise, cross-channel settling, and stale secondary data can affect values. Board-level AI scale defaults are 1.0 and cloud scaling must not be accidentally duplicated or omitted.

Required correction: define the raw-to-engineering-unit contract, calibrate per board if needed, use filtering and plausibility limits, report sample age/link status, and validate all channels over their full electrical range.

### P2: DI event behavior needs stress validation

The B1-V4-C mapping and polarity now work in isolated testing. Production validation still needs bounce/noise handling, simultaneous transitions, event storms, rule duration/hysteresis, boot-state suppression, and queue overflow behavior.

### P2: Modbus recovery needs field validation

USART3 and DE/RE on PD10 work after correcting the board mapping and A/B wiring. Remaining tests include missing slave, wrong baud/parity, CRC storms, bus contention, stuck RX/TX, maximum read sets, retries, and ensuring Modbus failure cannot starve connectivity or watchdog checkpoints.

### P2: Storage failure policy is incomplete

W25Q is essential for configuration and deferred logs; SD initialization has often failed and logging is disabled afterward. Production behavior must distinguish optional SD failure from mandatory W25Q failure and must avoid boot loops or silent data loss.

### P2: USB CDC must remain diagnostically isolated

USB CDC has historically disconnected/re-enumerated on reset. Field operation should never depend on USB state, and attaching/removing a host must not block tasks, alter watchdog behavior, or reset the MCU. The monitor script reconnecting is a host convenience, not a firmware reliability mechanism.

## Production Stabilization Order

1. Freeze a known working main/secondary pair and create a reproducible release build.
2. Redesign watchdog ownership and prove reset/recovery using injected hangs.
3. Introduce a single-owner modem transport with continuous URC dispatch and queued outbound operations.
4. Harden connectivity escalation and run long broker/network/modem outage tests.
5. Restore nonblocking health publication and expose recovery/watchdog/queue/sample-age metrics.
6. Define reliable command/acknowledgement semantics and repair relay feedback verification.
7. Harden config persistence, identity provisioning, protocol compatibility, and offline delivery.
8. Complete DI, Modbus, ADC/power, storage, USB, and power-cycle HIL tests.
9. Run a multi-day soak test before any remote deployment.

## Minimum Release Gates

- No watchdog force-refresh in normal runtime logic.
- Injected deadlocks recover with a recorded root cause.
- No missed downlink during continuous telemetry/event/log publishing.
- Broker/network outage for at least 24 hours recovers automatically without intervention.
- Modem nonresponse escalates through bounded recovery and returns online.
- Pending data survives MCU/modem resets within defined retention limits.
- Invalid/oversized configuration cannot corrupt the last-known-good configuration.
- Main/secondary protocol mismatch is detected and reported.
- DI, relay, Modbus, ADC, power monitor, W25Q, and USB tests pass on B1-V4-C hardware.
- Unique device identity and production topics are verified.
- Release artifact is reproducible from a clean commit and includes version, Git SHA, board SKU, checksums, and paired secondary version.
- A minimum 72-hour mixed-traffic soak test completes without unexplained reset, heap decline, stack watermark violation, queue overflow, or missed command.

## Recommended Immediate Scope

The first implementation cycle should touch only two areas:

1. Watchdog/liveness redesign based on the stable base firmware behavior, with fault-injection tests.
2. MQTT transport serialization and URC/downlink dispatch, with simultaneous uplink/downlink stress tests.

Board mappings and working ADC/DI/Modbus behavior should remain frozen while those two reliability foundations are corrected.
