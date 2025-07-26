#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <Wire.h>
#include <RTClib.h>
#include <LiquidCrystal_I2C.h>
#include <Stepper.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <WiFiClientSecureBearSSL.h>
#include <ESP8266HTTPClient.h>

// WiFi config
const char* SSID_AP = "Pill dispenser";
const char* PASS_AP = "12345678";
String deviceId = "PD01";
const uint16_t WIFI_TIMEOUT = 10000;
const size_t EEPROM_SIZE = 128;
ESP8266WebServer server(80);

struct Credentials {
  char ssid[32];
  char password[32];
} credentials;

// RTC and LCD setup
RTC_DS3231 rtc;
LiquidCrystal_I2C lcd(0x27, 16, 2);

// Stepper motor setup
#define STEPS_PER_REV 2048
#define DEGREE_TO_STEP (STEPS_PER_REV / 360.0 * 51) // ≈ 290 steps
#define IN1 D3
#define IN2 D4
#define IN3 D5
#define IN4 D0
#define buzzer D7
#define CONFIRM D6

Stepper stepper(STEPS_PER_REV, IN1, IN3, IN2, IN4);

// Scheduling
int scheduleHours[6];
int scheduleMinutes[6];
int scheduleCount = 0;
bool alreadyExecuted[6] = {false};

// Mode management
bool refillMode = false;
unsigned long buttonPressStart = 0;
bool buttonHeld = false;

// API endpoint
const char* API_URL = "https://smart-scheduler-s5q7.onrender.com/schedule/PD01";

// --------- SPIFFS functions ----------
bool saveScheduleToSPIFFS() {
  File file = SPIFFS.open("/schedule.json", "w");
  if (!file) {
    Serial.println("Failed to open schedule.json for writing");
    return false;
  }

  StaticJsonDocument<256> doc;
  JsonArray times = doc.createNestedArray("times");
  for (int i = 0; i < scheduleCount; i++) {
    char buf[6];
    sprintf(buf, "%02d:%02d", scheduleHours[i], scheduleMinutes[i]);
    times.add(buf);
  }

  if (serializeJson(doc, file) == 0) {
    Serial.println("Failed to write JSON");
    file.close();
    return false;
  }
  file.close();
  Serial.println("Schedule saved to SPIFFS");
  return true;
}

bool loadScheduleFromSPIFFS() {
  if (!SPIFFS.exists("/schedule.json")) {
    Serial.println("No schedule file found");
    return false;
  }

  File file = SPIFFS.open("/schedule.json", "r");
  if (!file) {
    Serial.println("Failed to open schedule.json");
    return false;
  }

  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  if (error) {
    Serial.println("Failed to parse schedule JSON from SPIFFS");
    return false;
  }

  JsonArray times = doc["times"];
  scheduleCount = 0;
  for (JsonVariant v : times) {
    String t = v.as<String>();
    int hour = t.substring(0, 2).toInt();
    int minute = t.substring(3, 5).toInt();
    scheduleHours[scheduleCount] = hour;
    scheduleMinutes[scheduleCount] = minute;
    alreadyExecuted[scheduleCount] = false;
    scheduleCount++;
    if (scheduleCount >= 6) break;
  }
  Serial.println("Schedule loaded from SPIFFS");
  return true;
}

// --------- API fetch ----------
bool fetchScheduleFromAPI() {
  if (WiFi.status() != WL_CONNECTED) return false;

  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure();

  HTTPClient https;
  if (!https.begin(*client, API_URL)) {
    Serial.println("HTTPS begin failed");
    return false;
  }

  int httpCode = https.GET();
  Serial.println("Sending GET request");

  if (httpCode > 0) {
    Serial.printf("HTTP code: %d\n", httpCode);
    if (httpCode == HTTP_CODE_OK) {
      String payload = https.getString();
      Serial.println(payload);

      StaticJsonDocument<512> doc;
      DeserializationError error = deserializeJson(doc, payload);
      if (error) {
        Serial.println("JSON parse failed");
        https.end();
        return false;
      }

      JsonArray timesArray = doc["times"];
      scheduleCount = 0;
      for (JsonVariant t : timesArray) {
        String timeStr = t["time"].as<String>();
        int hour = timeStr.substring(0, 2).toInt();
        int minute = timeStr.substring(3, 5).toInt();
        scheduleHours[scheduleCount] = hour;
        scheduleMinutes[scheduleCount] = minute;
        alreadyExecuted[scheduleCount] = false;
        scheduleCount++;
        if (scheduleCount >= 6) break;
      }

      saveScheduleToSPIFFS();
      https.end();
      return true;
    }
  } else {
    Serial.printf("HTTPS GET failed, error: %s\n", https.errorToString(httpCode).c_str());
  }

  https.end();
  return false;
}

// EEPROM functions
bool saveWiFiCredentials(const String& ssid, const String& pass) {
  if (ssid.length() > 31 || pass.length() > 31) return false;
  EEPROM.begin(EEPROM_SIZE);
  Credentials temp;
  memset(&temp, 0, sizeof(Credentials));
  ssid.toCharArray(temp.ssid, sizeof(temp.ssid));
  pass.toCharArray(temp.password, sizeof(temp.password));
  EEPROM.put(0, temp);
  bool success = EEPROM.commit();
  EEPROM.end();
  return success;
}

bool loadWiFiCredentials() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, credentials);
  EEPROM.end();
  return strlen(credentials.ssid) > 0 && strlen(credentials.password) > 0;
}

// Web handling
void handleRoot() {
  String html = R"rawliteral(
    <!DOCTYPE html><html><head><title>WiFi Setup</title>
    <meta name='viewport' content='width=device-width, initial-scale=1'>
    <style>body{font-family:Arial;}input{margin:5px;}</style></head>
    <body><h2>ESP8266 WiFi Setup</h2>
    <form action='/save' method='POST'>
      <input name='ssid' placeholder='SSID' required><br>
      <input name='pass' type='password' placeholder='Password' required><br>
      <input type='submit' value='Save'>
    </form></body></html>
  )rawliteral";
  server.send(200, "text/html", html);
}

void handleSave() {
  if (!server.hasArg("ssid") || !server.hasArg("pass")) {
    server.send(400, "text/plain", "Missing credentials");
    return;
  }
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  if (saveWiFiCredentials(ssid, pass)) {
    server.send(200, "text/plain", "Saved. Restarting...");
    delay(1000);
    ESP.restart();
  } else {
    server.send(500, "text/plain", "Failed to save credentials");
  }
}

void startAPMode() {
  WiFi.disconnect();
  WiFi.mode(WIFI_AP);
  WiFi.softAP(SSID_AP, PASS_AP);
  Serial.printf("AP Mode started. IP: %s\n", WiFi.softAPIP().toString().c_str());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
}

bool connectWiFi() {
  if (!loadWiFiCredentials()) {
    Serial.println("No credentials found.");
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(credentials.ssid, credentials.password);
  Serial.printf("Connecting to %s...\n", credentials.ssid);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT) {
    delay(100);
    Serial.print(".");
  }
  Serial.println();
  return WiFi.status() == WL_CONNECTED;
}

void setup() {
  Serial.begin(115200);
  Wire.begin(D2, D1); // SDA, SCL
  lcd.init(); lcd.backlight();
  pinMode(buzzer, OUTPUT);
  pinMode(CONFIRM, INPUT_PULLUP);
  stepper.setSpeed(10);

  if (!rtc.begin()) {
    lcd.print("RTC error");
    while (1);
  }

  // Initialize SPIFFS
  if (!SPIFFS.begin()) {
    Serial.println("SPIFFS init failed");
  }

  lcd.setCursor(0, 0);
  lcd.print("Connecting WiFi...");
  delay(1000);

  if (!connectWiFi()) {
    lcd.clear();
    lcd.print("AP Mode Start");
    startAPMode();
    loadScheduleFromSPIFFS(); // fallback to offline
  } else {
    lcd.clear();
    lcd.print("WiFi connected!");
    delay(1000);

    // Fetch schedule from API
    lcd.clear();
    lcd.print("Fetching from net");
    delay(1000);
    if (!fetchScheduleFromAPI()) {
      loadScheduleFromSPIFFS();
      lcd.clear();
      lcd.print("Loaded local data");
      delay(1000);
    }

    // Initialize OTA
    ArduinoOTA.setHostname("PillDispenser");
    ArduinoOTA.begin();
    Serial.println("OTA Ready");
  }
}

void loop() {
  if (WiFi.getMode() == WIFI_AP) {
    server.handleClient();
  } else {
    ArduinoOTA.handle();
  }

  DateTime now = rtc.now();

  // Display time on row 0
  char buf[9];
  sprintf(buf, "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
  lcd.setCursor(0, 0);
  lcd.print("Time: ");
  lcd.print(buf);
  lcd.print("  ");

  // Button state
  int confState = digitalRead(CONFIRM);
  unsigned long currentMillis = millis();

  // Detect button press duration
  if (confState == LOW) { // button pressed
    if (!buttonHeld) {
      buttonPressStart = currentMillis;
      buttonHeld = true;
    }
    // 5-second hold: toggle mode
    if (buttonHeld && (currentMillis - buttonPressStart >= 5000)) {
      refillMode = !refillMode;
      buttonHeld = false; // reset
      lcd.setCursor(0, 1);
      lcd.print("Mode: ");
      lcd.print(refillMode ? "R " : "N ");
      lcd.print(confState);
      delay(1000); // show change briefly
    }
  } else {
    // Button released
    if (buttonHeld) {
      unsigned long pressDuration = currentMillis - buttonPressStart;

      // In refill mode: 1s press triggers step
      if (refillMode && pressDuration >= 1000 && pressDuration < 5000) {
        stepper.step((int)DEGREE_TO_STEP);
        playBuzzer(500);
      }
      buttonHeld = false;
    }
  }

  // Normal mode: run schedule logic
  if (!refillMode) {
    for (int i = 0; i < scheduleCount; i++) {
      if (now.hour() == scheduleHours[i] && now.minute() == scheduleMinutes[i]) {
        if (!alreadyExecuted[i]) {
          stepper.step((int)DEGREE_TO_STEP);
          playBuzzer(1000);
          alreadyExecuted[i] = true;
        }
      } else {
        alreadyExecuted[i] = false; // reset for next day
      }
    }
  }

  // Update LCD row 1
  lcd.setCursor(0, 1);
  lcd.print("Mode: ");
  lcd.print(refillMode ? "R " : "N ");
  lcd.print(confState);
  lcd.print("   "); // clear leftovers

  delay(100);
}

void playBuzzer(int delayi) {
  digitalWrite(buzzer, HIGH);
  delay(delayi);
  digitalWrite(buzzer, LOW);
}
