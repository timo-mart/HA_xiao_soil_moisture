# XIAO Zigbee Soil Moisture Sensor

Battery-oriented Zigbee firmware for the [Seeed Studio XIAO Soil Moisture
Sensor](https://wiki.seeedstudio.com/xiao_soil_moisture_sensor/), built with
PlatformIO and Arduino-ESP32.

## Features

- Soil moisture, calibrated sensor millivolts, and battery reporting through ZHA
- Factory-calibrated ESP32-C6 ADC millivolt conversion
- Persistent wet/dry calibration values exposed as ZHA Number entities
- Deep sleep on battery power and frequent reporting on USB power
- Short-press configuration mode for immediate reports and calibration writes
- Ten-second button hold for Zigbee factory reset
- Immediate LED feedback before Zigbee reconnects

## Button behavior on battery

- Short press: yellow acknowledges the wake immediately, solid green confirms
  the short press, and blinking green means the Zigbee configuration window is
  ready. The sensor reports moisture, raw millivolts, and battery immediately.
- Calibration writes extend the 30-second inactivity window. An absolute
  two-minute limit prevents the device remaining awake indefinitely.
- Hold for ten seconds: a two-second red indication confirms the factory-reset
  request before the device erases only its Zigbee network state and starts
  fresh commissioning. Stored soil calibration remains intact.

The configuration window ends with a measurement using the latest calibration.
The device then makes a one-second deep-sleep transition back to normal sleepy
operation and reports once more.

## ZHA setup

Install the custom quirk in [`zha_quirks`](zha_quirks/README.md) to give the raw
voltage and calibration entities their correct names, units, and ranges.

## Build

```powershell
pio run -e seeed_xiao_esp32_c6
```

OTA firmware updates are not yet implemented.
