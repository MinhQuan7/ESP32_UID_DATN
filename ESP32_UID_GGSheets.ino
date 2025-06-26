#include "Arduino.h"
#include <HardwareSerial.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <map>

// WiFi credentials
const char* ssid = "eoh.ioo";
const char* password = "Eoh@2020";

// Google Apps Script Web App URL
const char* googleScriptURL = "https://script.google.com/macros/s/AKfycbzjx4MP0oo2lDLb_fFZzXdCWpI6NeW4aaTKsKlrMRBosX6w64I9gWDcWF9KHWx3ywzG/exec";

// NTP Client for time sync
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 7 * 3600, 60000);  // GMT+7 Vietnam

HardwareSerial ReaderSerial(2);  // Use RX2/TX2 for UHF Reader

// UID tracking structure
struct UIDRecord {
  String uid;
  unsigned long timestamp;
  bool sent;
};

// Map to store UID records
std::map<String, UIDRecord> uidRecords;

// Configuration
const unsigned long DUPLICATE_FILTER_TIME = 5000;  // 5 seconds
const unsigned long CLEANUP_INTERVAL = 60000;      // 1 minute
unsigned long lastCleanupTime = 0;

// Serial buffer for packet processing
String serialBuffer = "";
unsigned long lastDataTime = 0;
const unsigned long DATA_TIMEOUT = 100;  // 100ms

void setup() {
  Serial.begin(115200);
  ReaderSerial.begin(57600, SERIAL_8N1, 16, 17);  // RX2=GPIO16, TX2=GPIO17

  Serial.println("ESP32 UHF Reader to Google Sheets - v3.0");

  // Connect to WiFi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("Connecting to WiFi...");
  }
  Serial.println("WiFi connected!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  // Initialize NTP
  timeClient.begin();
  timeClient.update();
  Serial.println("NTP initialized");

  Serial.println("System ready - listening for UHF tags...");
  lastCleanupTime = millis();
}

void loop() {
  // Update NTP time
  timeClient.update();

  // Cleanup old records
  cleanupOldRecords();

  // Process serial data from UHF reader
  processSerialData();

  delay(10);
}

void processSerialData() {
  while (ReaderSerial.available() > 0) {
    byte incomingByte = ReaderSerial.read();

    // Reset buffer on timeout
    if (millis() - lastDataTime > DATA_TIMEOUT && serialBuffer.length() > 0) {
      serialBuffer = "";
    }

    // Add byte to buffer as hex
    if (incomingByte < 0x10) serialBuffer += "0";
    serialBuffer += String(incomingByte, HEX);
    lastDataTime = millis();

    // Check for complete packet (16 hex chars = 8 bytes)
    if (serialBuffer.length() >= 16) {
      processPacket(serialBuffer);
      serialBuffer = "";
    }
  }
}

void processPacket(String hexData) {
  // Convert hex string to byte array
  int len = hexData.length() / 2;
  byte buffer[len];

  for (int i = 0; i < len; i++) {
    String byteString = hexData.substring(i * 2, i * 2 + 2);
    buffer[i] = (byte)strtol(byteString.c_str(), NULL, 16);
  }

  // Debug output
  Serial.print("Received packet: ");
  for (int i = 0; i < len; i++) {
    if (buffer[i] < 0x10) Serial.print("0");
    Serial.print(buffer[i], HEX);
    Serial.print(" ");
  }
  Serial.println();

  // Check packet format: length=8, buffer[1]=0x00, buffer[2]=0xEE
  if (len == 8 && buffer[1] == 0x00 && buffer[2] == 0xEE) {
    // Extract full 8-byte UID
    String uid = extractFullUID(buffer, len);
    Serial.println("Detected UID: " + uid);
    
    // Process UID
    processUID(uid);
  } else {
    Serial.println("Invalid packet format");
  }
}

String extractFullUID(byte* buffer, int len) {
  // Extract all 8 bytes as UID (including packet header and data)
  String uid = "";
  
  for (int i = 0; i < len; i++) {
    if (buffer[i] < 0x10) uid += "0";
    uid += String(buffer[i], HEX);
  }
  
  uid.toUpperCase();
  return uid;
}

void processUID(String uid) {
  unsigned long currentTime = millis();

  // Check if UID already exists
  auto it = uidRecords.find(uid);
  
  if (it != uidRecords.end()) {
    // UID exists, check if within filter time
    if (currentTime - it->second.timestamp < DUPLICATE_FILTER_TIME) {
      return; // Skip duplicate
    }
    if (it->second.sent) {
      // Update timestamp but don't resend
      it->second.timestamp = currentTime;
      return;
    }
  }

  // New UID or retry needed
  Serial.println("Processing new UID: " + uid);
  
  // Add/update record
  UIDRecord record = {uid, currentTime, false};
  uidRecords[uid] = record;

  // Send to Google Sheets
  if (sendToGoogleSheets(uid)) {
    Serial.println("✓ Successfully sent to Google Sheets");
    uidRecords[uid].sent = true;
  } else {
    Serial.println("✗ Failed to send to Google Sheets");
  }
}

bool sendToGoogleSheets(String uid) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected");
    return false;
  }

  HTTPClient http;
  http.begin(googleScriptURL);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(15000); // 15 second timeout

  // Create JSON payload
  DynamicJsonDocument doc(256);
  doc["uid"] = uid;
  
  String jsonString;
  serializeJson(doc, jsonString);
  
  Serial.println("Sending: " + jsonString);
  
  // Send POST request
  int httpResponseCode = http.POST(jsonString);
  
  Serial.println("Response Code: " + String(httpResponseCode));
  
  bool success = false;
  
  // Check response
  if (httpResponseCode == 200) {
    String response = http.getString();
    Serial.println("Response: " + response);
    
    // Parse JSON response
    DynamicJsonDocument responseDoc(512);
    if (deserializeJson(responseDoc, response) == DeserializationError::Ok) {
      success = responseDoc["success"] | false;
    }
  } else if (httpResponseCode == 302) {
    // Redirect is usually success for Google Apps Script
    Serial.println("Redirect response - treating as success");
    success = true;
  }
  
  http.end();
  return success;
}

void cleanupOldRecords() {
  unsigned long currentTime = millis();
  
  if (currentTime - lastCleanupTime < CLEANUP_INTERVAL) {
    return;
  }
  
  lastCleanupTime = currentTime;
  
  // Remove old sent records
  auto it = uidRecords.begin();
  while (it != uidRecords.end()) {
    if (it->second.sent && (currentTime - it->second.timestamp > 300000)) { // 5 minutes
      Serial.println("Cleaning up old UID: " + it->second.uid);
      it = uidRecords.erase(it);
    } else {
      ++it;
    }
  }
  
  Serial.println("Active UID records: " + String(uidRecords.size()));
}