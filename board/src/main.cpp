#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include "secrets.h"

// ---------- Settings to check before building ----------
#define WIFI_SSID     "Wokwi-GUEST"
#define WIFI_PASSWORD ""
#define API_URL       "http://iot-rquelch.spaincentral.cloudapp.azure.com"  // your VM, no trailing slash
#define MQTT_HOST     "135.232.203.144"                                    // class broker
#define MQTT_PORT     1883
#define MQTT_NAME     "richard2003"   // same <name> as Tutorial 9 and MQTT_NAME in api/.env

// ---------- Pins (from diagram.json) ----------
#define DHT_PIN 15
#define FAN_PIN 2
#define DHT_TYPE DHT22

const unsigned long READING_INTERVAL_MS = 10000;
const unsigned long MQTT_RETRY_MS = 3000;

const String TOPIC_SETTINGS = String("ecse3038/") + MQTT_NAME + "/fan/settings";
const String TOPIC_STATE    = String("ecse3038/") + MQTT_NAME + "/fan/state";
const String TOPIC_STATUS   = String("ecse3038/") + MQTT_NAME + "/status";

DHT dht(DHT_PIN, DHT_TYPE);
WiFiClient mqttNet;
PubSubClient mqtt(mqttNet);

String mac;

// Fan settings: none until the first valid message arrives
bool   haveSettings = false;
String mode = "off";
float  maxHumidity = 0;

// Latest humidity
bool  haveHumidity = false;
float humidity = 0;

// Fan output
bool fanOn = false;
int publishedState = -1;   // what fan/state last said: -1 nothing yet, 0 off, 1 on

unsigned long lastReading = 0;
unsigned long lastMqttTry = 0;
bool firstReading = true;

// ---------- Fan logic (B4, B5) ----------

void applyFan() {
  bool want = false;
  if (haveSettings) {
    if (mode == "on") {
      want = true;
    } else if (mode == "auto") {
      want = haveHumidity && humidity > maxHumidity;
    }
  }

  fanOn = want;
  digitalWrite(FAN_PIN, fanOn ? HIGH : LOW);

  // Publish only when the state differs from what was last published
  if (mqtt.connected() && publishedState != (int)fanOn) {
    mqtt.publish(TOPIC_STATE.c_str(), fanOn ? "on" : "off", true);
    publishedState = fanOn;
    Serial.printf("Fan -> %s\n", fanOn ? "on" : "off");
  }
}

// ---------- Settings messages (B3) ----------

void onMessage(char *topic, byte *payload, unsigned int length) {
  if (TOPIC_SETTINGS != topic) return;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.printf("Settings refused: not JSON (%s)\n", err.c_str());
    return;
  }
  if (!doc["mode"].is<const char *>() || !doc["max_humidity"].is<float>()) {
    Serial.println("Settings refused: field missing or wrong type");
    return;
  }

  String newMode = doc["mode"].as<const char *>();
  if (newMode != "auto" && newMode != "on" && newMode != "off") {
    Serial.printf("Settings refused: bad mode '%s'\n", newMode.c_str());
    return;
  }

  mode = newMode;
  maxHumidity = doc["max_humidity"].as<float>();
  haveSettings = true;
  Serial.printf("Settings: mode=%s max_humidity=%.1f\n", mode.c_str(), maxHumidity);

  applyFan();   // react now, not at the next reading
}

// ---------- WiFi and MQTT (B2) ----------

void connectWiFi() {
  Serial.print("WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD, 6);
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
  }
  Serial.println(" connected");
}

void connectMqtt() {
  String clientId = String("ecse3038-") + MQTT_NAME + "-" + mac;
  clientId.replace(":", "");

  Serial.print("MQTT connecting... ");
  // Last will: "offline", retained, on status
  if (mqtt.connect(clientId.c_str(), TOPIC_STATUS.c_str(), 1, true, "offline")) {
    Serial.println("connected");
    mqtt.publish(TOPIC_STATUS.c_str(), "online", true);
    mqtt.subscribe(TOPIC_SETTINGS.c_str(), 1);
    applyFan();   // catch up fan/state if it changed while offline
  } else {
    Serial.printf("failed, rc=%d\n", mqtt.state());
  }
}

// ---------- Readings (B1) ----------

void postReading(float h) {
  if (WiFi.status() != WL_CONNECTED) return;

  JsonDocument body;
  body["mac"] = mac;
  body["humidity"] = h;
  String json;
  serializeJson(body, json);

  HTTPClient http;
  http.begin(String(API_URL) + "/readings");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", DEVICE_KEY);
  int code = http.POST(json);
  Serial.printf("POST /readings %s -> %d\n", json.c_str(), code);
  if (code == 401) {
    Serial.println("401: register this MAC with POST /fans and put the key in include/secrets.h");
  }
  http.end();
}

void takeReading() {
  float h = dht.readHumidity();
  if (isnan(h)) {
    Serial.println("DHT22 read failed");
    return;
  }
  humidity = h;
  haveHumidity = true;
  postReading(h);
  applyFan();
}

// ---------- Arduino ----------

void setup() {
  Serial.begin(115200);
  pinMode(FAN_PIN, OUTPUT);
  digitalWrite(FAN_PIN, LOW);   // fan off until settings arrive
  dht.begin();

  connectWiFi();
  mac = WiFi.macAddress();
  Serial.println("MAC: " + mac);

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMessage);
  connectMqtt();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  if (!mqtt.connected()) {
    unsigned long now = millis();
    if (now - lastMqttTry >= MQTT_RETRY_MS) {
      lastMqttTry = now;
      connectMqtt();
    }
  } else {
    mqtt.loop();
  }

  unsigned long now = millis();
  if (firstReading || now - lastReading >= READING_INTERVAL_MS) {
    firstReading = false;
    lastReading = now;
    takeReading();
  }
}
