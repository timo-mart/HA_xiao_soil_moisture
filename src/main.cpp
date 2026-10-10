// This is firmaware for Seed Studio Xiao Soil Mosture Sensor based on ESP32C6 communicating via Zigbee in End Device mode. 
// The device measures soil moisture and reports it to the coordinator, then goes to deep sleep to save battery. 
// When the device is plugged in, it reports more frequently and indicates the moisture level by LED. 
// The device can be woken up by a button press, which can be used for factory reset if the button is pressed for more than 10 seconds.

#include <Arduino.h>
#include <Preferences.h>
#include "esp_system.h"

#ifndef ZIGBEE_MODE_ED
#error "Zigbee end device mode is not selected in Tools->Zigbee mode"
#endif

#include "Zigbee.h"

#define USE_LED_ON_BATTERY 0 // when 1 LED blink shortly when reporting

#define BATTERY_CHEMISTRY_NIMH 1
#define BATTERY_CHEMISTRY_ALKALINE 2
#ifndef BATTERY_CHEMISTRY
#define BATTERY_CHEMISTRY BATTERY_CHEMISTRY_NIMH
#endif
#if BATTERY_CHEMISTRY != BATTERY_CHEMISTRY_NIMH && \
    BATTERY_CHEMISTRY != BATTERY_CHEMISTRY_ALKALINE
#error "BATTERY_CHEMISTRY must be NiMH (1) or alkaline (2)"
#endif

#define MOISTURE_ENDPOINT_NUMBER 1
#define RAW_MILLIVOLTS_ENDPOINT_NUMBER 2
#define WET_CALIBRATION_ENDPOINT_NUMBER 3
#define DRY_CALIBRATION_ENDPOINT_NUMBER 4
#define DRY_INTERVAL_ENDPOINT_NUMBER 5
#define MOIST_INTERVAL_ENDPOINT_NUMBER 6
#define DRY_THRESHOLD_ENDPOINT_NUMBER 7

#define BUTTON_PIN_BITMASK(GPIO) (1ULL << GPIO)  // 2 ^ GPIO_NUMBER in hex
#define WAKEUP_GPIO              GPIO_NUM_2 

#define uS_TO_S_FACTOR 1000000ULL /* Conversion factor for micro seconds to seconds */
#define REPORT_TIMEOUT 1000       /* Timeout for response from coordinator in ms */
#define COMMISSIONING_TIMEOUT 60000 /* Keep a cold-booted device reachable while ZHA interviews it */
#define COMMISSIONING_INTERVIEW_GRACE 10000 /* Let ZHA read descriptors and Basic attributes first */
#define COMMISSIONING_REPORT_INTERVAL 5000 /* Keep Zigbee traffic flowing while ZHA configures the device */

static constexpr uint32_t CONFIG_INACTIVITY_TIMEOUT_MS = 30000;
static constexpr uint32_t CONFIG_MAXIMUM_DURATION_MS = 120000;
static constexpr uint32_t CONFIG_SAMPLE_INTERVAL_MS = 5000;
static constexpr uint32_t ZIGBEE_CONNECTION_TIMEOUT_MS = 30000;
static constexpr uint8_t CONFIG_RECOVERY_RESTARTS = 1;
static constexpr uint32_t FACTORY_RESET_HOLD_MS = 10000;
static constexpr uint32_t FACTORY_RESET_LED_MS = 2000;
static constexpr uint32_t FACTORY_RESET_LED_OFF_MS = 250;

static constexpr char CONFIG_NAMESPACE[] = "soil_monitor";
static constexpr char CALIBRATION_KEY[] = "calibration";
static constexpr char SLEEP_POLICY_KEY[] = "sleep_policy";

static constexpr uint16_t CALIBRATION_VERSION = 1;
static constexpr uint16_t DEFAULT_WET_MV = 1200;
static constexpr uint16_t DEFAULT_DRY_MV = 2750;
static constexpr uint16_t MIN_CALIBRATION_MV = 100;
static constexpr uint16_t MAX_CALIBRATION_MV = 3300;
static constexpr uint16_t MIN_CALIBRATION_SPAN_MV = 100;

static constexpr uint16_t SLEEP_POLICY_VERSION = 1;
static constexpr uint16_t DEFAULT_DRY_INTERVAL_MINUTES = 15;
static constexpr uint16_t DEFAULT_MOIST_INTERVAL_MINUTES = 15;
static constexpr uint16_t DEFAULT_DRY_THRESHOLD_PERCENT = 45;
static constexpr uint16_t MIN_SLEEP_INTERVAL_MINUTES = 1;
static constexpr uint16_t MAX_SLEEP_INTERVAL_MINUTES = 7 * 24 * 60;
static constexpr uint16_t MIN_DRY_THRESHOLD_PERCENT = 0;
static constexpr uint16_t MAX_DRY_THRESHOLD_PERCENT = 100;

static constexpr uint8_t BATTERY_SAMPLE_COUNT = 8;
static constexpr uint16_t BATTERY_SAMPLE_DELAY_MS = 2;
static constexpr uint16_t BATTERY_PRESENT_MIN_MV = 600;

struct BatteryCurvePoint {
  uint16_t millivolts;
  uint8_t percentage;
};

#if BATTERY_CHEMISTRY == BATTERY_CHEMISTRY_NIMH
static constexpr char BATTERY_CHEMISTRY_NAME[] = "NiMH";
// Terminal voltage is only an estimate of NiMH state of charge. These points
// deliberately spread the flat discharge plateau across the useful range and
// can be refined later from logged field data without changing Zigbee state.
static constexpr BatteryCurvePoint BATTERY_CURVE[] = {
  {1350, 100},
  {1320, 90},
  {1290, 75},
  {1270, 60},
  {1250, 45},
  {1230, 30},
  {1200, 15},
  {1100, 5},
  {1000, 0},
};
#else
static constexpr char BATTERY_CHEMISTRY_NAME[] = "alkaline";
static constexpr BatteryCurvePoint BATTERY_CURVE[] = {
  {1550, 100},
  {1450, 90},
  {1375, 75},
  {1300, 60},
  {1230, 45},
  {1160, 30},
  {1080, 15},
  {1000, 5},
  {900, 0},
};
#endif

const uint8_t buttonPin = 2;
const uint8_t pwmPin = 21;
const uint8_t sensorPin = 1;
const uint8_t batteryPin = 0;
const uint8_t ledRedPin = 20;
const uint8_t ledGreenPin = 19;
const uint8_t ledYellowPin = 18;

// Arduino-ESP32 4.0.0-RC1's setPowerSource() tries to add the Basic cluster's
// PowerSource attribute even though the SDK 2.x default Basic cluster already
// contains it. The duplicate add fails before the Power Configuration cluster
// is created. Keep this workaround local and remove it when the core fixes the
// implementation.
class BatteryZigbeeAnalog : public ZigbeeAnalog {
public:
  using ZigbeeAnalog::ZigbeeAnalog;

  bool setBatteryPowerSource() {
    ezb_zcl_cluster_desc_t basic = getEpClusterDesc(EZB_ZCL_CLUSTER_ID_BASIC);
    ezb_zcl_attr_desc_t duplicate = ezb_zcl_cluster_remove_attr_desc(
      basic, EZB_ZCL_ATTR_BASIC_POWER_SOURCE_ID, 0);
    if (duplicate == EZB_INVALID_ZCL_ATTR_DESC) {
      return false;
    }
    ezb_zcl_free_attr_desc(duplicate);
    if (!setPowerSource(ZB_POWER_SOURCE_BATTERY)) {
      return false;
    }

    // Keep the battery entity unknown until the first real percentage report.
    _battery_percentage = 0xff;
    if (!setEpClusterAttrValue(
          EZB_ZCL_CLUSTER_ID_POWER_CONFIG,
          EZB_ZCL_CLUSTER_SERVER,
          EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID,
          &_battery_percentage)) {
      return false;
    }

    // BatteryVoltage is optional and not reportable through this Arduino API.
    // Omitting it is preferable to leaving ZHA with a stale or bogus value.
    ezb_zcl_cluster_desc_t power = getEpClusterDesc(EZB_ZCL_CLUSTER_ID_POWER_CONFIG);
    ezb_zcl_attr_desc_t voltage = ezb_zcl_cluster_remove_attr_desc(
      power, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID, 0);
    if (voltage == EZB_INVALID_ZCL_ATTR_DESC) {
      return false;
    }
    ezb_zcl_free_attr_desc(voltage);
    return true;
  }
};

BatteryZigbeeAnalog zbMoisture = BatteryZigbeeAnalog(MOISTURE_ENDPOINT_NUMBER);
ZigbeeAnalog zbRawMillivolts = ZigbeeAnalog(RAW_MILLIVOLTS_ENDPOINT_NUMBER);
ZigbeeAnalog zbWetCalibration = ZigbeeAnalog(WET_CALIBRATION_ENDPOINT_NUMBER);
ZigbeeAnalog zbDryCalibration = ZigbeeAnalog(DRY_CALIBRATION_ENDPOINT_NUMBER);
ZigbeeAnalog zbDryInterval = ZigbeeAnalog(DRY_INTERVAL_ENDPOINT_NUMBER);
ZigbeeAnalog zbMoistInterval = ZigbeeAnalog(MOIST_INTERVAL_ENDPOINT_NUMBER);
ZigbeeAnalog zbDryThreshold = ZigbeeAnalog(DRY_THRESHOLD_ENDPOINT_NUMBER);

enum class BootAction {
  Normal,
  Configure,
  FactoryReset,
};

static constexpr uint32_t RETAINED_CONFIG_MAGIC = 0x43464731;
RTC_NOINIT_ATTR volatile uint32_t retainedConfigMagic;
RTC_NOINIT_ATTR volatile uint32_t retainedConfigMagicInverse;
RTC_NOINIT_ATTR volatile uint32_t retainedConfigRestartCount;

bool retainedConfigRequestIsValid() {
  return retainedConfigMagic == RETAINED_CONFIG_MAGIC &&
         retainedConfigMagicInverse == ~RETAINED_CONFIG_MAGIC;
}

void retainConfigRequest(bool resetRestartCount) {
  retainedConfigMagic = RETAINED_CONFIG_MAGIC;
  retainedConfigMagicInverse = ~RETAINED_CONFIG_MAGIC;
  if (resetRestartCount) {
    retainedConfigRestartCount = 0;
  }
}

void clearRetainedConfigRequest() {
  retainedConfigMagic = 0;
  retainedConfigMagicInverse = 0;
  retainedConfigRestartCount = 0;
}

struct PendingCalibrationWrites {
  bool wetPending;
  bool dryPending;
  float wetMv;
  float dryMv;
};

struct PendingSleepPolicyWrites {
  bool dryIntervalPending;
  bool moistIntervalPending;
  bool dryThresholdPending;
  float dryIntervalMinutes;
  float moistIntervalMinutes;
  float dryThresholdPercent;
};

portMUX_TYPE calibrationWriteMux = portMUX_INITIALIZER_UNLOCKED;
PendingCalibrationWrites pendingCalibrationWrites = {};
volatile bool syncingCalibrationOutputs = false;
portMUX_TYPE sleepPolicyWriteMux = portMUX_INITIALIZER_UNLOCKED;
PendingSleepPolicyWrites pendingSleepPolicyWrites = {};
volatile bool syncingSleepPolicyOutputs = false;

bool batteryPowered = false;
bool commissioningSession = false;
bool configurationModeActive = false;
uint32_t configurationInactivityDeadline = 0;
uint32_t configurationAbsoluteDeadline = 0;
uint32_t nextConfigurationSample = 0;
uint32_t nextConfigurationLedToggle = 0;
bool configurationLedOn = true;

struct CalibrationRecord {
  uint16_t version;
  uint16_t wetMv;
  uint16_t dryMv;
};

CalibrationRecord activeCalibration = {
  CALIBRATION_VERSION,
  DEFAULT_WET_MV,
  DEFAULT_DRY_MV,
};

struct SleepPolicyRecord {
  uint16_t version;
  uint16_t dryIntervalMinutes;
  uint16_t moistIntervalMinutes;
  uint16_t dryThresholdPercent;
};

SleepPolicyRecord activeSleepPolicy = {
  SLEEP_POLICY_VERSION,
  DEFAULT_DRY_INTERVAL_MINUTES,
  DEFAULT_MOIST_INTERVAL_MINUTES,
  DEFAULT_DRY_THRESHOLD_PERCENT,
};

bool calibrationIsValid(const CalibrationRecord &calibration) {
  return calibration.version == CALIBRATION_VERSION &&
         calibration.wetMv >= MIN_CALIBRATION_MV &&
         calibration.dryMv <= MAX_CALIBRATION_MV &&
         calibration.dryMv > calibration.wetMv &&
         calibration.dryMv - calibration.wetMv >= MIN_CALIBRATION_SPAN_MV;
}

void loadCalibration() {
  Preferences preferences;
  if (!preferences.begin(CONFIG_NAMESPACE, false)) {
    return;
  }

  CalibrationRecord stored = {};
  const size_t storedSize = preferences.isKey(CALIBRATION_KEY)
    ? preferences.getBytesLength(CALIBRATION_KEY)
    : 0;
  const size_t bytesRead = storedSize == sizeof(stored)
    ? preferences.getBytes(CALIBRATION_KEY, &stored, sizeof(stored))
    : 0;
  preferences.end();

  if (bytesRead == sizeof(stored) && calibrationIsValid(stored)) {
    activeCalibration = stored;
  }
}

bool saveCalibration(uint16_t wetMv, uint16_t dryMv) {
  const CalibrationRecord proposed = {
    CALIBRATION_VERSION,
    wetMv,
    dryMv,
  };
  if (!calibrationIsValid(proposed)) {
    return false;
  }
  if (activeCalibration.wetMv == wetMv &&
      activeCalibration.dryMv == dryMv) {
    return true;
  }

  Preferences preferences;
  if (!preferences.begin(CONFIG_NAMESPACE, false)) {
    return false;
  }
  const size_t bytesWritten = preferences.putBytes(
    CALIBRATION_KEY, &proposed, sizeof(proposed));
  preferences.end();
  if (bytesWritten != sizeof(proposed)) {
    return false;
  }

  activeCalibration = proposed;
  return true;
}

bool sleepPolicyIsValid(const SleepPolicyRecord &policy) {
  return policy.version == SLEEP_POLICY_VERSION &&
         policy.dryIntervalMinutes >= MIN_SLEEP_INTERVAL_MINUTES &&
         policy.dryIntervalMinutes <= MAX_SLEEP_INTERVAL_MINUTES &&
         policy.moistIntervalMinutes >= MIN_SLEEP_INTERVAL_MINUTES &&
         policy.moistIntervalMinutes <= MAX_SLEEP_INTERVAL_MINUTES &&
         policy.dryIntervalMinutes <= policy.moistIntervalMinutes &&
         policy.dryThresholdPercent >= MIN_DRY_THRESHOLD_PERCENT &&
         policy.dryThresholdPercent <= MAX_DRY_THRESHOLD_PERCENT;
}

void loadSleepPolicy() {
  Preferences preferences;
  if (!preferences.begin(CONFIG_NAMESPACE, false)) {
    return;
  }

  SleepPolicyRecord stored = {};
  const size_t storedSize = preferences.isKey(SLEEP_POLICY_KEY)
    ? preferences.getBytesLength(SLEEP_POLICY_KEY)
    : 0;
  const size_t bytesRead = storedSize == sizeof(stored)
    ? preferences.getBytes(SLEEP_POLICY_KEY, &stored, sizeof(stored))
    : 0;
  preferences.end();

  if (bytesRead == sizeof(stored) && sleepPolicyIsValid(stored)) {
    activeSleepPolicy = stored;
  }
}

bool saveSleepPolicy(const SleepPolicyRecord &proposed) {
  if (!sleepPolicyIsValid(proposed)) {
    return false;
  }
  if (memcmp(&activeSleepPolicy, &proposed, sizeof(proposed)) == 0) {
    return true;
  }

  Preferences preferences;
  if (!preferences.begin(CONFIG_NAMESPACE, false)) {
    return false;
  }
  const size_t bytesWritten = preferences.putBytes(
    SLEEP_POLICY_KEY, &proposed, sizeof(proposed));
  preferences.end();
  if (bytesWritten != sizeof(proposed)) {
    return false;
  }

  activeSleepPolicy = proposed;
  return true;
}

bool deadlineReached(uint32_t deadline) {
  return (int32_t)(millis() - deadline) >= 0;
}

void queueWetCalibrationWrite(float wetMv) {
  if (syncingCalibrationOutputs) {
    return;
  }
  portENTER_CRITICAL(&calibrationWriteMux);
  pendingCalibrationWrites.wetMv = wetMv;
  pendingCalibrationWrites.wetPending = true;
  portEXIT_CRITICAL(&calibrationWriteMux);
}

void queueDryCalibrationWrite(float dryMv) {
  if (syncingCalibrationOutputs) {
    return;
  }
  portENTER_CRITICAL(&calibrationWriteMux);
  pendingCalibrationWrites.dryMv = dryMv;
  pendingCalibrationWrites.dryPending = true;
  portEXIT_CRITICAL(&calibrationWriteMux);
}

void queueDryIntervalWrite(float minutes) {
  if (syncingSleepPolicyOutputs) {
    return;
  }
  portENTER_CRITICAL(&sleepPolicyWriteMux);
  pendingSleepPolicyWrites.dryIntervalMinutes = minutes;
  pendingSleepPolicyWrites.dryIntervalPending = true;
  portEXIT_CRITICAL(&sleepPolicyWriteMux);
}

void queueMoistIntervalWrite(float minutes) {
  if (syncingSleepPolicyOutputs) {
    return;
  }
  portENTER_CRITICAL(&sleepPolicyWriteMux);
  pendingSleepPolicyWrites.moistIntervalMinutes = minutes;
  pendingSleepPolicyWrites.moistIntervalPending = true;
  portEXIT_CRITICAL(&sleepPolicyWriteMux);
}

void queueDryThresholdWrite(float percent) {
  if (syncingSleepPolicyOutputs) {
    return;
  }
  portENTER_CRITICAL(&sleepPolicyWriteMux);
  pendingSleepPolicyWrites.dryThresholdPercent = percent;
  pendingSleepPolicyWrites.dryThresholdPending = true;
  portEXIT_CRITICAL(&sleepPolicyWriteMux);
}

void syncCalibrationOutputs(bool report) {
  syncingCalibrationOutputs = true;
  zbWetCalibration.setAnalogOutput(activeCalibration.wetMv);
  zbDryCalibration.setAnalogOutput(activeCalibration.dryMv);
  syncingCalibrationOutputs = false;

  if (report) {
    zbWetCalibration.reportAnalogOutput();
    zbDryCalibration.reportAnalogOutput();
  }
}

void syncSleepPolicyOutputs(bool report) {
  syncingSleepPolicyOutputs = true;
  zbDryInterval.setAnalogOutput(activeSleepPolicy.dryIntervalMinutes);
  zbMoistInterval.setAnalogOutput(activeSleepPolicy.moistIntervalMinutes);
  zbDryThreshold.setAnalogOutput(activeSleepPolicy.dryThresholdPercent);
  syncingSleepPolicyOutputs = false;

  if (report) {
    zbDryInterval.reportAnalogOutput();
    zbMoistInterval.reportAnalogOutput();
    zbDryThreshold.reportAnalogOutput();
  }
}

void processPendingCalibrationWrites() {
  PendingCalibrationWrites writes = {};
  portENTER_CRITICAL(&calibrationWriteMux);
  writes = pendingCalibrationWrites;
  pendingCalibrationWrites.wetPending = false;
  pendingCalibrationWrites.dryPending = false;
  portEXIT_CRITICAL(&calibrationWriteMux);

  if (!writes.wetPending && !writes.dryPending) {
    return;
  }

  // A write means the user is still configuring the device. Extend the
  // inactivity window even when the proposed calibration is rejected.
  if (configurationModeActive) {
    configurationInactivityDeadline = millis() + CONFIG_INACTIVITY_TIMEOUT_MS;
  }

  int32_t wetMv = activeCalibration.wetMv;
  int32_t dryMv = activeCalibration.dryMv;
  if (writes.wetPending && isfinite(writes.wetMv)) {
    wetMv = lroundf(writes.wetMv);
  }
  if (writes.dryPending && isfinite(writes.dryMv)) {
    dryMv = lroundf(writes.dryMv);
  }

  const bool valuesFit =
    wetMv >= 0 && wetMv <= UINT16_MAX &&
    dryMv >= 0 && dryMv <= UINT16_MAX;
  const bool saved = valuesFit && saveCalibration(
    static_cast<uint16_t>(wetMv), static_cast<uint16_t>(dryMv));
  if (saved) {
    log_i("Calibration accepted: wet=%ld mV, dry=%ld mV", wetMv, dryMv);
  } else {
    log_w("Calibration rejected: wet=%ld mV, dry=%ld mV", wetMv, dryMv);
  }

  // Echo the accepted pair, or restore the previous pair after an invalid
  // write, so ZHA never displays a value the firmware did not persist.
  syncCalibrationOutputs(true);
}

void processPendingSleepPolicyWrites() {
  PendingSleepPolicyWrites writes = {};
  portENTER_CRITICAL(&sleepPolicyWriteMux);
  writes = pendingSleepPolicyWrites;
  pendingSleepPolicyWrites.dryIntervalPending = false;
  pendingSleepPolicyWrites.moistIntervalPending = false;
  pendingSleepPolicyWrites.dryThresholdPending = false;
  portEXIT_CRITICAL(&sleepPolicyWriteMux);

  if (!writes.dryIntervalPending && !writes.moistIntervalPending &&
      !writes.dryThresholdPending) {
    return;
  }

  if (configurationModeActive) {
    configurationInactivityDeadline = millis() + CONFIG_INACTIVITY_TIMEOUT_MS;
  }

  int32_t dryInterval = activeSleepPolicy.dryIntervalMinutes;
  int32_t moistInterval = activeSleepPolicy.moistIntervalMinutes;
  int32_t dryThreshold = activeSleepPolicy.dryThresholdPercent;
  if (writes.dryIntervalPending && isfinite(writes.dryIntervalMinutes)) {
    dryInterval = lroundf(writes.dryIntervalMinutes);
  }
  if (writes.moistIntervalPending && isfinite(writes.moistIntervalMinutes)) {
    moistInterval = lroundf(writes.moistIntervalMinutes);
  }
  if (writes.dryThresholdPending && isfinite(writes.dryThresholdPercent)) {
    dryThreshold = lroundf(writes.dryThresholdPercent);
  }

  const bool valuesFit =
    dryInterval >= 0 && dryInterval <= UINT16_MAX &&
    moistInterval >= 0 && moistInterval <= UINT16_MAX &&
    dryThreshold >= 0 && dryThreshold <= UINT16_MAX;
  const SleepPolicyRecord proposed = {
    SLEEP_POLICY_VERSION,
    static_cast<uint16_t>(dryInterval),
    static_cast<uint16_t>(moistInterval),
    static_cast<uint16_t>(dryThreshold),
  };
  const bool saved = valuesFit && saveSleepPolicy(proposed);
  if (saved) {
    log_i(
      "Sleep policy accepted: dry=%ld min, moist=%ld min, dry_below=%ld%%",
      dryInterval, moistInterval, dryThreshold);
  } else {
    log_w(
      "Sleep policy rejected: dry=%ld min, moist=%ld min, dry_below=%ld%%",
      dryInterval, moistInterval, dryThreshold);
  }

  syncSleepPolicyOutputs(true);
}

uint8_t batteryPercentageFromMillivolts(uint16_t millivolts) {
  if (millivolts >= BATTERY_CURVE[0].millivolts) {
    return BATTERY_CURVE[0].percentage;
  }

  const size_t pointCount = sizeof(BATTERY_CURVE) / sizeof(BATTERY_CURVE[0]);
  for (size_t index = 1; index < pointCount; index++) {
    const BatteryCurvePoint &upper = BATTERY_CURVE[index - 1];
    const BatteryCurvePoint &lower = BATTERY_CURVE[index];
    if (millivolts >= lower.millivolts) {
      const uint32_t voltageAboveLower = millivolts - lower.millivolts;
      const uint32_t voltageSpan = upper.millivolts - lower.millivolts;
      const uint32_t percentageSpan = upper.percentage - lower.percentage;
      return lower.percentage +
        (voltageAboveLower * percentageSpan + voltageSpan / 2) /
        voltageSpan;
    }
  }

  return BATTERY_CURVE[pointCount - 1].percentage;
}

struct BatteryReading {
  uint16_t millivolts;
  uint8_t percentage;
};

BatteryReading measureBattery() {
  uint32_t millivoltTotal = 0;
  for (uint8_t sample = 0; sample < BATTERY_SAMPLE_COUNT; sample++) {
    millivoltTotal += analogReadMilliVolts(batteryPin);
    if (sample + 1 < BATTERY_SAMPLE_COUNT) {
      delay(BATTERY_SAMPLE_DELAY_MS);
    }
  }

  const uint16_t millivolts = static_cast<uint16_t>(
    millivoltTotal / BATTERY_SAMPLE_COUNT);
  const uint8_t percentage = batteryPercentageFromMillivolts(millivolts);
  log_i(
    "Battery: chemistry=%s, calibrated=%u mV, calculated=%u%%",
    BATTERY_CHEMISTRY_NAME, millivolts, percentage);
  return {millivolts, percentage};
}

struct MoistureReading {
  bool valid;
  uint16_t millivolts;
  uint8_t percentage;
};

MoistureReading measureMoisture() {
  uint32_t millivoltTotal = 0;
  uint8_t validSamples = 0;
  for (uint8_t sample = 0; sample < 5; sample++) {
    const uint32_t millivolts = analogReadMilliVolts(sensorPin);
    if (millivolts > 0 && millivolts <= MAX_CALIBRATION_MV) {
      millivoltTotal += millivolts;
      validSamples++;
    }
  }

  if (validSamples == 0) {
    return {false, 0, 0};
  }

  const uint16_t millivolts = millivoltTotal / validSamples;
  const int32_t span = activeCalibration.dryMv - activeCalibration.wetMv;
  const int32_t moisture =
    (activeCalibration.dryMv - millivolts) * 100 / span;
  return {
    true,
    millivolts,
    static_cast<uint8_t>(constrain(moisture, 0, 100)),
  };
}

void clearMoistureLeds() {
  digitalWrite(ledRedPin, 0);
  digitalWrite(ledGreenPin, 0);
  digitalWrite(ledYellowPin, 0);
}

void configureButtonWakeup() {
  esp_sleep_enable_ext1_wakeup_io(
    BUTTON_PIN_BITMASK(WAKEUP_GPIO), ESP_EXT1_WAKEUP_ANY_LOW);
  gpio_pullup_dis(WAKEUP_GPIO);
  gpio_pulldown_dis(WAKEUP_GPIO);
}

BootAction determineBootAction() {
  const bool wokeFromButton =
    (esp_sleep_get_wakeup_causes() &
      (1UL << ESP_SLEEP_WAKEUP_EXT1)) != 0 &&
    (esp_sleep_get_ext1_wakeup_status() &
      BUTTON_PIN_BITMASK(WAKEUP_GPIO)) != 0;
  const bool buttonHeld = digitalRead(buttonPin) == LOW;
  if (!wokeFromButton && !buttonHeld) {
    return BootAction::Normal;
  }

  // Acknowledge the wake immediately. This happens before Zigbee startup, so
  // the user is not left waiting through a potentially slow parent rejoin.
  clearMoistureLeds();
  digitalWrite(ledYellowPin, 1);

  if (buttonHeld) {
    const uint32_t holdStarted = millis();
    while (digitalRead(buttonPin) == LOW) {
      if (millis() - holdStarted >= FACTORY_RESET_HOLD_MS) {
        clearMoistureLeds();
        digitalWrite(ledRedPin, 1);
        delay(FACTORY_RESET_LED_MS);
        digitalWrite(ledRedPin, 0);
        delay(FACTORY_RESET_LED_OFF_MS);
        log_i("Factory reset gesture accepted");
        return BootAction::FactoryReset;
      }
      delay(20);
    }
    delay(30);
  }

  clearMoistureLeds();
  digitalWrite(ledGreenPin, 1);
  log_i("Configuration gesture accepted");
  return BootAction::Configure;
}

void indicateMoisture(const MoistureReading &moisture) {
  if (!moisture.valid || moisture.percentage <= 40) {
    digitalWrite(ledRedPin, 1);
  } else if (moisture.percentage <= 50) {
    digitalWrite(ledYellowPin, 1);
  } else {
    digitalWrite(ledGreenPin, 1);
  }
}

bool reportMoisture(const MoistureReading &moisture) {
  return !moisture.valid || zbMoisture.reportAnalogInput();
}

bool reportRawMillivolts(const MoistureReading &moisture) {
  return !moisture.valid || zbRawMillivolts.reportAnalogInput();
}

void updateRawMillivoltsAttribute(const MoistureReading &moisture) {
  if (moisture.valid) {
    zbRawMillivolts.setAnalogInput(moisture.millivolts);
  }
}

void updateMoistureAttributes(const MoistureReading &moisture) {
  if (!moisture.valid) {
    return;
  }
  zbMoisture.setAnalogInput(moisture.percentage);
  updateRawMillivoltsAttribute(moisture);
}

void reportBatteryAndMoisture(const MoistureReading &moisture) {
  zbMoisture.reportBatteryPercentage();
  if (moisture.valid) {
    zbMoisture.reportAnalogInput();
    zbRawMillivolts.reportAnalogInput();
  }
}

void primeZigbeeAttributes() {
  // Populate readable values as soon as the endpoint descriptors exist, before
  // ZHA can begin its interview immediately after the network connection.
  const MoistureReading moisture = measureMoisture();
  updateMoistureAttributes(moisture);
  if (batteryPowered) {
    zbMoisture.setBatteryPercentage(measureBattery().percentage);
  }
  syncCalibrationOutputs(false);
  syncSleepPolicyOutputs(false);
}

void runCommissioningReportWindow(
    const MoistureReading &moisture, uint32_t reportDeadline) {
  vTaskDelay(COMMISSIONING_INTERVIEW_GRACE / portTICK_PERIOD_MS);

  while ((int32_t)(reportDeadline - millis()) > 0) {
    // Regular reports keep a sleepy device exchanging traffic with its
    // parent while ZHA binds clusters and configures attribute reporting.
    reportBatteryAndMoisture(moisture);

    const int32_t remaining = reportDeadline - millis();
    if (remaining <= 0) {
      break;
    }
    const uint32_t delayMs = min(
      (uint32_t)remaining, (uint32_t)COMMISSIONING_REPORT_INTERVAL);
    vTaskDelay(delayMs / portTICK_PERIOD_MS);
  }

  // Publish the final values after ZHA has had the full interview window.
  reportBatteryAndMoisture(moisture);
}

void retryMoistureReport(
    const MoistureReading &moisture, uint32_t reportDeadline, bool sent) {
  while (!sent && (int32_t)(reportDeadline - millis()) > 0) {
    vTaskDelay(500 / portTICK_PERIOD_MS);
    sent = reportMoisture(moisture);
  }
}

void scheduleNextWake(const MoistureReading &moisture) {
  const bool plantIsDry =
    !moisture.valid ||
    moisture.percentage < activeSleepPolicy.dryThresholdPercent;
  const uint32_t sleepMinutes = plantIsDry
    ? activeSleepPolicy.dryIntervalMinutes
    : activeSleepPolicy.moistIntervalMinutes;
  log_i(
    "Sleep policy: moisture=%u%%, dry_below=%u%%, next_wake=%lu min",
    moisture.percentage, activeSleepPolicy.dryThresholdPercent,
    static_cast<unsigned long>(sleepMinutes));
  esp_sleep_enable_timer_wakeup(
    static_cast<uint64_t>(sleepMinutes) * 60ULL * uS_TO_S_FACTOR);
}

/************************ function called when on battery *****************************/
static void measureAndSleep(void *arg) {
  bool sent = false;

  const uint8_t batteryPercentage = measureBattery().percentage;
  const MoistureReading moisture = measureMoisture();
  analogWrite(pwmPin, 0);

  updateMoistureAttributes(moisture);

  zbMoisture.setBatteryPercentage(batteryPercentage);
  if (!commissioningSession) {
    zbMoisture.reportBatteryPercentage();
    sent = reportMoisture(moisture);
    reportRawMillivolts(moisture);
  }

#if USE_LED_ON_BATTERY
  indicateMoisture(moisture);
#endif

  uint32_t reportDeadline = millis() +
    (commissioningSession ? COMMISSIONING_TIMEOUT : REPORT_TIMEOUT);
  if (commissioningSession) {
    runCommissioningReportWindow(moisture, reportDeadline);
  } else {
    retryMoistureReport(moisture, reportDeadline, sent);
  }
  vTaskDelay(500 / portTICK_PERIOD_MS); // wait for a while to let the data be sent before going to sleep, otherwise the device may go to sleep before sending the data and the data will be lost

#if USE_LED_ON_BATTERY
  clearMoistureLeds();
#endif
  
  // Put device to deep sleep based on moisture level, if the moisture is below the threshold, the device will sleep for shorter time to report more frequently, otherwise it will sleep for longer time to save battery
  scheduleNextWake(moisture);
  esp_deep_sleep_start();
}

/************************ function called when plugged in *****************************/
static void measure(void *arg) {
  while(true){
    const MoistureReading moisture = measureMoisture();

    // indicate moisture level by LED when plugged in (all the time on, not only when reporting)
    clearMoistureLeds();

    if (moisture.valid) {
      updateMoistureAttributes(moisture);
      indicateMoisture(moisture);
      zbMoisture.reportAnalogInput();
      zbRawMillivolts.reportAnalogInput();
    }

    vTaskDelay(5000 / portTICK_PERIOD_MS); // wait for 5s before measuring again, the reporting interval is not critical when the device is plugged in, so we can afford to report more frequently to get more accurate data
  }
}

void initializeHardware() {
  pinMode(ledRedPin, OUTPUT);
  pinMode(ledGreenPin, OUTPUT);
  pinMode(ledYellowPin, OUTPUT);

  analogWriteFrequency(pwmPin, 200000);
  analogWriteResolution(pwmPin, 2);
  analogWrite(pwmPin, 2);

  // Use the ESP32-C6's factory-calibrated full-range ADC conversion for both
  // analog channels. Arduino calls this attenuation ADC_11db; ESP-IDF names
  // the same setting ADC_ATTEN_DB_12.
  analogSetPinAttenuation(sensorPin, ADC_11db);
  analogSetPinAttenuation(batteryPin, ADC_11db);

  loadCalibration();
  loadSleepPolicy();
  pinMode(buttonPin, INPUT);
}

void indicateZigbeeStartFailure() {
  for (uint8_t blink = 0; blink < 3; blink++) {
    digitalWrite(ledRedPin, 1);
    delay(500);
    digitalWrite(ledRedPin, 0);
    delay(500);
  }
}

bool initializeZigbee(bool eraseZigbeeStorage, bool receiverOnWhenIdle) {
  // Arduino-ESP32 4 selects and initializes the Zigbee role before endpoint
  // configuration. Use its default 3-second end-device keep-alive so a sleepy
  // device polls its parent promptly during commissioning.
  esp_zigbee_device_config_t zigbeeConfig = ZIGBEE_DEFAULT_ED_CONFIG();
  if (!Zigbee.role(&zigbeeConfig, eraseZigbeeStorage)) {
    log_e("Failed to initialize Zigbee role");
    return false;
  }
  Zigbee.setRxOnWhenIdle(receiverOnWhenIdle);

  zbMoisture.setManufacturerAndModel("Seed Studio", "Xiao Soil Moisture Sensor");
  zbMoisture.addAnalogInput();
  zbMoisture.setAnalogInputApplication(EZB_ZCL_AI_PERCENTAGE_OTHER);
  zbMoisture.setAnalogInputDescription("Soil Moisture");
  zbMoisture.setAnalogInputResolution(1);

  zbRawMillivolts.setManufacturerAndModel("Seed Studio", "Xiao Soil Moisture Sensor");
  zbRawMillivolts.addAnalogInput();
  zbRawMillivolts.setAnalogInputApplication(0x000C0000);
  zbRawMillivolts.setAnalogInputDescription("Soil Sensor Voltage");
  zbRawMillivolts.setAnalogInputResolution(1);
  zbRawMillivolts.setAnalogInputMinMax(
    MIN_CALIBRATION_MV, MAX_CALIBRATION_MV);

  zbWetCalibration.setManufacturerAndModel("Seed Studio", "Xiao Soil Moisture Sensor");
  zbWetCalibration.addAnalogOutput();
  zbWetCalibration.setAnalogOutputApplication(0x000C0000);
  zbWetCalibration.setAnalogOutputDescription("Wet Calibration");
  zbWetCalibration.setAnalogOutputResolution(1);
  zbWetCalibration.setAnalogOutputMinMax(
    MIN_CALIBRATION_MV, MAX_CALIBRATION_MV);
  zbWetCalibration.onAnalogOutputChange(queueWetCalibrationWrite);

  zbDryCalibration.setManufacturerAndModel("Seed Studio", "Xiao Soil Moisture Sensor");
  zbDryCalibration.addAnalogOutput();
  zbDryCalibration.setAnalogOutputApplication(0x000C0000);
  zbDryCalibration.setAnalogOutputDescription("Dry Calibration");
  zbDryCalibration.setAnalogOutputResolution(1);
  zbDryCalibration.setAnalogOutputMinMax(
    MIN_CALIBRATION_MV, MAX_CALIBRATION_MV);
  zbDryCalibration.onAnalogOutputChange(queueDryCalibrationWrite);

  zbDryInterval.setManufacturerAndModel("Seed Studio", "Xiao Soil Moisture Sensor");
  zbDryInterval.addAnalogOutput();
  zbDryInterval.setAnalogOutputApplication(EZB_ZCL_AO_TIME_OTHER);
  zbDryInterval.setAnalogOutputDescription("Dry Reporting Interval");
  zbDryInterval.setAnalogOutputResolution(1);
  zbDryInterval.setAnalogOutputMinMax(
    MIN_SLEEP_INTERVAL_MINUTES, MAX_SLEEP_INTERVAL_MINUTES);
  zbDryInterval.onAnalogOutputChange(queueDryIntervalWrite);

  zbMoistInterval.setManufacturerAndModel("Seed Studio", "Xiao Soil Moisture Sensor");
  zbMoistInterval.addAnalogOutput();
  zbMoistInterval.setAnalogOutputApplication(EZB_ZCL_AO_TIME_OTHER);
  zbMoistInterval.setAnalogOutputDescription("Moist Reporting Interval");
  zbMoistInterval.setAnalogOutputResolution(1);
  zbMoistInterval.setAnalogOutputMinMax(
    MIN_SLEEP_INTERVAL_MINUTES, MAX_SLEEP_INTERVAL_MINUTES);
  zbMoistInterval.onAnalogOutputChange(queueMoistIntervalWrite);

  zbDryThreshold.setManufacturerAndModel("Seed Studio", "Xiao Soil Moisture Sensor");
  zbDryThreshold.addAnalogOutput();
  zbDryThreshold.setAnalogOutputApplication(EZB_ZCL_AO_PERCENTAGE_OTHER);
  zbDryThreshold.setAnalogOutputDescription("Dry Threshold");
  zbDryThreshold.setAnalogOutputResolution(1);
  zbDryThreshold.setAnalogOutputMinMax(
    MIN_DRY_THRESHOLD_PERCENT, MAX_DRY_THRESHOLD_PERCENT);
  zbDryThreshold.onAnalogOutputChange(queueDryThresholdWrite);

  // Keep the Zigbee identity stable regardless of the current power source.
  // Battery measurements are populated later when running on battery.
  if (!zbMoisture.setBatteryPowerSource()) {
    log_e("Failed to configure Zigbee battery cluster");
    return false;
  }

  Zigbee.addEndpoint(&zbMoisture);
  Zigbee.addEndpoint(&zbRawMillivolts);
  Zigbee.addEndpoint(&zbWetCalibration);
  Zigbee.addEndpoint(&zbDryCalibration);
  Zigbee.addEndpoint(&zbDryInterval);
  Zigbee.addEndpoint(&zbMoistInterval);
  Zigbee.addEndpoint(&zbDryThreshold);
  Zigbee.setTimeout(10000);

  if (!Zigbee.begin()) {
    log_e("Failed to start Zigbee stack");
    return false;
  }
  primeZigbeeAttributes();

  const uint32_t connectionDeadline =
    millis() + ZIGBEE_CONNECTION_TIMEOUT_MS;
  while (!Zigbee.connected() && !deadlineReached(connectionDeadline)) {
    delay(100);
  }
  if (!Zigbee.connected()) {
    log_e("Timed out waiting for Zigbee connection");
    return false;
  }
  return true;
}

void handleZigbeeStartupFailure(BootAction bootAction) {
  indicateZigbeeStartFailure();

  if (batteryPowered &&
      bootAction == BootAction::Configure &&
      retainedConfigRequestIsValid() &&
      retainedConfigRestartCount < CONFIG_RECOVERY_RESTARTS) {
    const uint32_t nextRestartCount = retainedConfigRestartCount + 1;
    retainedConfigRestartCount = nextRestartCount;
    log_w(
      "Restarting once to recover configuration mode (attempt %lu)",
      static_cast<unsigned long>(nextRestartCount));
    delay(250);
    ESP.restart();
  }

  clearRetainedConfigRequest();
  if (batteryPowered) {
    // Never leave a battery device stuck awake in a failed connection loop.
    // A subsequent short press retries with the receiver continuously on.
    log_w("Zigbee unavailable; sleeping until the next button press");
    analogWrite(pwmPin, 0);
    clearMoistureLeds();
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    configureButtonWakeup();
    esp_deep_sleep_start();
  }

  delay(1000);
  ESP.restart();
}

void startMeasurementTask() {
  if (batteryPowered) {
    configureButtonWakeup();
    xTaskCreate(measureAndSleep, "sensor_update", 2048, NULL, 10, NULL);
  } else {
    xTaskCreate(measure, "sensor_update", 2048, NULL, 10, NULL);
  }
}

void startConfigurationMode() {
  clearRetainedConfigRequest();
  configurationModeActive = true;
  const uint32_t now = millis();
  configurationInactivityDeadline = now + CONFIG_INACTIVITY_TIMEOUT_MS;
  configurationAbsoluteDeadline = now + CONFIG_MAXIMUM_DURATION_MS;
  nextConfigurationSample = now + CONFIG_SAMPLE_INTERVAL_MS;
  nextConfigurationLedToggle = now + 500;
  configurationLedOn = true;
  log_i("Configuration mode ready");

  const MoistureReading moisture = measureMoisture();
  const uint8_t batteryPercentage = measureBattery().percentage;
  updateMoistureAttributes(moisture);
  zbMoisture.setBatteryPercentage(batteryPercentage);
  reportBatteryAndMoisture(moisture);
  syncCalibrationOutputs(true);
  syncSleepPolicyOutputs(true);
}

void finishConfigurationMode() {
  log_i("Configuration mode finished");
  // Publish one final measurement using the calibration that was accepted
  // during this window. The one-second deep-sleep hop below then restores the
  // ordinary sleepy-device boot path and sends the same values once more.
  const MoistureReading moisture = measureMoisture();
  updateMoistureAttributes(moisture);
  zbMoisture.setBatteryPercentage(measureBattery().percentage);
  reportBatteryAndMoisture(moisture);
  syncCalibrationOutputs(true);
  syncSleepPolicyOutputs(true);
  delay(500);

  analogWrite(pwmPin, 0);
  clearMoistureLeds();
  configurationModeActive = false;
  configureButtonWakeup();
  esp_sleep_enable_timer_wakeup(1 * uS_TO_S_FACTOR);
  esp_deep_sleep_start();
}

void runConfigurationMode() {
  const uint32_t now = millis();
  if (deadlineReached(configurationInactivityDeadline) ||
      deadlineReached(configurationAbsoluteDeadline)) {
    finishConfigurationMode();
    return;
  }

  if (deadlineReached(nextConfigurationSample)) {
    nextConfigurationSample = now + CONFIG_SAMPLE_INTERVAL_MS;
    const MoistureReading moisture = measureMoisture();
    updateRawMillivoltsAttribute(moisture);
    reportRawMillivolts(moisture);
  }

  if (deadlineReached(nextConfigurationLedToggle)) {
    configurationLedOn = !configurationLedOn;
    digitalWrite(ledGreenPin, configurationLedOn);
    nextConfigurationLedToggle = now + 500;
  }
}

/********************* Arduino functions **************************/
void setup() {
  initializeHardware();
  const BatteryReading startupBattery = measureBattery();
  batteryPowered = startupBattery.millivolts >= BATTERY_PRESENT_MIN_MV;
  BootAction bootAction = determineBootAction();
  const bool recoveringConfigRequest =
    bootAction == BootAction::Normal &&
    batteryPowered &&
    esp_reset_reason() == ESP_RST_SW &&
    retainedConfigRequestIsValid();
  if (recoveringConfigRequest) {
    bootAction = BootAction::Configure;
    clearMoistureLeds();
    digitalWrite(ledGreenPin, 1);
    log_i("Recovering configuration mode after software restart");
  }

  if (bootAction == BootAction::Configure && batteryPowered) {
    retainConfigRequest(!recoveringConfigRequest);
  } else {
    clearRetainedConfigRequest();
  }

  const bool eraseZigbeeStorage = bootAction == BootAction::FactoryReset;
  const bool commissioningBoot = esp_reset_reason() != ESP_RST_DEEPSLEEP;
  commissioningSession = commissioningBoot || eraseZigbeeStorage;
  const bool receiverOnWhenIdle =
    !batteryPowered || commissioningSession || bootAction != BootAction::Normal;

  log_i(
    "Boot mode: power=%s, action=%u, rx_on_when_idle=%s",
    batteryPowered ? "battery" : "usb",
    static_cast<unsigned>(bootAction),
    receiverOnWhenIdle ? "true" : "false");

  if (!initializeZigbee(eraseZigbeeStorage, receiverOnWhenIdle)) {
    handleZigbeeStartupFailure(bootAction);
    return;
  }

  if (bootAction == BootAction::Configure && batteryPowered) {
    startConfigurationMode();
  } else {
    startMeasurementTask();
  }
}

void loop() {
  processPendingCalibrationWrites();
  processPendingSleepPolicyWrites();
  if (configurationModeActive) {
    runConfigurationMode();
  }
  delay(20);
}
