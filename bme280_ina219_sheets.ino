/*
  Arduino UNO R4 WiFi -> Google Sheets, fully autonomous
    - BME280 temperature + humidity every 5 minutes (found at I2C 0x76, chip ID 0x60)
    - INA219 bus voltage / current / power every 30 minutes

  I2C: SDA = A4, SCL = A5 (Wire)  OR  the Qwiic connector (Wire1). The sketch scans both
  buses at boot and uses whichever one each sensor answers on.

  Libraries (Library Manager, accept dependencies):
    "Adafruit BME280 Library"
    "Adafruit INA219"
  Secrets go in arduino_secrets.h (template at the bottom).
  The Apps Script (Code.gs) is unchanged from the SHT30 + INA219 version.
*/

#include <WiFiS3.h>
#include <WiFiSSLClient.h>
#include <Wire.h>
#include <Adafruit_BME280.h>
#include <Adafruit_INA219.h>
#include "arduino_secrets.h"

// ---------- Settings ----------
const uint32_t CLIMATE_INTERVAL = 5UL * 60UL * 1000UL;    // 5 minutes
const uint32_t POWER_INTERVAL   = 30UL * 60UL * 1000UL;   // 30 minutes
const uint8_t  MAX_FAILS_RESET  = 5;
const int      POWER_SAMPLES    = 100;

const char HOST[] = "script.google.com";

Adafruit_BME280 bme;
Adafruit_INA219 ina219;
WiFiSSLClient   client;

TwoWire *bmeWire = nullptr;
TwoWire *inaWire = nullptr;
uint32_t lastClimate = 0, lastPower = 0;
uint8_t  failCount = 0;

// ---------- I2C helpers ----------
bool present(TwoWire &w, uint8_t addr) {
  w.beginTransmission(addr);
  return w.endTransmission() == 0;
}

void scanBus(TwoWire &w, const char *label) {
  Serial.print("I2C scan "); Serial.print(label); Serial.print(": ");
  bool any = false;
  for (uint8_t a = 1; a < 127; a++) {
    if (present(w, a)) {
      Serial.print("0x"); Serial.print(a, HEX); Serial.print(" ");
      any = true;
    }
  }
  Serial.println(any ? "" : "(nothing found)");
}

bool detectBme() {
  TwoWire *buses[2] = { &Wire, &Wire1 };
  const uint8_t addrs[2] = { 0x76, 0x77 };
  for (int b = 0; b < 2; b++)
    for (int a = 0; a < 2; a++)
      if (present(*buses[b], addrs[a]) && bme.begin(addrs[a], buses[b])) {
        // Forced mode: measure only when asked, avoids self-heating skewing the temperature
        bme.setSampling(Adafruit_BME280::MODE_FORCED,
                        Adafruit_BME280::SAMPLING_X1,    // temperature
                        Adafruit_BME280::SAMPLING_NONE,  // pressure (not used)
                        Adafruit_BME280::SAMPLING_X1,    // humidity
                        Adafruit_BME280::FILTER_OFF);
        bmeWire = buses[b];
        Serial.print("BME280 found at 0x"); Serial.print(addrs[a], HEX);
        Serial.println(b == 0 ? " on Wire (A4/A5)" : " on Wire1 (Qwiic)");
        return true;
      }
  return false;
}

bool detectIna() {
  TwoWire *buses[2] = { &Wire, &Wire1 };
  for (int b = 0; b < 2; b++)
    if (present(*buses[b], 0x40) && ina219.begin(buses[b])) {
      inaWire = buses[b];
      Serial.println(b == 0 ? "INA219 found on Wire (A4/A5)" : "INA219 found on Wire1 (Qwiic)");
      return true;
    }
  return false;
}

// ---------- WiFi ----------
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.print("Connecting to WiFi");
  WiFi.disconnect();
  for (int attempt = 0; attempt < 5 && WiFi.status() != WL_CONNECTED; attempt++) {
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
      delay(500);
      Serial.print(".");
    }
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? " connected" : " FAILED");
}

// ---------- Send to Google Sheet ----------
bool sendQuery(const String &query) {
  String path = String("/macros/s/") + SCRIPT_ID + "/exec?key=" + SHARED_KEY + "&" + query;

  client.stop();
  if (!client.connect(HOST, 443)) {
    Serial.println("TLS connect failed");
    return false;
  }
  client.print(String("GET ") + path + " HTTP/1.1\r\n"
             + "Host: " + HOST + "\r\n"
             + "Connection: close\r\n\r\n");

  uint32_t t0 = millis();
  while (!client.available() && millis() - t0 < 15000) delay(10);
  String status = client.readStringUntil('\n');
  Serial.print("Response: "); Serial.println(status);

  bool redirectOk = false;
  if (status.indexOf("302") > 0) {
    while (client.available() || client.connected()) {
      String h = client.readStringUntil('\n');
      if (h.startsWith("Location:") || h.startsWith("location:")) {
        redirectOk = h.indexOf("googleusercontent.com") > 0;
        break;
      }
      if (h == "\r") break;
    }
  }
  client.stop();
  return status.indexOf("200") > 0 || redirectOk;
}

// ---------- Sensors ----------
bool sendClimate() {
  if (!bmeWire && !detectBme()) {
    Serial.println("BME280 not found on any I2C bus (see scan above)");
    return false;
  }
  float t = NAN, h = NAN;
  for (int i = 0; i < 3; i++) {
    if (bme.takeForcedMeasurement()) {
      t = bme.readTemperature();
      h = bme.readHumidity();
      if (!isnan(t) && !isnan(h)) break;
    }
    delay(200);
  }
  if (isnan(t) || isnan(h)) {
    Serial.println("BME280 read failed");
    bmeWire = nullptr;               // force re-detect next time
    return false;
  }
  Serial.print("Temp "); Serial.print(t, 2); Serial.print(" C, Hum "); Serial.print(h, 1); Serial.println(" %");
  return sendQuery(String("type=climate&temp=") + String(t, 2) + "&hum=" + String(h, 1));
}

bool sendPower() {
  if (!inaWire && !detectIna()) {
    Serial.println("INA219 not found on any I2C bus (see scan above)");
    return false;
  }
  double sumV = 0, sumMA = 0, sumMW = 0;
  for (int i = 0; i < POWER_SAMPLES; i++) {
    sumV  += ina219.getBusVoltage_V();
    sumMA += ina219.getCurrent_mA();
    sumMW += ina219.getPower_mW();
    delay(10);
  }
  float v  = sumV  / POWER_SAMPLES;
  float ma = sumMA / POWER_SAMPLES;
  float mw = sumMW / POWER_SAMPLES;
  Serial.print("Bus "); Serial.print(v, 3); Serial.print(" V, ");
  Serial.print(ma, 2); Serial.print(" mA, ");
  Serial.print(mw, 2); Serial.println(" mW");
  return sendQuery(String("type=power&v=") + String(v, 3) + "&ma=" + String(ma, 2) + "&mw=" + String(mw, 2));
}

void handleResult(bool ok) {
  if (ok) {
    failCount = 0;
    Serial.println("Sheet updated");
  } else {
    failCount++;
    Serial.print("Send failed ("); Serial.print(failCount); Serial.println(")");
    if (failCount >= MAX_FAILS_RESET) {
      Serial.println("Too many failures, rebooting");
      delay(200);
      NVIC_SystemReset();
    }
  }
}

// ---------- Arduino ----------
void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 5000) {}   // wait up to 5 s for the monitor, then carry on

  Wire.begin();   Wire.setClock(100000);
  Wire1.begin();  Wire1.setClock(100000);

  scanBus(Wire,  "Wire  (A4/A5)");
  scanBus(Wire1, "Wire1 (Qwiic)");
  if (!detectBme()) Serial.println("BME280 NOT FOUND (expected 0x76 or 0x77 in the scan)");
  if (!detectIna()) Serial.println("INA219 NOT FOUND (expected 0x40 in the scan)");

  connectWiFi();

  lastClimate = millis() - CLIMATE_INTERVAL;
  lastPower   = millis() - POWER_INTERVAL;
}

void loop() {
  uint32_t now = millis();

  if (now - lastClimate >= CLIMATE_INTERVAL) {
    lastClimate = now;
    connectWiFi();
    handleResult(WiFi.status() == WL_CONNECTED && sendClimate());
  }

  if (now - lastPower >= POWER_INTERVAL) {
    lastPower = now;
    connectWiFi();
    handleResult(WiFi.status() == WL_CONNECTED && sendPower());
  }
}

/* ---------- arduino_secrets.h (create as a second tab) ----------
#define WIFI_SSID   "your-wifi-name"
#define WIFI_PASS   "your-wifi-password"
#define SCRIPT_ID   "AKfycb...your-DEPLOYMENT-id..."
#define SHARED_KEY  "pick-any-random-string"
*/
