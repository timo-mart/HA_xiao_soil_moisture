// This is firmaware for Seed Studio Xiao Soil Mosture Sensor based on ESP32C6 communicating via Zigbee in End Device mode. 
// The device measures soil moisture and reports it to the coordinator, then goes to deep sleep to save battery. 
// When the device is plugged in, it reports more frequently and indicates the moisture level by LED. 
// The device can be woken up by a button press, which can be used for factory reset if the button is pressed for more than 10 seconds.

#include <Arduino.h>

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

#define WET_VOLTAGE 1600 // voltage at full water saturation
#define DRY_VOLTAGE 2550 // voltage at dry soil
#define BATTERY_FACTOR 0 // how much battery charge affects the voltage measurement in water, 0 means no effect

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
  if(percentage == 0){
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    esp_deep_sleep_start();
  }
  return percentage;
}

uint8_t measureMoisturePercentage(int wetVoltage = WET_VOLTAGE) {
  uint32_t adcTotal = 0;
  for (uint8_t sample = 0; sample < 5; sample++) {
    adcTotal += analogRead(sensorPin);
  }

  int32_t adc = adcTotal / 5;
  int32_t span = DRY_VOLTAGE - wetVoltage;
  if (span <= 0) {
    return 0;
  }
  int32_t moisture = (DRY_VOLTAGE - adc) * 100 / span;
  return constrain(moisture, 0, 100);
}

/************************ function called when on battery *****************************/
static void measureAndSleep(void *arg) {
  bool sent;

  uint8_t batteryPercentage = measureBatteryPercentage();
  int wetVoltage = WET_VOLTAGE - BATTERY_FACTOR * (100 - batteryPercentage);
  uint8_t moisture = measureMoisturePercentage(wetVoltage);
  analogWrite(pwmPin, 0);

  zbMoisture.setAnalogInput(moisture);

  zbMoisture.setBatteryPercentage(batteryPercentage);
  zbMoisture.reportBatteryPercentage();

  sent = zbMoisture.reportAnalogInput();  // report moisture as percentage value

#if USE_LED_ON_BATTERY
  if(moisture > 50) digitalWrite(ledGreenPin, 1);
  else if(moisture > 40) digitalWrite(ledYellowPin, 1);
  else digitalWrite(ledRedPin, 1);
#endif

  uint8_t tryies = 3;
  while(!sent && tryies != 0){
    sent = zbMoisture.reportAnalogInput();
    tryies--;
  }
  vTaskDelay(500 / portTICK_PERIOD_MS); // wait for a while to let the data be sent before going to sleep, otherwise the device may go to sleep before sending the data and the data will be lost

#if USE_LED_ON_BATTERY
  digitalWrite(ledRedPin, 0);
  digitalWrite(ledGreenPin, 0);
  digitalWrite(ledYellowPin, 0);
#endif
  
  // Put device to deep sleep based on moisture level, if the moisture is below the threshold, the device will sleep for shorter time to report more frequently, otherwise it will sleep for longer time to save battery
  if(moisture < TIME_TO_SLEEP_TRESHOLD) esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP_SHORT * uS_TO_S_FACTOR);
  else esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP_LONG * uS_TO_S_FACTOR);
  esp_deep_sleep_start();
}

/************************ function called when plugged in *****************************/
static void measure(void *arg) {
  while(true){
    uint8_t moisture = measureMoisturePercentage();

    zbMoisture.setAnalogInput(moisture);
    
    // indicate moisture level by LED when plugged in (all the time on, not only when reporting)
    digitalWrite(ledRedPin, 0);
    digitalWrite(ledGreenPin, 0);
    digitalWrite(ledYellowPin, 0);

    if(moisture > 50) digitalWrite(ledGreenPin, 1);
    else if(moisture > 40) digitalWrite(ledYellowPin, 1);
    else digitalWrite(ledRedPin, 1); 

    zbMoisture.reportAnalogInput();

    vTaskDelay(5000 / portTICK_PERIOD_MS); // wait for 5s before measuring again, the reporting interval is not critical when the device is plugged in, so we can afford to report more frequently to get more accurate data
  }
}

/********************* Arduino functions **************************/
void setup() {
  
  pinMode(ledRedPin, OUTPUT);
  pinMode(ledGreenPin, OUTPUT);
  pinMode(ledYellowPin, OUTPUT);

  analogWriteFrequency(pwmPin, 200000);
  analogWriteResolution(pwmPin, 2);
  analogWrite(pwmPin, 2);

  // Init button switch
  pinMode(buttonPin, INPUT);

  // Arduino-ESP32 4 selects and initializes the Zigbee role before endpoint
  // configuration. Keep the same 10-second end-device keep-alive as the
  // original firmware and Espressif's sleepy-sensor example.
  esp_zigbee_device_config_t zigbeeConfig = ZIGBEE_DEFAULT_ED_CONFIG();
  zigbeeConfig.zed_config.keep_alive = 10000;
  if (!Zigbee.role(&zigbeeConfig, false)) {
    ESP.restart();
  }

  // Optional: set Zigbee device name and model + add cluster for moisture sensor
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

  // Add endpoint to Zigbee Core
  Zigbee.addEndpoint(&zbMoisture);

  // For battery powered devices, it can be better to set timeout for Zigbee Begin to lower value to save battery
  // If the timeout has been reached, the network channel mask will be reset and the device will try to connect again after reset (scanning all channels)
  Zigbee.setTimeout(10000);  // Set timeout for Zigbee Begin to 10s (default is 30s)

  // When all EPs are registered, start Zigbee in End Device mode
  if (!Zigbee.begin()) {
    digitalWrite(ledRedPin, 1);
    delay(500);
    digitalWrite(ledRedPin, 0);
    delay(500);
    digitalWrite(ledRedPin, 1);
    delay(500);
    digitalWrite(ledRedPin, 0);
    delay(500);
    digitalWrite(ledRedPin, 1);
    delay(500);
    digitalWrite(ledRedPin, 0);
    ESP.restart();  // If Zigbee failed to start, reboot the device and try again after couple blinks of red LED to indicate the error
  }
  while (!Zigbee.connected()) {
    delay(100);
  }

  // setup wakeup button for battery powered device and start measurement task according to power source
  if(measureBatteryVoltage() > 1){
    esp_sleep_enable_ext1_wakeup_io(BUTTON_PIN_BITMASK(WAKEUP_GPIO), ESP_EXT1_WAKEUP_ANY_LOW);
    // Configure pullup/downs via RTCIO to tie wakeup pins to inactive level during deepsleep.
    // EXT0 resides in the same power domain (RTC_PERIPH) as the RTC IO pullup/downs.
    // No need to keep that power domain explicitly, unlike EXT1.
    gpio_pullup_dis(WAKEUP_GPIO);
    gpio_pulldown_dis(WAKEUP_GPIO);

    xTaskCreate(measureAndSleep, "sensor_update", 2048, NULL, 10, NULL);
  }
  else xTaskCreate(measure, "sensor_update", 2048, NULL, 10, NULL);
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
