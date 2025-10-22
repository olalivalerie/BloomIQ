#define TINY_GSM_MODEM_SIM800

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <TinyGsmClient.h>
#include <PubSubClient.h>
#include "DHT.h"

// ========== Configuration ==========
#define DHTPIN 4
#define DHTTYPE DHT11
#define RELAY_PIN 26
#define THINGSBOARD_SERVER "mqtt.thingsboard.cloud"
#define TOKEN "a9mNdTy4vMdiLgwnul0L"   // ← Replace with your actual ThingsBoard device token

// Wi-Fi credentials
const char* ssid = "AIZEN";
const char* password = "12345678";

// Phone number to receive SMS alerts
const char* alertNumber = "+254743474123"; // ← Change to your preferred recipient number

// SIM800L setup
#define MODEM_TX 27
#define MODEM_RX 26
#define MODEM_PWRKEY 4
#define MODEM_RST 5
#define MODEM_POWER_ON 23
#define SerialAT Serial1

// Objects
DHT dht(DHTPIN, DHTTYPE);
LiquidCrystal_I2C lcd(0x27, 16, 2);
WiFiClient wifiClient;
TinyGsm modem(SerialAT);
TinyGsmClient gsmClient(modem);
PubSubClient mqttClient;

// Variables
bool pumpState = false;
unsigned long lastSend = 0;
const unsigned long interval = 30000;  // send every 30s for testing
bool wifiConnected = false;
bool gsmConnected = false;

// ========== LCD Helper ==========
void lcdStatus(const String &line1, const String &line2 = "") {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
  Serial.println(line1 + " " + line2);
}

// ========== MQTT Callback (Handle RPC from ThingsBoard) ==========
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message;
  for (uint8_t i = 0; i < length; i++) message += (char)payload[i];

  Serial.print("Incoming message: ");
  Serial.println(message);

  // Parse pump RPC
  if (message.indexOf("\"method\":\"setPump\"") != -1) {
    bool newState = (message.indexOf("true") != -1);
    pumpState = newState;
    digitalWrite(RELAY_PIN, pumpState ? LOW : HIGH);
    lcdStatus(pumpState ? "Pump: ON" : "Pump: OFF", "Remote toggle");

    // Acknowledge attribute update
    String response = "{\"pump\":" + String(pumpState ? "true" : "false") + "}";
    mqttClient.publish("v1/devices/me/attributes", response.c_str());
  }
}

// ========== MQTT Connect ==========
bool connectMQTT() {
  mqttClient.setServer(THINGSBOARD_SERVER, 1883);
  mqttClient.setCallback(mqttCallback);

  lcdStatus("Connecting MQTT...");
  Serial.print("Connecting to MQTT broker: ");
  Serial.println(THINGSBOARD_SERVER);

  for (int i = 0; i < 3; i++) {
    if (mqttClient.connect("ESP32Client", TOKEN, NULL)) {
      mqttClient.subscribe("v1/devices/me/rpc/request/+");
      lcdStatus("MQTT Connected");
      Serial.println("MQTT Connected!");
      return true;
    } else {
      Serial.print("MQTT failed, rc=");
      Serial.println(mqttClient.state());
      delay(2000);
    }
  }
  lcdStatus("MQTT Failed");
  Serial.println("MQTT connection failed after retries.");
  return false;
}

// ========== Wi-Fi Connect ==========
bool connectWiFi() {
  lcdStatus("Connecting WiFi...");
  WiFi.begin(ssid, password);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
    lcdStatus("WiFi...", ".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    lcdStatus("WiFi OK", WiFi.localIP().toString());
    wifiConnected = true;
    mqttClient.setClient(wifiClient);
    return true;
  }
  lcdStatus("WiFi Failed");
  wifiConnected = false;
  return false;
}

// ========== GSM Connect ==========
bool connectGSM() {
  lcdStatus("Init GSM...");
  SerialAT.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
  delay(3000);
  modem.restart();

  if (!modem.waitForNetwork(20000L)) {
    lcdStatus("No GSM net");
    Serial.println("No GSM network found");
    gsmConnected = false;
    return false;
  }
  if (!modem.gprsConnect("safaricom", "", "")) { // Safaricom APN
    lcdStatus("GPRS Failed");
    gsmConnected = false;
    return false;
  }
  lcdStatus("GPRS OK");
  mqttClient.setClient(gsmClient);
  gsmConnected = true;
  return true;
}

// ========== SMS SENDER ==========
void sendSMS(String message) {
  Serial.println("Sending SMS: " + message);
  lcdStatus("Sending SMS...", "");
  SerialAT.println("AT+CMGF=1"); // Set SMS text mode
  delay(500);
  SerialAT.print("AT+CMGS=\"");
  SerialAT.print(alertNumber);
  SerialAT.println("\"");
  delay(500);
  SerialAT.print(message);
  delay(500);
  SerialAT.write(26); // CTRL+Z to send
  delay(500);
  lcdStatus("SMS Sent", "");
  Serial.println("SMS sent successfully");
}

// ========== Send Data (Main Reporting Function) ==========
void sendData() {
  float temp = dht.readTemperature();
  float hum = dht.readHumidity();

  if (isnan(temp) || isnan(hum)) {
    Serial.println("Failed to read DHT!");
    lcdStatus("Sensor Error", "");
    return;
  }

  // Build telemetry JSON
  String payload = "{\"temperature\":" + String(temp, 1) +
                   ",\"humidity\":" + String(hum, 1) +
                   ",\"pump\":" + String(pumpState ? "true" : "false") + "}";

  // Try MQTT publishing
  if (mqttClient.connected()) {
    mqttClient.publish("v1/devices/me/telemetry", payload.c_str());
    Serial.println("Sent to ThingsBoard: " + payload);
    lcdStatus("Data Sent", "MQTT OK");
  } else {
    // If MQTT or Wi-Fi fails, use GSM SMS fallback
    String smsMsg = "SMART FARM REPORT:\n";
    smsMsg += "Temp: " + String(temp, 1) + "C\n";
    smsMsg += "Humidity: " + String(hum, 1) + "%\n";
    smsMsg += "Pump: " + String(pumpState ? "ON" : "OFF") + "\n";
    smsMsg += (wifiConnected ? "Wi-Fi OFFLINE\n" : "Wi-Fi UNAVAILABLE\n");
    sendSMS(smsMsg);
  }

  // Optional: send high temp alert
  if (temp > 35.0) {
    sendSMS("ALERT: Temperature is above 35°C!");
  }
}

// ========== Setup ==========
void setup() {
  Serial.begin(115200);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);

  lcd.init();
  lcd.backlight();
  dht.begin();

  // Try Wi-Fi first
  if (!connectWiFi()) {
    // If Wi-Fi fails, try GSM
    if (!connectGSM()) {
      lcdStatus("No Network!", "");
      while (true);
    }
  }

  connectMQTT();
  lcdStatus("System Ready", "Monitoring...");
}

// ========== Loop ==========
void loop() {
  if (!mqttClient.connected()) {
    connectMQTT();
  }
  mqttClient.loop();

  // Switch between Wi-Fi & GSM if needed
  if (wifiConnected && WiFi.status() != WL_CONNECTED) {
    wifiConnected = false;
    lcdStatus("Wi-Fi Lost", "Switching to GSM");
    if (connectGSM()) connectMQTT();
  }
  if (!wifiConnected && WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    lcdStatus("Wi-Fi Restored", "");
    mqttClient.setClient(wifiClient);
    connectMQTT();
  }

  // Periodic telemetry
  if (millis() - lastSend > interval) {
    lastSend = millis();
    sendData();
  }
}