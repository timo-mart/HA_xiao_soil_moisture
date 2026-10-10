"""ZHA entities for the XIAO Zigbee soil moisture sensor."""

from zigpy.zcl.clusters.general import AnalogInput, AnalogOutput

from zhaquirks.builder import QuirkBuilder
from zhaquirks.clusters import CustomCluster


class RawSoilVoltageAnalogInput(CustomCluster, AnalogInput):
    """Expose endpoint 2 as a read-only millivolt sensor."""

    _CONSTANT_ATTRIBUTES = {
        AnalogInput.AttributeDefs.description.id: "Soil Sensor Voltage",
        AnalogInput.AttributeDefs.engineering_units.id: 124,
        AnalogInput.AttributeDefs.min_present_value.id: 100.0,
        AnalogInput.AttributeDefs.max_present_value.id: 3300.0,
        AnalogInput.AttributeDefs.resolution.id: 1.0,
    }

    def get(self, key, default=None):
        """Let ZHA derive the unit from engineering_units instead of count."""

        if key in (
            AnalogInput.AttributeDefs.application_type.id,
            AnalogInput.AttributeDefs.application_type.name,
        ):
            return None
        return super().get(key, default)


class WetCalibrationAnalogOutput(CustomCluster, AnalogOutput):
    """Expose endpoint 3 as the writable wet calibration point."""

    _CONSTANT_ATTRIBUTES = {
        AnalogOutput.AttributeDefs.description.id: "Wet Calibration",
        AnalogOutput.AttributeDefs.application_type.id: 0x010CFFFF,
        AnalogOutput.AttributeDefs.engineering_units.id: 124,
        AnalogOutput.AttributeDefs.min_present_value.id: 100.0,
        AnalogOutput.AttributeDefs.max_present_value.id: 3300.0,
        AnalogOutput.AttributeDefs.resolution.id: 1.0,
    }


class DryCalibrationAnalogOutput(CustomCluster, AnalogOutput):
    """Expose endpoint 4 as the writable dry calibration point."""

    _CONSTANT_ATTRIBUTES = {
        AnalogOutput.AttributeDefs.description.id: "Dry Calibration",
        AnalogOutput.AttributeDefs.application_type.id: 0x010CFFFF,
        AnalogOutput.AttributeDefs.engineering_units.id: 124,
        AnalogOutput.AttributeDefs.min_present_value.id: 100.0,
        AnalogOutput.AttributeDefs.max_present_value.id: 3300.0,
        AnalogOutput.AttributeDefs.resolution.id: 1.0,
    }


class DryReportingIntervalAnalogOutput(CustomCluster, AnalogOutput):
    """Expose endpoint 5 as the dry-soil reporting interval."""

    _CONSTANT_ATTRIBUTES = {
        AnalogOutput.AttributeDefs.description.id: "Dry Reporting Interval",
        AnalogOutput.AttributeDefs.application_type.id: 0x010EFFFF,
        AnalogOutput.AttributeDefs.engineering_units.id: 72,
        AnalogOutput.AttributeDefs.min_present_value.id: 1.0,
        AnalogOutput.AttributeDefs.max_present_value.id: 10080.0,
        AnalogOutput.AttributeDefs.resolution.id: 1.0,
    }


class MoistReportingIntervalAnalogOutput(CustomCluster, AnalogOutput):
    """Expose endpoint 6 as the moist-soil reporting interval."""

    _CONSTANT_ATTRIBUTES = {
        AnalogOutput.AttributeDefs.description.id: "Moist Reporting Interval",
        AnalogOutput.AttributeDefs.application_type.id: 0x010EFFFF,
        AnalogOutput.AttributeDefs.engineering_units.id: 72,
        AnalogOutput.AttributeDefs.min_present_value.id: 1.0,
        AnalogOutput.AttributeDefs.max_present_value.id: 10080.0,
        AnalogOutput.AttributeDefs.resolution.id: 1.0,
    }


class DryThresholdAnalogOutput(CustomCluster, AnalogOutput):
    """Expose endpoint 7 as the dry-soil percentage threshold."""

    _CONSTANT_ATTRIBUTES = {
        AnalogOutput.AttributeDefs.description.id: "Dry Threshold",
        AnalogOutput.AttributeDefs.application_type.id: 0x0104FFFF,
        AnalogOutput.AttributeDefs.engineering_units.id: 98,
        AnalogOutput.AttributeDefs.min_present_value.id: 0.0,
        AnalogOutput.AttributeDefs.max_present_value.id: 100.0,
        AnalogOutput.AttributeDefs.resolution.id: 1.0,
    }


(
    QuirkBuilder("Seed Studio", "Xiao Soil Moisture Sensor")
    .replaces(RawSoilVoltageAnalogInput, endpoint_id=2)
    .replaces(WetCalibrationAnalogOutput, endpoint_id=3)
    .replaces(DryCalibrationAnalogOutput, endpoint_id=4)
    .replaces(DryReportingIntervalAnalogOutput, endpoint_id=5)
    .replaces(MoistReportingIntervalAnalogOutput, endpoint_id=6)
    .replaces(DryThresholdAnalogOutput, endpoint_id=7)
    .add_to_registry()
)
