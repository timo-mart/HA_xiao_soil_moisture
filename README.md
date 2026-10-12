# XIAO Zigbee Soil Moisture Sensor

Battery-oriented Zigbee firmware for the [Seeed Studio XIAO Soil Moisture
Sensor](https://wiki.seeedstudio.com/xiao_soil_moisture_sensor/), built with
PlatformIO and Arduino-ESP32.

## Features

- Soil moisture, calibrated sensor millivolts, and battery reporting through ZHA
- Factory-calibrated ESP32-C6 ADC millivolt conversion
- Averaged battery measurement with build-time NiMH or alkaline percentage curves
- Standard Zigbee metadata for one AA cell and a chemistry-specific software build ID
- Persistent wet/dry calibration values exposed as ZHA Configuration Numbers
- Persistent dry/moist reporting intervals and dry threshold in ZHA Configuration
- Adaptive deep sleep on battery power and frequent reporting on USB power
- Automatic Zigbee reconnect retries with battery-conscious backoff
- Short-press configuration mode for immediate reports and setting writes
- Ten-second button hold for Zigbee factory reset
- Immediate LED feedback before Zigbee reconnects

## Button behavior on battery

- Short press: yellow acknowledges the wake immediately, solid green confirms
  the short press, and blinking green means the Zigbee configuration window is
  ready. The sensor reports moisture, raw millivolts, and battery immediately.
- Calibration or sleep-policy writes extend the 30-second inactivity window. An absolute
  two-minute limit prevents the device remaining awake indefinitely.
- Hold for ten seconds: a two-second red indication confirms the factory-reset
  request before the device erases only its Zigbee network state and starts
  fresh commissioning. Stored soil calibration and sleep policy remain intact.

The configuration window ends with a measurement using the latest calibration.
The device then makes a one-second deep-sleep transition back to normal sleepy
operation, reports once more, and schedules its next wake from the saved policy.

## ZHA setup

Install the custom quirk in [`zha_quirks`](zha_quirks/README.md) to give the raw
voltage, calibration, and sleep-policy entities their correct names, units, and
ranges, and to expose the software build ID as a diagnostic sensor.

## Build

The default environment targets a rechargeable NiMH AA cell:

```powershell
pio run -e seeed_xiao_esp32_c6
```

To build for a disposable alkaline AA cell instead:

```powershell
pio run -e seeed_xiao_esp32_c6_alkaline
```

Changing chemistry only changes the internal voltage-to-percentage curve. A
normal upload preserves Zigbee pairing and stored device settings; do not erase
the device when switching profiles. ZHA can identify the installed profile from
the standard software build ID (`dev2.09-nimh` or `dev2.09-alkaline`). The
custom quirk displays that value as a diagnostic Firmware version sensor without
periodic reporting. The existing battery entity also advertises one AA cell
through standard read-only Power Configuration attributes.

OTA firmware updates are not yet implemented.
