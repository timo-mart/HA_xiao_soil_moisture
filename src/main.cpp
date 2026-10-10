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

#define ENDPOINT_NUMBER 1

#define BUTTON_PIN_BITMASK(GPIO) (1ULL << GPIO)  // 2 ^ GPIO_NUMBER in hex
#define WAKEUP_GPIO              GPIO_NUM_2 

#define uS_TO_S_FACTOR 1000000ULL /* Conversion factor for micro seconds to seconds */
#define TIME_TO_SLEEP_SHORT  900         /* Sleep for 15min */
#define TIME_TO_SLEEP_LONG  900       /* Test cadence: also wake every 15min when soil is moist */
#define TIME_TO_SLEEP_TRESHOLD 45 /* moisture percentage below witch is period of reporting more frequent */
#define REPORT_TIMEOUT 1000       /* Timeout for response from coordinator in ms */
#define COMMISSIONING_TIMEOUT 60000 /* Keep a cold-booted device reachable while ZHA interviews it */
#define COMMISSIONING_INTERVIEW_GRACE 10000 /* Let ZHA read descriptors and Basic attributes first */
#define COMMISSIONING_REPORT_INTERVAL 5000 /* Keep Zigbee traffic flowing while ZHA configures the device */

static constexpr char CONFIG_NAMESPACE[] = "soil_monitor";
static constexpr char CALIBRATION_KEY[] = "calibration";

static constexpr uint16_t CALIBRATION_VERSION = 1;
static constexpr uint16_t DEFAULT_WET_MV = 1200;
static constexpr uint16_t DEFAULT_DRY_MV = 2750;
static constexpr uint16_t MIN_CALIBRATION_MV = 100;
static constexpr uint16_t MAX_CALIBRATION_MV = 3300;
static constexpr uint16_t MIN_CALIBRATION_SPAN_MV = 100;

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

BatteryZigbeeAnalog zbMoisture = BatteryZigbeeAnalog(ENDPOINT_NUMBER);

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

uint8_t measureBatteryVoltage(){
  uint8_t voltage = analogRead(batteryPin)/100;
  return voltage;
}
uint8_t measureBatteryPercentage(){
  uint8_t percentage;
  int voltage = analogRead(batteryPin);
  if( (voltage < 1100) && (voltage > 100) ) percentage = 0;
  else if(voltage < 1500 && (voltage > 100) ) percentage = (voltage-1100)*100/400;
  else percentage = 100;
  return percentage;
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

void reportBatteryAndMoisture(const MoistureReading &moisture) {
  zbMoisture.reportBatteryPercentage();
  if (moisture.valid) {
    zbMoisture.reportAnalogInput();
  }
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
  const uint32_t sleepSeconds =
    !moisture.valid || moisture.percentage < TIME_TO_SLEEP_TRESHOLD
      ? TIME_TO_SLEEP_SHORT
      : TIME_TO_SLEEP_LONG;
  esp_sleep_enable_timer_wakeup(sleepSeconds * uS_TO_S_FACTOR);
}

/************************ function called when on battery *****************************/
static void measureAndSleep(void *arg) {
  bool sent = false;
  esp_reset_reason_t resetReason = esp_reset_reason();
  bool commissioningBoot = resetReason != ESP_RST_DEEPSLEEP;

  uint8_t batteryPercentage = measureBatteryPercentage();
  if (batteryPercentage == 0 && !commissioningBoot) {
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    esp_deep_sleep_start();
  }
  const MoistureReading moisture = measureMoisture();
  analogWrite(pwmPin, 0);

  if (moisture.valid) {
    zbMoisture.setAnalogInput(moisture.percentage);
  }

  zbMoisture.setBatteryPercentage(batteryPercentage);
  if (!commissioningBoot) {
    zbMoisture.reportBatteryPercentage();
    sent = reportMoisture(moisture);
  }

#if USE_LED_ON_BATTERY
  indicateMoisture(moisture);
#endif

  uint32_t reportDeadline = millis() + (commissioningBoot ? COMMISSIONING_TIMEOUT : REPORT_TIMEOUT);
  if (commissioningBoot) {
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
      zbMoisture.setAnalogInput(moisture.percentage);
      indicateMoisture(moisture);
      zbMoisture.reportAnalogInput();
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

void initializeZigbee() {
  // Arduino-ESP32 4 selects and initializes the Zigbee role before endpoint
  // configuration. Use its default 3-second end-device keep-alive so a sleepy
  // device polls its parent promptly during commissioning.
  esp_zigbee_device_config_t zigbeeConfig = ZIGBEE_DEFAULT_ED_CONFIG();
  if (!Zigbee.role(&zigbeeConfig, false)) {
    ESP.restart();
  }
  Zigbee.setRxOnWhenIdle(false);

  zbMoisture.setManufacturerAndModel("Seed Studio", "Xiao Soil Moisture Sensor");
  zbMoisture.addAnalogInput();
  zbMoisture.setAnalogInputApplication(EZB_ZCL_AI_PERCENTAGE_OTHER);
  zbMoisture.setAnalogInputDescription("Soil Moisture");
  zbMoisture.setAnalogInputResolution(1);

  // Keep the Zigbee identity stable regardless of the current power source.
  // Battery measurements are populated later when running on battery.
  if (!zbMoisture.setBatteryPowerSource()) {
    ESP.restart();
  }

  Zigbee.addEndpoint(&zbMoisture);
  Zigbee.setTimeout(10000);

  if (!Zigbee.begin()) {
    indicateZigbeeStartFailure();
    ESP.restart();
  }
  while (!Zigbee.connected()) {
    delay(100);
  }
}

void startMeasurementTask() {
  if (measureBatteryVoltage() > 1) {
    esp_sleep_enable_ext1_wakeup_io(
      BUTTON_PIN_BITMASK(WAKEUP_GPIO), ESP_EXT1_WAKEUP_ANY_LOW);
    gpio_pullup_dis(WAKEUP_GPIO);
    gpio_pulldown_dis(WAKEUP_GPIO);
    xTaskCreate(measureAndSleep, "sensor_update", 2048, NULL, 10, NULL);
  } else {
    xTaskCreate(measure, "sensor_update", 2048, NULL, 10, NULL);
  }
}

/********************* Arduino functions **************************/
void setup() {
  initializeHardware();
  initializeZigbee();
  startMeasurementTask();
}

void loop() {
  // Checking button for factory reset
  if (digitalRead(buttonPin) == LOW) {  // Push button pressed
    // Key debounce handling
    delay(100);
    int startTime = millis();
    while (digitalRead(buttonPin) == LOW) {
      delay(50);
      if ((millis() - startTime) > 10000) {
        // If key pressed for more than 10secs, factory reset Zigbee and reboot
        delay(1000);
        // Optional set reset in factoryReset to false, to not restart device after erasing nvram, but set it to endless sleep manually instead
        //Zigbee.factoryReset(false);
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
        esp_deep_sleep_start();
      }
    }
  }
  delay(100);
}
