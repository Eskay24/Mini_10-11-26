/*
  ESP32 climate controller: BME280 + MOSFET fan + ultrasonic mist maker
  - Wakes from deep sleep every SLEEP_MINUTES
  - Reads BME280 temperature + humidity (forced mode, low self-heating)
  - Temp too high    -> fan burst, re-check, repeat (capped)
  - Humidity too low -> mist burst, re-check, repeat (capped)
  - Goes back to deep sleep with both outputs OFF

  Library (Arduino Library Manager): "Adafruit BME280 Library" (+ "Adafruit Unified Sensor")

  Wiring (default pins, change below):
    BME280 SDA -> GPIO21, SCL -> GPIO22, VCC -> 3V3, GND -> GND
    FAN MOSFET gate  -> GPIO25 (+ 10k pulldown gate->GND)
    MIST MOSFET gate -> GPIO26 (+ 10k pulldown gate->GND)
  Use logic-level MOSFETs (e.g. IRLZ44N, AO3400) and a flyback diode across the fan.
  The mist maker's driver board is switched on its supply (low side) by the MOSFET.
*/

#include <Wire.h>
#include <Adafruit_BME280.h>
#include "esp_sleep.h"

// ---------------- User settings ----------------
const uint32_t SLEEP_MINUTES   = 5;      // x: time between checks
const float    TEMP_MAX_C      = 28.0;   // y: fan runs above this
const float    TEMP_HYST_C     = 0.5;    // fan stops being needed below (TEMP_MAX_C - hyst)
const float    HUM_MIN_PCT     = 55.0;   // z: mist runs below this
const float    HUM_HYST_PCT    = 3.0;    // mist not needed above (HUM_MIN_PCT + hyst)

const uint32_t FAN_BURST_S     = 20;     // fan on-time per burst
const uint32_t MIST_BURST_S    = 10;     // mist on-time per burst
const uint32_t SETTLE_S        = 15;     // wait after a burst so the air mixes before re-reading
const uint8_t  MAX_BURSTS      = 6;      // safety cap per output, per wake cycle

const gpio_num_t FAN_PIN  = GPIO_NUM_25;
const gpio_num_t MIST_PIN = GPIO_NUM_26;
const int SDA_PIN = 21;
const int SCL_PIN = 22;
const uint8_t BME_ADDR = 0x76;           // use 0x77 if your module's SDO is tied high
// ------------------------------------------------

Adafruit_BME280 bme;
bool bmeOk = false;

bool initBME() {
  if (!bme.begin(BME_ADDR, &Wire)) return false;
  // Forced mode: sensor measures once on request, then sleeps (no self-heating).
  bme.setSampling(Adafruit_BME280::MODE_FORCED,
                  Adafruit_BME280::SAMPLING_X1,    // temperature
                  Adafruit_BME280::SAMPLING_NONE,  // pressure (unused)
                  Adafruit_BME280::SAMPLING_X1,    // humidity
                  Adafruit_BME280::FILTER_OFF);
  return true;
}

bool readClimate(float &t, float &h) {
  if (!bmeOk) return false;
  for (int i = 0; i < 3; i++) {
    if (bme.takeForcedMeasurement()) {
      t = bme.readTemperature();
      h = bme.readHumidity();
      if (!isnan(t) && !isnan(h)) return true;
    }
    delay(100);
  }
  return false;
}

void outputsOff() {
  digitalWrite(FAN_PIN, LOW);
  digitalWrite(MIST_PIN, LOW);
}

void burst(gpio_num_t pin, uint32_t seconds) {
  digitalWrite(pin, HIGH);
  delay(seconds * 1000UL);
  digitalWrite(pin, LOW);
}

void goToSleep() {
  outputsOff();
  Serial.printf("Sleeping %lu min\n", (unsigned long)SLEEP_MINUTES);
  Serial.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_MINUTES * 60ULL * 1000000ULL);
  esp_deep_sleep_start();
}

void setup() {
  pinMode(FAN_PIN, OUTPUT);
  pinMode(MIST_PIN, OUTPUT);
  outputsOff();

  Serial.begin(115200);
  delay(100);
  Wire.begin(SDA_PIN, SCL_PIN);
  bmeOk = initBME();

  float t, h;
  if (!readClimate(t, h)) {
    Serial.println("BME280 read failed - skipping actuation");
    goToSleep();
  }
  Serial.printf("Wake: %.2f C, %.1f %%RH\n", t, h);

  // ---- Cooling ----
  if (t > TEMP_MAX_C) {
    for (uint8_t i = 1; i <= MAX_BURSTS; i++) {
      Serial.printf("Fan burst %u/%u\n", i, MAX_BURSTS);
      burst(FAN_PIN, FAN_BURST_S);
      delay(SETTLE_S * 1000UL);
      if (!readClimate(t, h)) { Serial.println("Sensor fail, stopping fan"); break; }
      Serial.printf("  now %.2f C, %.1f %%RH\n", t, h);
      if (t <= TEMP_MAX_C - TEMP_HYST_C) break;   // back in range
    }
  }

  // ---- Humidifying ----
  if (h < HUM_MIN_PCT) {
    for (uint8_t i = 1; i <= MAX_BURSTS; i++) {
      Serial.printf("Mist burst %u/%u\n", i, MAX_BURSTS);
      burst(MIST_PIN, MIST_BURST_S);
      delay(SETTLE_S * 1000UL);
      if (!readClimate(t, h)) { Serial.println("Sensor fail, stopping mist"); break; }
      Serial.printf("  now %.2f C, %.1f %%RH\n", t, h);
      if (h >= HUM_MIN_PCT + HUM_HYST_PCT) break;  // back in range
    }
  }

  goToSleep();
}

void loop() {}  // never reached; each wake restarts setup()
