# ZHA quirk

Copy `xiao_soil_moisture.py` to Home Assistant at:

```text
/config/zha_quirks/xiao_soil_moisture.py
```

Enable the custom quirk directory in `configuration.yaml`:

```yaml
zha:
  enable_quirks: true
  custom_quirks_path: /config/zha_quirks
```

Restart Home Assistant after copying or changing the quirk. The device page
should then show the soil sensor voltage in mV; writable wet/dry calibration
Numbers; dry and moist reporting intervals in minutes; and a dry-threshold
percentage Number. The five writable Numbers appear in the device's
Configuration section. The quirk also exposes the standard software build ID
as a diagnostic Firmware version sensor. That sensor does not configure
periodic attribute reporting.

The quirk assigns Home Assistant device classes matching each value: moisture
for the soil reading and dry threshold, voltage for the raw sensor output and
calibration points, duration for the reporting intervals, and battery for the
standard battery entity.

The current firmware identifies the manufacturer as `Seeed Studio`. The quirk
also accepts the older misspelled `Seed Studio` identity so devices paired with
an earlier development build continue to load it.

Because this firmware adds endpoints 2-7 to the Zigbee device signature,
remove an older version of the device from ZHA and pair it again after flashing.
