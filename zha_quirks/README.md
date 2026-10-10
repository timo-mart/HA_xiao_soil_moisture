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
should then show the soil sensor voltage in mV and writable wet/dry calibration
Number entities.

Because this firmware adds endpoints 2-4 to the Zigbee device signature,
remove an older version of the device from ZHA and pair it again after flashing.
