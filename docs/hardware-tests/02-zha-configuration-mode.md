# ZHA configuration mode hardware test

This firmware changes the Zigbee signature. Install the custom quirk, restart
Home Assistant, erase/flash the device, remove its old ZHA entry, and perform a
fresh battery-only pairing.

The sleep-policy defaults and accepted ranges are:

| Setting | Default | Range |
| --- | ---: | ---: |
| Dry reporting interval | 15 min | 1-10080 min |
| Moist reporting interval | 15 min | 1-10080 min |
| Dry threshold | 45% | 0-100% |

Below the threshold the dry interval is selected; at or above it the moist
interval is selected. The dry interval may not exceed the moist interval.

## 1. Commissioning and entities

1. Start ZHA device discovery.
2. Insert the battery without USB connected.
3. Wait for commissioning to finish.
4. Confirm these entities appear and become known:
   - soil moisture percentage
   - battery percentage
   - soil sensor voltage in mV
   - wet calibration Number
   - dry calibration Number
   - dry reporting interval Number in minutes
   - moist reporting interval Number in minutes
   - dry threshold Number in percent
5. Confirm the five writable Numbers appear in the device's Configuration
   section, while moisture, battery, and raw voltage remain ordinary sensors.
6. Confirm the entity metadata uses the moisture device class for soil
   moisture and dry threshold, voltage for raw voltage and both calibration
   Numbers, duration for both reporting intervals, and battery for battery.
7. Confirm the initial calibration values are 1200 mV wet and 2750 mV dry.
8. Confirm the initial sleep policy is 15 minutes dry, 15 minutes moist, and a
   45% threshold.
9. Wait for the device to sleep and confirm the normal 15-minute report still
   updates moisture, raw voltage, and battery.

## 2. Short-press configuration mode

1. While the device is asleep on battery, press and release the button.
2. Confirm yellow appears immediately, then solid green while Zigbee rejoins,
   then blinking green once configuration mode is ready.
3. Confirm moisture, raw voltage, and battery update immediately.
4. Confirm raw voltage updates about every five seconds while green blinks.
5. Change one calibration Number in ZHA. Confirm the value is echoed back and
   that green continues blinking for about 30 seconds after the write.
6. Change the other Number and confirm the inactivity window extends again.
7. Change the moist reporting interval, then the dry reporting interval, and
   confirm both are echoed back. The dry interval must not exceed the moist
   interval.
8. Change the dry threshold and confirm it is echoed back.
9. Let the window expire. Confirm the LED turns off, then moisture, raw voltage,
   and battery receive final updates before the device returns to normal sleep.

## 3. Validation and persistence

1. Enter configuration mode again.
2. Attempt a pair where dry is not at least 100 mV above wet.
3. Confirm the firmware restores the last valid pair in ZHA.
4. Save a distinct valid pair and let configuration mode finish.
5. Enter configuration mode and attempt to set the dry reporting interval
   above the moist interval. Confirm the saved dry value is restored in ZHA.
6. Set a distinct valid moist interval first, then a dry interval and threshold.
7. Power-cycle the device and confirm the valid calibration and sleep-policy
   values are preserved.
8. With one-minute test intervals, place the measured percentage on each side
   of the configured threshold and confirm the serial `Sleep policy` message
   selects the expected interval.

## 4. Zigbee factory reset

1. Record the stored wet/dry calibration values.
2. Hold the button continuously while the sleeping battery device wakes.
3. Confirm yellow appears immediately and red remains clearly visible for about
   two seconds after the ten-second hold threshold.
4. Confirm the device no longer rejoins using its old Zigbee network state.
5. Open ZHA discovery and commission it again.
6. Confirm all eight entities return and the calibration and sleep-policy
   values survived the Zigbee-only reset.

## Result: 2026-10-10

- Battery-only commissioning completed and all eight ZHA entities appeared.
- Valid dry/moist reporting intervals and the dry threshold were accepted and
  echoed back during configuration mode.
- A moist interval below the current dry interval was rejected and restored to
  the previously saved value.
- Below-threshold readings selected the dry interval; readings at or above the
  threshold selected the moist interval.
- Sleep-policy settings survived complete power cycles.
- Existing moisture, raw-voltage, battery, calibration, configuration-mode,
  and commissioning behavior remained functional.

The ZHA configuration mode and persistent sleep policy pass hardware testing.
