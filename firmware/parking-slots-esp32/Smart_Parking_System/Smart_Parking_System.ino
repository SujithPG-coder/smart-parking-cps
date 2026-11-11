/**************************************************
 SMART PARKING SYSTEM WITH CLOUD API SYNC
 Module : Parking Slot Monitoring
 Target : ESP32-S3 (Arduino framework)

 - 3 IR sensors detect occupancy of 3 parking slots
 - Per-slot LEDs: GREEN = free, YELLOW = reserved (cloud), RED = occupied
 - SSD1306 OLED shows the number of free slots
 - Syncs with the cloud dashboard over Wi-Fi:
     GET  api/slots.json    -> pull reservations every 5 s (GitHub Pages)
     POST /api/update-slot  -> push occupancy changes (optional backend)
 Libraries: ArduinoJson, Adafruit GFX, Adafruit SSD1306
**************************************************/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

/******************** OLED ********************/
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

/******************** WIFI ********************/
// Replace with your network credentials before uploading
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

/******************** API ********************/
// Slot data is served from GitHub Pages (read-only, static JSON).
// GitHub Pages cannot accept POST requests, so occupancy uploads are
// disabled by default. Point apiUpdateSlotURL at your own backend and set
// ENABLE_SLOT_UPLOAD to 1 to push live occupancy to a server.
const char* apiGetSlotsURL   = "https://sujithpg-coder.github.io/smart-parking-cps/api/slots.json";
const char* apiUpdateSlotURL = "https://YOUR-SERVER/api/update-slot";
#define ENABLE_SLOT_UPLOAD 0

/******************** PINS ********************/
int irPins[3]     = {4, 5, 6};
int greenPins[3]  = {7, 10, 13};
int redPins[3]    = {8, 11, 14};
int yellowPins[3] = {9, 12, 15};

/******************** STATE ********************/
bool reserved[3] = {false, false, false};

/******************** WIFI CONNECT ********************/
void connectWiFi() {
  Serial.println("Connecting to WiFi...");
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi Connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
}

/******************** FETCH API ********************/
bool fetchSlotStatus() {

  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, apiGetSlotsURL);

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.print("HTTP Error: ");
    Serial.println(httpCode);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, payload);

  if (error) {
    Serial.print("JSON Error: ");
    Serial.println(error.c_str());
    return false;
  }

  JsonArray slots = doc["slots"];

  for (JsonObject slot : slots) {
    int id = slot["id"];
    bool occupied = slot["occupied"];
    bool reservedSrv = slot["reserved"];

    int index = id - 1;

    if (index >= 0 && index < 3) {
      reserved[index] = reservedSrv;

      if (occupied) {
        reserved[index] = false;
      }
    }
  }

  Serial.println("API Sync Successful");
  return true;
}

/******************** POST UPDATE ********************/
bool postSlotUpdate(int id, bool occupied) {

  if (!ENABLE_SLOT_UPLOAD) return false;   // no backend configured
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, apiUpdateSlotURL);
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<128> doc;
  doc["id"] = id;
  doc["occupied"] = occupied;

  String body;
  serializeJson(doc, body);

  int httpCode = http.POST(body);
  http.getString();
  http.end();

  Serial.print("POST id=");
  Serial.print(id);
  Serial.print(" occupied=");
  Serial.print(occupied ? "true" : "false");
  Serial.print(" -> HTTP ");
  Serial.println(httpCode);

  return (httpCode >= 200 && httpCode < 300);
}

/******************** SETUP ********************/
void setup() {

  Serial.begin(115200);

  for (int i = 0; i < 3; i++) {
    pinMode(irPins[i], INPUT);
    pinMode(greenPins[i], OUTPUT);
    pinMode(redPins[i], OUTPUT);
    pinMode(yellowPins[i], OUTPUT);
  }

  // ---- OLED INIT (Stable for ESP32-S3) ----
  Wire.begin(16, 17);
  Wire.setClock(100000);
  delay(200);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED Failed");
    while (true);
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(10, 25);
  display.println("Starting...");
  display.display();
  delay(1000);

  connectWiFi();
}

/******************** LOOP ********************/
unsigned long lastFetch = 0;
const unsigned long fetchInterval = 5000;

bool lastOccupied[3] = {false, false, false};

void loop() {

  if (millis() - lastFetch > fetchInterval) {
    fetchSlotStatus();
    lastFetch = millis();
  }

  int freeSlots = 0;

  for (int i = 0; i < 3; i++) {

    bool occupied = (digitalRead(irPins[i]) == LOW);

    if (occupied != lastOccupied[i]) {
      postSlotUpdate(i + 1, occupied);
      lastOccupied[i] = occupied;
    }

    digitalWrite(greenPins[i], LOW);
    digitalWrite(redPins[i], LOW);
    digitalWrite(yellowPins[i], LOW);

    // ---- Corrected Free Logic ----
    if (occupied) {
      digitalWrite(redPins[i], HIGH);
      reserved[i] = false;
    }
    else if (reserved[i]) {
      digitalWrite(yellowPins[i], HIGH);
    }
    else {
      digitalWrite(greenPins[i], HIGH);
      freeSlots++;   // Count ONLY when green
    }
  }

  /************* OLED DISPLAY *************/
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(10, 25);
  display.print("Free: ");
  display.print(freeSlots);
  display.display();

  delay(200);
}
