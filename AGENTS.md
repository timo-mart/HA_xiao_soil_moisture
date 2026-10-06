# AGENTS.md

## Project Overview

This repository contains firmware for the Seeed Studio XIAO Soil Moisture Sensor based on the ESP32-C6.

The primary goal is to provide a reliable, low-power Zigbee soil moisture sensor for Home Assistant, with ZHA as the main target.

The device should operate as a sleepy Zigbee end device and should not require Wi-Fi during normal operation.

## Primary Goals

Prioritize the following, in this order:

1. Reliable operation with Home Assistant ZHA.
2. Low power consumption and long battery life.
3. Stable and repeatable soil moisture readings.
4. Standards-compliant Zigbee behavior.
5. Simple maintenance and understandable firmware.
6. Minimal dependence on custom Home Assistant quirks.
7. Good recovery behavior after power loss, coordinator restart, or Zigbee network interruption.

## Hardware

Target hardware:

- Seeed Studio XIAO Soil Moisture Sensor
- MCU: ESP32-C6
- Radio: IEEE 802.15.4 / Zigbee
- Power source: battery
- Capacitive soil moisture sensing
- Physical button
- On-device LEDs

Do not change hardware pin assignments unless the change is explicitly required and verified against the hardware documentation or existing working firmware.

Before modifying any hardware-related constants, inspect the existing implementation and document the reason for the change.

## Zigbee Requirements

The device must operate as a battery-powered Zigbee end device.

ZHA compatibility is the primary target.

Prefer standard Zigbee clusters and attributes wherever practical.

Avoid manufacturer-specific Zigbee attributes unless standard clusters cannot represent the required functionality.

Do not introduce a requirement for a custom ZHA quirk unless absolutely necessary.

When changing Zigbee behavior, inspect:

- endpoint definitions
- device type
- input and output clusters
- attribute types
- reporting configuration
- battery reporting
- sleepy-end-device behavior
- join and rejoin behavior

Existing Zigbee compatibility should be preserved unless a change is intentionally documented.

## Home Assistant Requirements

The device should expose useful entities directly through ZHA.

At minimum, the device should provide:

- soil moisture percentage
- battery percentage

Additional entities may be added when useful, but avoid unnecessary diagnostic entities that increase reporting traffic or power consumption.

Plant-specific watering logic does not belong in the firmware.

For example, the firmware should report that soil moisture is 28%, but Home Assistant should decide whether 28% means a particular plant needs watering.

## Soil Moisture Measurement

Soil moisture readings should be:

- stable
- repeatable
- bounded between 0% and 100%
- based on calibrated sensor values when calibration exists
- based on safe defaults when calibration does not exist

The current sensor behavior uses voltage readings where wetter soil corresponds to a lower measured voltage.

Use a two-point calibration model:

- `dry_mv`: sensor voltage representing the dry reference
- `wet_mv`: sensor voltage representing the wet reference

The percentage calculation should conceptually follow:

```cpp
moisture_pct =
    100.0f * (dry_mv - measured_mv) / (dry_mv - wet_mv);
```

Clamp the result to the range 0–100%.

Do not assume that calibration values are valid merely because they exist.

Validate calibration data before using it.

## Calibration

Calibration is a firmware-level sensor calibration feature.

Calibration must remain separate from plant-specific watering thresholds.

The intended calibration model is:

1. Capture a dry reference.
2. Capture a wet reference.
3. Validate the measurements.
4. Persist them in non-volatile storage.
5. Use them for subsequent moisture percentage calculations.

Calibration data should survive:

- deep sleep
- resets
- battery replacement
- normal firmware restarts

Use ESP32 non-volatile storage where appropriate.

If calibration data is absent or invalid, fall back to known-safe default values.

Do not silently overwrite valid calibration data.

If implementing a physical-button calibration workflow, use clear state transitions and LED feedback.

Avoid long blocking delays because they can increase power consumption and complicate Zigbee behavior.

## Power Consumption

Battery life is a major design requirement.

Assume every unnecessary wakeup, radio transmission, LED activation, ADC operation, or long-running loop has a cost.

Prefer:

- deep sleep whenever possible
- short wake periods
- minimal Zigbee transmissions
- local processing before transmission
- reporting only meaningful changes
- maximum-report intervals to prevent stale values

Avoid:

- continuous radio operation
- Wi-Fi
- unnecessary logging in production operation
- frequent LED activity
- unnecessary polling
- high-frequency Zigbee reporting

When changing wake or reporting behavior, explicitly consider the battery-life impact.

## Sampling and Reporting

Sampling and Zigbee reporting are separate concerns.

It may be useful to sample more often than values are transmitted.

Prefer designs where:

- sensor measurements occur on a predictable schedule
- Zigbee reports are sent when the value changes sufficiently
- a maximum reporting interval ensures Home Assistant eventually receives a fresh value

Avoid embedding plant-specific thresholds such as "wake more often when soil is below 45%" unless there is a strong technical reason.

Home Assistant should normally handle watering thresholds and plant-specific automation.

## Deep Sleep

Deep sleep is expected during normal operation.

Changes must not unintentionally prevent the ESP32-C6 from returning to deep sleep.

When modifying startup, Zigbee, calibration, or reporting logic, verify that:

- wake reason is handled correctly
- required measurements complete before sleep
- required Zigbee transmissions are given enough time to complete
- the device does not remain awake indefinitely on network failure
- button-triggered behavior does not break normal sleep behavior

## Zigbee Join and Rejoin Behavior

The device should recover gracefully if:

- the Home Assistant host restarts
- the Zigbee coordinator restarts
- the Zigbee network is temporarily unavailable
- the device wakes while the coordinator is offline
- the device loses its parent router
- the battery is removed and reinserted

Do not implement aggressive reconnect loops that keep the radio active and drain the battery.

Prefer bounded retry behavior followed by sleep and retry on a later wake cycle.

## Persistent Storage

Use persistent storage sparingly.

Suitable persistent values include:

- wet calibration value
- dry calibration value
- calibration version or validity marker
- future user-configurable device settings

Avoid writing values on every wake cycle.

Flash writes should be minimized to reduce wear and unnecessary power consumption.

When persistent storage formats change, provide a versioning or validation mechanism where practical.

## Firmware Updates

OTA is desirable but secondary to stable sensing, calibration, Zigbee compatibility, and battery life.

Do not add OTA support if it significantly compromises:

- deep sleep
- flash size
- reliability
- Zigbee compatibility
- battery life

Any OTA implementation should be documented clearly.

## Code Quality

Prefer:

- small focused functions
- descriptive names
- explicit state machines where appropriate
- constants instead of unexplained magic numbers
- comments explaining why, not restating what the code already says
- clear separation between sensing, calibration, Zigbee, storage, and power-management logic

Avoid unnecessary abstractions in a small embedded project.

Do not perform large unrelated refactors while implementing a focused feature.

## Change Discipline

Before making significant changes:

1. Inspect the existing implementation.
2. Understand the current behavior.
3. Identify which behavior must remain unchanged.
4. Make the smallest reasonable modification.
5. Build the project.
6. Fix all compiler errors introduced by the change.
7. Summarize the resulting behavior.

Do not change several major subsystems at once unless required.

Prefer incremental commits or logically separated changes.

## Build Requirements

After modifying firmware:

- run the normal project build
- resolve build errors
- report warnings that may indicate real problems
- do not claim hardware behavior was verified unless it was actually tested on physical hardware

A successful build does not prove that:

- Zigbee pairing works
- ZHA creates the expected entities
- deep sleep behaves correctly
- battery consumption is acceptable
- ADC readings are correct

Clearly distinguish compile-time validation from hardware validation.

## Testing Strategy

When practical, test logic separately from hardware-specific behavior.

Particularly important cases include:

- uncalibrated sensor
- valid calibration
- invalid calibration
- equal wet and dry calibration values
- measured voltage above dry calibration
- measured voltage below wet calibration
- values near 0%
- values near 100%
- corrupted persistent calibration data
- loss of Zigbee network
- coordinator restart
- cold boot
- deep-sleep wake
- button-triggered calibration

Clamp or reject invalid values safely rather than relying on undefined behavior.

## Logging

Logging is useful during development but should not materially increase production power consumption.

Keep high-frequency logging out of normal battery-powered operation.

Do not log continuously while the device should otherwise be sleeping.

When adding logs, prefer concise messages for:

- boot reason
- Zigbee join status
- measured voltage
- computed moisture percentage
- calibration values
- calibration state
- battery state
- sleep duration

## LED Behavior

LEDs are primarily diagnostic and user-feedback devices.

Use them sparingly because they consume power.

LED behavior should be clearly defined for states such as:

- boot
- Zigbee pairing
- calibration start
- dry calibration capture
- wet calibration capture
- calibration success
- calibration failure

Do not leave LEDs on during normal sleep operation.

## Button Behavior

Avoid accidental activation of destructive or persistent actions.

If the button is used for calibration, factory reset, or pairing:

- use deliberate interaction patterns
- debounce input
- clearly separate actions
- provide visible feedback
- avoid ambiguity between single, double, triple, and long presses

Persistent calibration values should not be erased by an accidental short press.

## Safety Against Regressions

Do not silently change:

- hardware pin assignments
- ADC configuration
- Zigbee endpoint IDs
- Zigbee cluster choices
- device identity
- sleep duration
- moisture calculation orientation
- battery calculation
- pairing/reset behavior

If any of these must change, document both the previous and new behavior.

## Preferred Development Workflow

For each feature request:

1. Inspect the relevant source files.
2. Explain the existing behavior.
3. Identify risks.
4. Propose a minimal implementation.
5. Implement the change.
6. Build the project.
7. Review the diff.
8. Summarize:
   - files changed
   - behavior changed
   - assumptions made
   - remaining hardware tests

Do not begin with a broad rewrite unless explicitly requested.

## Initial Development Priorities

Unless instructed otherwise, prioritize work in this order:

1. Preserve and verify current ZHA compatibility.
2. Implement persistent two-point wet/dry calibration.
3. Improve calibration UX using the physical button and LEDs.
4. Review sampling and Zigbee reporting strategy.
5. Improve resilience around Zigbee reconnect/rejoin behavior.
6. Measure and optimize battery consumption.
7. Consider configurable reporting intervals.
8. Consider Zigbee-accessible configuration.
9. Consider OTA firmware updates.

## Definition of Done

A firmware feature is not considered fully complete merely because it compiles.

For hardware-related features, clearly state which of the following have been verified:

- compiles successfully
- flashes successfully
- boots successfully
- joins ZHA successfully
- exposes expected entities
- reports correct values
- persists settings across reset
- survives deep sleep
- survives battery removal
- reconnects after Zigbee interruption
- has acceptable measured power consumption

Never claim verification for steps that were not actually performed.