# Battery measurement hardware test

The firmware averages eight factory-calibrated ADC millivolt readings and maps
the result through a chemistry-specific, piecewise-linear percentage curve.
Battery voltage remains an internal diagnostic and is not added to the Zigbee
device signature.

The default `seeed_xiao_esp32_c6` environment uses the NiMH curve. The
`seeed_xiao_esp32_c6_alkaline` environment uses the alkaline curve. Battery
chemistry cannot be inferred reliably because the voltage ranges of a NiMH
cell and a partly discharged alkaline cell overlap.

## NiMH verification

1. Build and upload `seeed_xiao_esp32_c6` without erasing the device.
2. Power the device from a charged NiMH AA cell. USB may remain connected for
   serial logging if the battery is also installed.
3. Confirm the log contains `chemistry=NiMH`, a plausible calibrated voltage,
   and the calculated percentage.
4. Confirm ZHA receives the same integer percentage and no battery-voltage
   entity appears.
5. Trigger configuration mode and confirm its initial and final battery reports
   use the new percentage without affecting the other seven entities.
6. Perform at least one normal battery wake and confirm the device still enters
   deep sleep on its configured schedule.
7. Confirm a calculated 0% does not disable future timer wakeups; the device
   must continue its normal report schedule while it remains operational.

## Curve behavior

If practical, record the logged millivolts and reported percentage immediately
after charging and periodically through one discharge cycle. The percentage is
an estimate from terminal voltage, so load, temperature, cell model, and
post-charge recovery will affect it. Runtime between full charge and shutdown
is the meaningful battery-life measurement.

## Alkaline build check

1. Build `seeed_xiao_esp32_c6_alkaline` and confirm it compiles successfully.
2. A normal upload may be used later when changing the physical cell chemistry;
   do not erase or remove the device from ZHA.
3. With an alkaline AA installed, confirm the log reports
   `chemistry=alkaline` and a plausible percentage.

## Pass criteria

- Both chemistry environments compile.
- Battery detection uses calibrated millivolts rather than raw ADC counts.
- A charged NiMH cell reports near the upper end of the percentage range.
- Battery, moisture, raw-voltage, and configuration reports remain functional.
- The device signature, Zigbee pairing, and sleep behavior are unchanged.
