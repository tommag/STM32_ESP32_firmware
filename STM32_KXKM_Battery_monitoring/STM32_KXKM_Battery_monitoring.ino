/*
KXKM - ESP32 audio & battery module
STM32 coprocessor - battery monitoring function

The processor is in charge of the following tasks :
  * Keep the 3.3V enable line high on power up (self power)
  * Control the ESP32 enable line and the main load switch
  * Monitor the input voltage and shut down the whole board (3.3V, load switch, ESP32) on low battery
  * Push button monitoring :
    * on short press, display the battery level on the LED gauge
    * on long press, shut down the whole board
  * Communication with the ESP32 processor :
    * battery level reporting
    * custom battery profile input
    * push button reporting
    * warning before shut down (e.g. to prevent SD card corruption)
    * display arbitrary data on the LED gauge

The processor serial port is available on the ESP32 programmation connector. RX & TX must be swapped.

Tom Magnier - 04/2018
*/

//TODO Watchdog
//TODO state machine : release bouton, activation esp, activation load sw, attente section critique, etc
//TODO display single led when on

#include <AceButton.h>
#include "KXKM_STM32_energy_API.h"

// Hardware definitions
const uint8_t LED_PINS[] = {4,3,2,1};
const uint8_t POWER_ENABLE_PIN = 12; //Self power enable. Keep HIGH to stay powered
const uint8_t MAIN_OUT_ENABLE_PIN = 6; //Load switch enable line
const uint8_t ESP32_ENABLE_PIN = 7; //ESP32 enable line
const uint8_t PUSH_BUTTON_DETECT_PIN = 0; //Main On/off push button
const uint8_t BATT_TYPE_SELECTOR_PINS[] = {10,11}; //3-way selector
const uint8_t LOAD_CURRENT_SENSE_PIN = A0; //Load switch current measurement
const uint8_t BATT_VOLTAGE_SENSE_PIN = A1; //Battery voltage measurement
const uint8_t ESP32_TX_PIN = 8;

const uint8_t LED_ORDERING[] = {1,0,3,5,4,2};

// Timing configuration
const unsigned long STARTUP_GUARD_TIME_MS = 5000; // Ignore long presses during this period after startup
const unsigned long BATT_DISPLAY_TIME_MS = 3000; // Display the battery level during this time then shut down the LEDs

ace_button::AceButton button(PUSH_BUTTON_DETECT_PIN, LOW);
KXKM_STM32_Energy::PushButtonEvent buttonEvent = KXKM_STM32_Energy::NO_EVENT;
unsigned long battLevelDisplayStart;

#define SERIAL_DEBUG(str) \
  beginSerial(); \
  Serial1.println(str); \
  endSerial();

void setup() {
  pinMode(POWER_ENABLE_PIN, OUTPUT);
  pinMode(ESP32_ENABLE_PIN, OUTPUT);
  pinMode(MAIN_OUT_ENABLE_PIN, OUTPUT);
  pinMode(PUSH_BUTTON_DETECT_PIN, INPUT);

  for (int i = 0; i < 2; i++)
    pinMode(BATT_TYPE_SELECTOR_PINS[i], INPUT_PULLUP);

  pinMode(ESP32_TX_PIN, INPUT); // Switch TX to High Z (shared with ESP32 programmation connector)

  Serial1.setTimeout(10);

  ace_button::ButtonConfig* buttonConfig = button.getButtonConfig();
  buttonConfig->setEventHandler(handleButtonEvent);
  buttonConfig->setFeature(ace_button::ButtonConfig::kFeatureClick);
  buttonConfig->setFeature(ace_button::ButtonConfig::kFeatureDoubleClick);
  buttonConfig->setFeature(ace_button::ButtonConfig::kFeatureLongPress);

  // To keep interactions consistent, a long press is required to start up the board.
  // If the MCU is still powered at the end of the delay, we can move along.
  delay(button.getButtonConfig()->getLongPressDelay());

  initBatteryMonitoring();

  //TODO check battery voltage & shut down if too low

  // Power up the board
  set3V3RegState(true); //Keep 3.3V regulator enabled
  setLoadSwitchState(true); //TODO after 2s if no disable command has been received
  setESP32State(true); //Enable ESP32

  initLedGauge();

  // Start up LED animation
  for (uint8_t i = 0; i <= 100; i++)
  {
    setLedGaugePercentage(i);
    delay(4);
  }
  clearLeds();
}

void loop()
{
  button.check();
  loopBatteryMonitoring();

  // Display the battery level if battery is low or the push button has been pressed
  if ((millis() - battLevelDisplayStart > 0 && millis() - battLevelDisplayStart < BATT_DISPLAY_TIME_MS) || (getBatteryPercentage() > 0 && getBatteryPercentage() < 10))
    displayBatteryLevel(getBatteryPercentage());
  else if (millis() - battLevelDisplayStart > BATT_DISPLAY_TIME_MS)
    clearLeds();

  //TODO shutdown if battery level is too low
}


void handleButtonEvent(ace_button::AceButton* button, uint8_t eventType, uint8_t buttonState) {
  switch (eventType) {
    case ace_button::AceButton::kEventClicked:
      //Display the battery percentage on the LED gauge
      battLevelDisplayStart = millis();
      buttonEvent = KXKM_STM32_Energy::BUTTON_CLICK_EVENT;
      break;

    case ace_button::AceButton::kEventDoubleClicked:
    buttonEvent = KXKM_STM32_Energy::BUTTON_DOUBLE_CLICK_EVENT;
      break;

    case ace_button::AceButton::kEventLongPressed:
      if (millis() > STARTUP_GUARD_TIME_MS)
      {
        //Shut down LED animation
        for (int i = 100; i >= 0; i--)
        {
          setLedGaugePercentage(i);
          delay(4);
        }

        digitalWrite(MAIN_OUT_ENABLE_PIN, LOW);
        digitalWrite(ESP32_ENABLE_PIN, LOW); //TODO send warning to ESP32 and wait for confirmation
        pinMode(POWER_ENABLE_PIN, INPUT);
      }
      break;
  }
}
