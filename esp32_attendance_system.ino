/* ==================================================================================
   SMART FINGERPRINT ATTENDANCE SYSTEM - ESP32 FIRMWARE
   ==================================================================================
   Hardware:
     - ESP32 Dev Board
     - AS608 / R307 Optical Fingerprint Sensor (UART / HardwareSerial2)
     - Micro SD Card Module (SPI)
     - SSD1306 / SH1106 OLED Display (I2C)

   Responsibilities:
     - Connect to local Wi-Fi, sync real time via NTP
     - Run a WebSocket server (port 81) that a phone browser (standalone_app.html)
       connects to for enroll / delete / live attendance streaming
     - Maintain a simple flat-file "database" on the SD card:
         users.csv       -> ID,Name,Department
         attendance.csv  -> ID,Name,Department,Date,Time
     - Continuously poll the fingerprint sensor for matches and broadcast
       check-in events over WebSocket in real time
     - Show status / instructions on the OLED

   Libraries required (install via Arduino Library Manager):
     - WebSockets by Markus Sattler        (links2004/arduinoWebSockets)
     - ArduinoJson by Benoit Blanchon (v6.x)
     - Adafruit Fingerprint Sensor Library
     - Adafruit SSD1306 + Adafruit GFX      (for OLED)
   ================================================================================== */

#include <WiFi.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <Adafruit_Fingerprint.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>
#include <time.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

/* ==================================================================================
   USER-CONFIGURABLE SETTINGS
   ================================================================================== */

// ---- Wi-Fi credentials ----
const char* WIFI_SSID     = "123579877";
const char* WIFI_PASSWORD = "asdfhj";

// ---- NTP / Timezone settings ----
// Example: India = 5*3600 + 1800 (UTC+5:30). Adjust for your region.
const char* NTP_SERVER_1     = "pool.ntp.org";
const char* NTP_SERVER_2     = "time.nist.gov";
const long  GMT_OFFSET_SEC   = 5 * 3600 + 1800;  // <-- CHANGE to your UTC offset in seconds
const int   DAYLIGHT_OFFSET  = 0;                // set to 3600 if your region observes DST

// ---- Pin definitions ----
// Fingerprint sensor on HardwareSerial2 (change RX/TX if needed)
#define FP_RX_PIN 16   // ESP32 RX2 <- Sensor TX
#define FP_TX_PIN 17   // ESP32 TX2 -> Sensor RX

// SD card module (SPI) - VSPI default pins used, CS is the only one that must be defined
#define SD_CS_PIN 5

// OLED (I2C) - uses default SDA=21, SCL=22 on most ESP32 dev boards
#define OLED_WIDTH   128
#define OLED_HEIGHT  64
#define OLED_RESET   -1
#define OLED_ADDR    0x3C

// ---- File names on the SD card ----
const char* USERS_FILE      = "/users.csv";
const char* ATTENDANCE_FILE = "/attendance.csv";

// ---- Attendance de-duplication window (avoid double-logging the same finger
//      if it stays on the sensor across multiple loop() iterations) ----
const unsigned long SAME_USER_COOLDOWN_MS = 8000;

/* ==================================================================================
   GLOBAL OBJECTS
   ================================================================================== */

WebSocketsServer webSocket = WebSocketsServer(81);

HardwareSerial fingerSerial(2); // UART2 for the fingerprint sensor
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fingerSerial);

Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET);

bool sdReady   = false;
bool fpReady   = false;
bool oledReady = false;

uint16_t lastMatchedId = 0;
unsigned long lastMatchTime = 0;

/* Track which connected WebSocket client (if any) initiated an enrollment,
   so status updates go back to the right phone if you extend this to
   multiple simultaneous clients. For a single-user LAN tool, broadcasting
   to all clients is simplest and is what we do below. */

/* ==================================================================================
   FORWARD DECLARATIONS
   ================================================================================== */
void connectWiFi();
void setupTime();
void setupSDCard();
void setupFingerprintSensor();
void setupOLED();
void oledMessage(const String& line1, const String& line2 = "", const String& line3 = "");

void webSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length);
void handleIncomingJson(uint8_t clientNum, const String& jsonStr);

void broadcastStatus(const String& message);
void broadcastError(const String& message);
void broadcastSuccess(const String& message);
void broadcastCheckIn(uint16_t id, const String& name, const String& dept, const String& dateStr, const String& timeStr);

bool findUserInCsv(uint16_t id, String& outName, String& outDept);
bool appendUserToCsv(uint16_t id, const String& name, const String& dept);
bool removeUserFromCsv(uint16_t id);
bool appendAttendanceRecord(uint16_t id, const String& name, const String& dept);

String getFormattedDate();
String getFormattedTime();
String getFormattedDateTime();

void handleDeleteAction(uint16_t id);
void handleEnrollAction(uint16_t id, const String& name, const String& dept);
uint8_t enrollFingerprint(uint16_t id);

void pollFingerprintSensor();
uint8_t getFingerprintID();

/* ==================================================================================
   SETUP
   ================================================================================== */
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n\n===== Smart Fingerprint Attendance System - Booting =====");

  setupOLED();
  oledMessage("Booting...", "Please wait");

  setupSDCard();
  setupFingerprintSensor();

  connectWiFi();
  setupTime();

  // Start the WebSocket server and register the event callback
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  Serial.println("[WS] WebSocket server started on port 81");

  oledMessage("System Ready", WiFi.localIP().toString(), "Scan finger...");
  Serial.println("===== Setup complete. Entering main loop. =====\n");
}

/* ==================================================================================
   MAIN LOOP
   ================================================================================== */
void loop() {
  // Keep the WebSocket server responsive at all times
  webSocket.loop();

  // Continuously check the fingerprint sensor for a match
  pollFingerprintSensor();
}

/* ==================================================================================
   WI-FI + NTP SETUP
   ================================================================================== */
void connectWiFi() {
  Serial.printf("[WiFi] Connecting to SSID '%s' ...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startAttempt = millis();
  const unsigned long WIFI_TIMEOUT_MS = 20000;

  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
    if (millis() - startAttempt > WIFI_TIMEOUT_MS) {
      Serial.println("\n[WiFi] ERROR: Could not connect within timeout. Retrying...");
      WiFi.disconnect();
      delay(1000);
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      startAttempt = millis();
      oledMessage("WiFi Error", "Retrying...");
    }
  }

  Serial.println();
  Serial.print("[WiFi] Connected! IP address: ");
  Serial.println(WiFi.localIP());
  oledMessage("WiFi Connected", WiFi.localIP().toString());
}

void setupTime() {
  Serial.println("[NTP] Syncing time...");
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET, NTP_SERVER_1, NTP_SERVER_2);

  struct tm timeinfo;
  int attempts = 0;
  while (!getLocalTime(&timeinfo) && attempts < 20) {
    Serial.println("[NTP] Waiting for time sync...");
    delay(500);
    attempts++;
  }

  if (attempts >= 20) {
    Serial.println("[NTP] ERROR: Failed to obtain time. Attendance timestamps may be inaccurate.");
    oledMessage("NTP Sync Failed", "Check network");
  } else {
    Serial.println("[NTP] Time synced successfully:");
    Serial.println(getFormattedDateTime());
  }
}

/* ==================================================================================
   SD CARD SETUP
   ================================================================================== */
void setupSDCard() {
  Serial.println("[SD] Initializing SD card...");

  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("[SD] ERROR: SD Card initialization failed! Check wiring/CS pin/card format (FAT32).");
    sdReady = false;
    oledMessage("SD Card Error!", "Check card");
    return;
  }

  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("[SD] ERROR: No SD card detected.");
    sdReady = false;
    oledMessage("No SD Card", "Insert card");
    return;
  }

  sdReady = true;
  Serial.println("[SD] SD card initialized successfully.");

  // Ensure users.csv exists (create empty file if missing)
  if (!SD.exists(USERS_FILE)) {
    File f = SD.open(USERS_FILE, FILE_WRITE);
    if (f) {
      Serial.println("[SD] Created new users.csv");
      f.close();
    } else {
      Serial.println("[SD] ERROR: Could not create users.csv");
    }
  }

  // Ensure attendance.csv exists, with a header row for readability
  if (!SD.exists(ATTENDANCE_FILE)) {
    File f = SD.open(ATTENDANCE_FILE, FILE_WRITE);
    if (f) {
      f.println("ID,Name,Department,Date,Time");
      Serial.println("[SD] Created new attendance.csv");
      f.close();
    } else {
      Serial.println("[SD] ERROR: Could not create attendance.csv");
    }
  }
}

/* ==================================================================================
   FINGERPRINT SENSOR SETUP
   ================================================================================== */
void setupFingerprintSensor() {
  Serial.println("[FP] Initializing fingerprint sensor...");
  fingerSerial.begin(57600, SERIAL_8N1, FP_RX_PIN, FP_TX_PIN);
  delay(100);

  if (finger.verifyPassword()) {
    Serial.println("[FP] Fingerprint sensor found and verified.");
    fpReady = true;
  } else {
    Serial.println("[FP] ERROR: Fingerprint sensor not found! Check wiring (RX/TX may be swapped) and baud rate.");
    fpReady = false;
    oledMessage("Sensor Error!", "Check wiring");
    return;
  }

  finger.getParameters();
  Serial.print("[FP] Sensor capacity: ");
  Serial.println(finger.capacity);
  Serial.print("[FP] Security level: ");
  Serial.println(finger.security_level);
}

/* ==================================================================================
   OLED SETUP + HELPER
   ================================================================================== */
void setupOLED() {
  Wire.begin(); // default SDA=21 SCL=22
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("[OLED] ERROR: SSD1306 allocation failed / display not found.");
    oledReady = false;
    return;
  }
  oledReady = true;
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Attendance System");
  display.display();
}

// Simple 3-line status writer for the OLED. Safe to call even if OLED failed init.
void oledMessage(const String& line1, const String& line2, const String& line3) {
  Serial.printf("[OLED] %s | %s | %s\n", line1.c_str(), line2.c_str(), line3.c_str());
  if (!oledReady) return;

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(line1);
  if (line2.length()) display.println(line2);
  if (line3.length()) display.println(line3);
  display.display();
}

/* ==================================================================================
   WEBSOCKET EVENT HANDLER
   ================================================================================== */
void webSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.printf("[WS] Client #%u disconnected\n", num);
      break;

    case WStype_CONNECTED: {
      IPAddress ip = webSocket.remoteIP(num);
      Serial.printf("[WS] Client #%u connected from %d.%d.%d.%d\n", num, ip[0], ip[1], ip[2], ip[3]);
      break;
    }

    case WStype_TEXT: {
      String msg = String((char*)payload).substring(0, length);
      Serial.printf("[WS] Received from #%u: %s\n", num, msg.c_str());
      handleIncomingJson(num, msg);
      break;
    }

    case WStype_ERROR:
      Serial.printf("[WS] Error on client #%u\n", num);
      break;

    default:
      break;
  }
}

/* ==================================================================================
   INCOMING JSON ROUTER
   Expected payloads from the phone app:
     Enroll: {"action":"enroll","id":X,"name":"...","dept":"..."}
     Delete: {"action":"delete","id":X}
   ================================================================================== */
void handleIncomingJson(uint8_t clientNum, const String& jsonStr) {
  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, jsonStr);

  if (err) {
    Serial.print("[WS] JSON parse error: ");
    Serial.println(err.c_str());
    broadcastError("Invalid JSON received by device.");
    return;
  }

  const char* action = doc["action"];
  if (!action) {
    broadcastError("Missing 'action' field in request.");
    return;
  }

  uint16_t id = doc["id"] | 0;

  if (id < 1 || id > 127) {
    broadcastError("User ID must be between 1 and 127.");
    return;
  }

  if (strcmp(action, "enroll") == 0) {
    String name = doc["name"] | "";
    String dept = doc["dept"] | "";
    if (name.length() == 0 || dept.length() == 0) {
      broadcastError("Name and Department are required for enrollment.");
      return;
    }
    handleEnrollAction(id, name, dept);
  }
  else if (strcmp(action, "delete") == 0) {
    handleDeleteAction(id);
  }
  else {
    broadcastError("Unknown action: " + String(action));
  }
}

/* ==================================================================================
   WEBSOCKET BROADCAST HELPERS
   All messages are broadcast to every connected client, which is sufficient for a
   single-phone LAN tool. Each message includes a "type" field the phone app
   switches on: "status" | "success" | "error" | "log"
   ================================================================================== */
void broadcastStatus(const String& message) {
  StaticJsonDocument<256> doc;
  doc["type"] = "status";
  doc["message"] = message;
  String out;
  serializeJson(doc, out);
  webSocket.broadcastTXT(out);
}

void broadcastError(const String& message) {
  Serial.println("[ERROR] " + message);
  StaticJsonDocument<256> doc;
  doc["type"] = "error";
  doc["message"] = message;
  String out;
  serializeJson(doc, out);
  webSocket.broadcastTXT(out);
}

void broadcastSuccess(const String& message) {
  StaticJsonDocument<256> doc;
  doc["type"] = "success";
  doc["message"] = message;
  String out;
  serializeJson(doc, out);
  webSocket.broadcastTXT(out);
}

void broadcastCheckIn(uint16_t id, const String& name, const String& dept, const String& dateStr, const String& timeStr) {
  StaticJsonDocument<256> doc;
  doc["type"] = "log";
  doc["id"] = id;
  doc["name"] = name;
  doc["dept"] = dept;
  doc["date"] = dateStr;
  doc["time"] = timeStr;
  String out;
  serializeJson(doc, out);
  webSocket.broadcastTXT(out);
}

/* ==================================================================================
   TIME FORMATTING HELPERS
   ================================================================================== */
String getFormattedDate() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "0000-00-00";
  char buf[16];
  strftime(buf, sizeof(buf), "%Y-%m-%d", &timeinfo);
  return String(buf);
}

String getFormattedTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "00:00:00";
  char buf[16];
  strftime(buf, sizeof(buf), "%H:%M:%S", &timeinfo);
  return String(buf);
}

String getFormattedDateTime() {
  return getFormattedDate() + " " + getFormattedTime();
}

/* ==================================================================================
   USERS.CSV HELPERS  (format: ID,Name,Department)
   ================================================================================== */

// Search users.csv line by line for a matching ID. Returns true if found.
bool findUserInCsv(uint16_t id, String& outName, String& outDept) {
  if (!sdReady) {
    Serial.println("[SD] ERROR: SD not ready, cannot read users.csv");
    return false;
  }

  File f = SD.open(USERS_FILE, FILE_READ);
  if (!f) {
    Serial.println("[SD] ERROR: Could not open users.csv for reading");
    return false;
  }

  bool found = false;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    int comma1 = line.indexOf(',');
    int comma2 = line.indexOf(',', comma1 + 1);
    if (comma1 == -1 || comma2 == -1) continue; // malformed line, skip

    uint16_t lineId = line.substring(0, comma1).toInt();
    if (lineId == id) {
      outName = line.substring(comma1 + 1, comma2);
      outDept = line.substring(comma2 + 1);
      found = true;
      break;
    }
  }

  f.close();
  return found;
}

// Append a new user record. Assumes caller has already checked ID isn't duplicated
// (or accepts duplicates being resolved by "first match wins" in findUserInCsv).
bool appendUserToCsv(uint16_t id, const String& name, const String& dept) {
  if (!sdReady) {
    Serial.println("[SD] ERROR: SD not ready, cannot write users.csv");
    return false;
  }

  File f = SD.open(USERS_FILE, FILE_APPEND);
  if (!f) {
    Serial.println("[SD] ERROR: Could not open users.csv for appending");
    return false;
  }

  f.printf("%u,%s,%s\n", id, name.c_str(), dept.c_str());
  f.close();
  Serial.printf("[SD] Added user to users.csv -> ID:%u Name:%s Dept:%s\n", id, name.c_str(), dept.c_str());
  return true;
}

// Rewrites users.csv without the given ID (simulates deletion on a flat file).
bool removeUserFromCsv(uint16_t id) {
  if (!sdReady) {
    Serial.println("[SD] ERROR: SD not ready, cannot modify users.csv");
    return false;
  }

  File src = SD.open(USERS_FILE, FILE_READ);
  if (!src) {
    Serial.println("[SD] ERROR: Could not open users.csv for reading");
    return false;
  }

  // Write surviving records to a temp file, then replace the original
  const char* TEMP_FILE = "/users_tmp.csv";
  SD.remove(TEMP_FILE); // just in case a stale temp file exists
  File tmp = SD.open(TEMP_FILE, FILE_WRITE);
  if (!tmp) {
    Serial.println("[SD] ERROR: Could not create temp file for users.csv rewrite");
    src.close();
    return false;
  }

  bool removedAny = false;
  while (src.available()) {
    String line = src.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    int comma1 = line.indexOf(',');
    if (comma1 == -1) continue;
    uint16_t lineId = line.substring(0, comma1).toInt();

    if (lineId == id) {
      removedAny = true;
      continue; // skip writing this line -> effectively deletes it
    }
    tmp.println(line);
  }

  src.close();
  tmp.close();

  // Replace original file with the rewritten one
  SD.remove(USERS_FILE);
  SD.rename(TEMP_FILE, USERS_FILE);

  if (removedAny) {
    Serial.printf("[SD] Removed user ID %u from users.csv\n", id);
  } else {
    Serial.printf("[SD] WARNING: ID %u was not found in users.csv (nothing removed)\n", id);
  }

  return removedAny;
}

/* ==================================================================================
   ATTENDANCE.CSV HELPER  (format: ID,Name,Department,Date,Time)
   ================================================================================== */
bool appendAttendanceRecord(uint16_t id, const String& name, const String& dept) {
  if (!sdReady) {
    Serial.println("[SD] ERROR: SD not ready, cannot write attendance.csv");
    return false;
  }

  File f = SD.open(ATTENDANCE_FILE, FILE_APPEND);
  if (!f) {
    Serial.println("[SD] ERROR: Could not open attendance.csv for appending");
    return false;
  }

  String date = getFormattedDate();
  String time = getFormattedTime();
  f.printf("%u,%s,%s,%s,%s\n", id, name.c_str(), dept.c_str(), date.c_str(), time.c_str());
  f.close();

  Serial.printf("[ATTEND] Logged check-in -> ID:%u Name:%s Dept:%s Date:%s Time:%s\n",
                id, name.c_str(), dept.c_str(), date.c_str(), time.c_str());
  return true;
}

/* ==================================================================================
   DELETE USER ACTION
   ================================================================================== */
void handleDeleteAction(uint16_t id) {
  Serial.printf("[ACTION] Delete requested for ID %u\n", id);
  oledMessage("Deleting user", "ID: " + String(id));

  if (!fpReady) {
    broadcastError("Fingerprint sensor not available. Cannot delete model.");
    return;
  }

  uint8_t result = finger.deleteModel(id);

  if (result == FINGERPRINT_OK) {
    Serial.printf("[FP] Fingerprint model %u deleted from sensor.\n", id);
  } else {
    // Not necessarily fatal - the ID may not have existed on the sensor,
    // but we still want to clean up users.csv in case of orphaned records.
    Serial.printf("[FP] WARNING: deleteModel() returned code %u for ID %u\n", result, id);
  }

  bool removed = removeUserFromCsv(id);

  if (result == FINGERPRINT_OK || removed) {
    broadcastSuccess("User ID " + String(id) + " deleted successfully.");
    oledMessage("User Deleted", "ID: " + String(id));
  } else {
    broadcastError("User ID " + String(id) + " was not found on sensor or in database.");
    oledMessage("Delete Failed", "ID not found");
  }
}

/* ==================================================================================
   ENROLL USER ACTION
   ================================================================================== */
void handleEnrollAction(uint16_t id, const String& name, const String& dept) {
  Serial.printf("[ACTION] Enroll requested for ID %u (%s, %s)\n", id, name.c_str(), dept.c_str());

  if (!fpReady) {
    broadcastError("Fingerprint sensor not available. Cannot enroll.");
    return;
  }
  if (!sdReady) {
    broadcastError("SD card not available. Cannot save user record.");
    return;
  }

  broadcastStatus("Starting enrollment for ID " + String(id) + ". Place finger on sensor...");
  oledMessage("Enroll ID " + String(id), "Place finger", "on sensor");

  uint8_t enrollResult = enrollFingerprint(id);

  if (enrollResult == FINGERPRINT_OK) {
    bool saved = appendUserToCsv(id, name, dept);
    if (saved) {
      broadcastSuccess("Enrollment complete for " + name + " (ID " + String(id) + ").");
      oledMessage("Enroll Success", name, "ID: " + String(id));
    } else {
      broadcastError("Fingerprint stored, but failed to save user record to SD card.");
      oledMessage("SD Write Failed", "Enrollment incomplete");
    }
  } else {
    broadcastError("Enrollment failed for ID " + String(id) + ". Please try again.");
    oledMessage("Enroll Failed", "Try again");
  }
}

/* ------------------------------------------------------------------------------
   enrollFingerprint()
   Standard two-scan enrollment state machine using the Adafruit Fingerprint
   library. Sends live status updates back to the phone at each stage so the
   user gets real-time feedback instead of silence during the multi-second
   process.
   ------------------------------------------------------------------------------ */
uint8_t enrollFingerprint(uint16_t id) {
  int p = -1;

  // ---- STEP 1: Capture first image ----
  Serial.println("[FP] Waiting for first finger image...");
  broadcastStatus("Place your finger firmly on the sensor.");

  unsigned long stepStart = millis();
  const unsigned long STEP_TIMEOUT_MS = 15000;

  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    switch (p) {
      case FINGERPRINT_OK:
        Serial.println("[FP] Image 1 captured.");
        break;
      case FINGERPRINT_NOFINGER:
        break; // keep waiting
      case FINGERPRINT_PACKETRECIEVEERR:
        broadcastError("Communication error with sensor. Aborting enrollment.");
        return p;
      case FINGERPRINT_IMAGEFAIL:
        broadcastError("Imaging error. Please clean the sensor and try again.");
        return p;
      default:
        broadcastError("Unknown sensor error (code " + String(p) + ").");
        return p;
    }
    if (millis() - stepStart > STEP_TIMEOUT_MS) {
      broadcastError("Enrollment timed out waiting for finger. Please retry.");
      return FINGERPRINT_IMAGEFAIL;
    }
    webSocket.loop(); // keep server responsive during the blocking wait
    delay(50);
  }

  p = finger.image2Tz(1);
  if (p != FINGERPRINT_OK) {
    broadcastError("Failed to process first fingerprint image (code " + String(p) + ").");
    return p;
  }

  broadcastStatus("First scan captured. Remove your finger.");
  oledMessage("Remove finger", "then place", "again");

  // ---- Wait for finger removal before requesting the second scan ----
  Serial.println("[FP] Waiting for finger removal...");
  p = finger.getImage();
  while (p != FINGERPRINT_NOFINGER) {
    p = finger.getImage();
    webSocket.loop();
    delay(50);
  }

  // ---- STEP 2: Capture second image ----
  broadcastStatus("Place the same finger again to confirm.");
  Serial.println("[FP] Waiting for second finger image...");
  p = -1;
  stepStart = millis();

  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    switch (p) {
      case FINGERPRINT_OK:
        Serial.println("[FP] Image 2 captured.");
        break;
      case FINGERPRINT_NOFINGER:
        break;
      case FINGERPRINT_PACKETRECIEVEERR:
        broadcastError("Communication error with sensor. Aborting enrollment.");
        return p;
      case FINGERPRINT_IMAGEFAIL:
        broadcastError("Imaging error. Please clean the sensor and try again.");
        return p;
      default:
        broadcastError("Unknown sensor error (code " + String(p) + ").");
        return p;
    }
    if (millis() - stepStart > STEP_TIMEOUT_MS) {
      broadcastError("Enrollment timed out waiting for finger. Please retry.");
      return FINGERPRINT_IMAGEFAIL;
    }
    webSocket.loop();
    delay(50);
  }

  p = finger.image2Tz(2);
  if (p != FINGERPRINT_OK) {
    broadcastError("Failed to process second fingerprint image (code " + String(p) + ").");
    return p;
  }

  // ---- Combine the two scans into a single model ----
  broadcastStatus("Creating fingerprint model...");
  p = finger.createModel();
  if (p != FINGERPRINT_OK) {
    if (p == FINGERPRINT_ENROLLMISMATCH) {
      broadcastError("Fingerprints did not match each other. Please retry enrollment.");
    } else {
      broadcastError("Failed to create fingerprint model (code " + String(p) + ").");
    }
    return p;
  }

  // ---- Store the model at the requested ID slot ----
  broadcastStatus("Saving fingerprint to sensor memory...");
  p = finger.storeModel(id);
  if (p != FINGERPRINT_OK) {
    broadcastError("Failed to store fingerprint model at ID " + String(id) + " (code " + String(p) + ").");
    return p;
  }

  Serial.printf("[FP] Fingerprint model successfully stored at ID %u\n", id);
  return FINGERPRINT_OK;
}

/* ==================================================================================
   FINGERPRINT MATCHING (MAIN LOOP POLLING)
   ================================================================================== */
void pollFingerprintSensor() {
  if (!fpReady) return; // nothing to do if sensor failed init

  uint8_t id = getFingerprintID();
  if (id == 0) return; // 0 = no valid match this cycle

  // Basic debounce: ignore repeated matches of the same finger within the cooldown
  if (id == lastMatchedId && (millis() - lastMatchTime) < SAME_USER_COOLDOWN_MS) {
    return;
  }

  lastMatchedId = id;
  lastMatchTime = millis();

  String name, dept;
  bool found = findUserInCsv(id, name, dept);

  if (!found) {
    Serial.printf("[ATTEND] WARNING: Fingerprint matched ID %u but no user record exists in users.csv\n", id);
    name = "Unknown";
    dept = "Unregistered";
  }

  bool logged = appendAttendanceRecord(id, name, dept);
  String dateStr = getFormattedDate();
  String timeStr = getFormattedTime();

  if (logged) {
    broadcastCheckIn(id, name, dept, dateStr, timeStr);
    oledMessage("Welcome!", name, timeStr);
  } else {
    broadcastError("Check-in matched but failed to write to attendance.csv.");
    oledMessage("Log Write Failed", name);
  }
}

/* ------------------------------------------------------------------------------
   getFingerprintID()
   Attempts a single non-blocking-ish read+match cycle.
   Returns the matched finger ID (1-127), or 0 if no finger / no match.
   ------------------------------------------------------------------------------ */
uint8_t getFingerprintID() {
  uint8_t p = finger.getImage();

  switch (p) {
    case FINGERPRINT_OK:
      break; // proceed to matching
    case FINGERPRINT_NOFINGER:
      return 0; // idle, nothing on the sensor - normal, not an error
    case FINGERPRINT_PACKETRECIEVEERR:
      Serial.println("[FP] ERROR: Communication error while reading image.");
      return 0;
    case FINGERPRINT_IMAGEFAIL:
      Serial.println("[FP] ERROR: Imaging error while reading finger.");
      return 0;
    default:
      Serial.printf("[FP] ERROR: Unknown error reading image (code %u).\n", p);
      return 0;
  }

  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) {
    Serial.printf("[FP] ERROR: Could not convert image (code %u).\n", p);
    return 0;
  }

  p = finger.fingerFastSearch();
  if (p != FINGERPRINT_OK) {
    // FINGERPRINT_NOTFOUND is expected for unregistered fingers - not a hard error
    if (p == FINGERPRINT_NOTFOUND) {
      Serial.println("[FP] Finger detected but not found in database (unregistered).");
    } else {
      Serial.printf("[FP] ERROR: fingerFastSearch failed (code %u).\n", p);
    }
    return 0;
  }

  // Match found
  Serial.printf("[FP] Match found -> ID: %u, Confidence: %u\n", finger.fingerID, finger.confidence);
  return finger.fingerID;
}
