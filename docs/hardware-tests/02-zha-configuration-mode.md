# ZHA configuration mode hardware test

This firmware changes the Zigbee signature. Install the custom quirk, restart
Home Assistant, erase/flash the device, remove its old ZHA entry, and perform a
fresh battery-only pairing.

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
5. Confirm the initial calibration values are 1200 mV wet and 2750 mV dry.
6. Wait for the device to sleep and confirm the normal 15-minute report still
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
7. Let the window expire. Confirm the LED turns off, then moisture, raw voltage,
   and battery receive final updates before the device returns to normal sleep.

## 3. Validation and persistence

1. Enter configuration mode again.
2. Attempt a pair where dry is not at least 100 mV above wet.
3. Confirm the firmware restores the last valid pair in ZHA.
4. Save a distinct valid pair and let configuration mode finish.
5. Power-cycle the device and confirm the valid pair is preserved.

## 4. Zigbee factory reset

1. Record the stored wet/dry calibration values.
2. Hold the button continuously while the sleeping battery device wakes.
3. Confirm yellow appears immediately and red remains clearly visible for about
   two seconds after the ten-second hold threshold.
4. Confirm the device no longer rejoins using its old Zigbee network state.
5. Open ZHA discovery and commission it again.
6. Confirm all five entities return and the wet/dry calibration values survived
   the Zigbee-only reset.
