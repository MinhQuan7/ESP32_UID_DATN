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
const char* googleScriptURL = "https://script.google.com/macros/s/AKfycbzEBoBf5EDSb5qkA_S-7er430o7UiSNDdXU-nr7gIgANiIJJbmeKQkUw3jJJG-pyAcu0Q/exec";

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

  Serial.println("ESP32 UHF Reader to Google Sheets - v3.1");

  // Connect to WiFi
  WiFi.begin(ssid, password);
  int wifiAttempts = 0;
  while (WiFi.status() != WL_CONNECTED && wifiAttempts < 20) {
    delay(500);
    Serial.print(".");
    wifiAttempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.println("WiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi connection failed!");
    return;
  }

  // Initialize NTP
  timeClient.begin();
  for (int i = 0; i < 5; i++) {
    if (timeClient.update()) {
      Serial.println("NTP synchronized");
      break;
    }
    delay(1000);
  }

  // Test Google Apps Script connection
  Serial.println("Testing Google Apps Script connection...");
  testGoogleConnection();

  Serial.println("System ready - listening for UHF tags...");
  lastCleanupTime = millis();
}

void loop() {
  // Check WiFi connection
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected, reconnecting...");
    WiFi.reconnect();
    delay(5000);
    return;
  }

  // Update NTP time
  timeClient.update();

  // Cleanup old records
  cleanupOldRecords();

  // Process serial data from UHF reader
  processSerialData();

  delay(10);
}

void testGoogleConnection() {
  HTTPClient http;
  http.begin(googleScriptURL);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(20000);
  
  // Test with dummy data
  String testPayload = "{\"uid\":\"TEST123456\"}";
  Serial.println("Sending test payload: " + testPayload);
  
  int httpResponseCode = http.POST(testPayload);
  
  Serial.println("Test Response Code: " + String(httpResponseCode));
  
  if (httpResponseCode > 0) {
    String response = http.getString();
    Serial.println("Test Response: " + response);
  } else {
    Serial.println("Test Error: " + String(httpResponseCode));
    Serial.println("HTTP Error: " + http.errorToString(httpResponseCode));
  }
  
  http.end();
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
      Serial.println("Duplicate UID filtered: " + uid);
      return; // Skip duplicate
    }
    if (it->second.sent) {
      // Update timestamp but don't resend
      it->second.timestamp = currentTime;
      Serial.println("UID already sent, updating timestamp: " + uid);
      return;
    }
  }

  // New UID or retry needed
  Serial.println("Processing new UID: " + uid);
  
  // Add/update record
  UIDRecord record = {uid, currentTime, false};
  uidRecords[uid] = record;

  // Send to Google Sheets with retry mechanism
  bool success = false;
  for (int attempt = 1; attempt <= 3; attempt++) {
    Serial.println("Attempt " + String(attempt) + " to send UID: " + uid);
    
    if (sendToGoogleSheets(uid)) {
      Serial.println("✓ Successfully sent to Google Sheets on attempt " + String(attempt));
      uidRecords[uid].sent = true;
      success = true;
      break;
    } else {
      Serial.println("✗ Failed attempt " + String(attempt));
      if (attempt < 3) {
        delay(2000); // Wait before retry
      }
    }
  }
  
  if (!success) {
    Serial.println("✗ All attempts failed for UID: " + uid);
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
  http.addHeader("User-Agent", "ESP32-UHF-Reader");
  http.setTimeout(30000); // 30 second timeout for Google Apps Script
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  // Create JSON payload - simpler format
  String jsonString = "{\"uid\":\"" + uid + "\"}";
  
  Serial.println("Sending payload: " + jsonString);
  Serial.println("To URL: " + String(googleScriptURL));
  Serial.println("Payload length: " + String(jsonString.length()));
  
  // Send POST request
  int httpResponseCode = http.POST(jsonString);
  
  Serial.println("HTTP Response Code: " + String(httpResponseCode));
  
  bool success = false;
  String response = "";
  
  // Check response
  if (httpResponseCode > 0) {
    response = http.getString();
    Serial.println("Response length: " + String(response.length()));
    
    // Only show first 500 chars of response to avoid spam
    if (response.length() > 500) {
      Serial.println("Response Body (first 500 chars): " + response.substring(0, 500) + "...");
    } else {
      Serial.println("Response Body: " + response);
    }
    
    // Check if it's HTML error (starts with <!DOCTYPE or <html)
    if (response.startsWith("<!DOCTYPE") || response.startsWith("<html")) {
      Serial.println("ERROR: Received HTML error page instead of JSON");
      Serial.println("This usually means:");
      Serial.println("1. Wrong Google Apps Script URL");
      Serial.println("2. Apps Script not deployed properly");
      Serial.println("3. Apps Script has runtime errors");
      return false;
    }
    
    // Try to parse JSON response
    StaticJsonDocument<512> responseDoc;
    DeserializationError error = deserializeJson(responseDoc, response);
    
    if (error) {
      Serial.println("JSON parsing error: " + String(error.c_str()));
      // If we get 200 but can't parse, might still be success
      if (httpResponseCode == 200 && response.indexOf("success") != -1) {
        Serial.println("Treating as success based on 200 response code");
        success = true;
      }
    } else {
      success = responseDoc["success"] | false;
      if (responseDoc.containsKey("error")) {
        Serial.println("Server error: " + String(responseDoc["error"].as<const char*>()));
      }
      if (responseDoc.containsKey("message")) {
        Serial.println("Server message: " + String(responseDoc["message"].as<const char*>()));
      }
    }
  } else {
    Serial.println("HTTP Error: " + http.errorToString(httpResponseCode));
    Serial.println("WiFi Status: " + String(WiFi.status()));
    
    // Additional debugging for negative error codes
    if (httpResponseCode < 0) {
      Serial.println("Connection error - check:");
      Serial.println("1. Internet connectivity");
      Serial.println("2. Google Apps Script URL");
      Serial.println("3. DNS resolution");
    }
  }
  
  // Handle redirects as success (common with Google Apps Script)
  if (httpResponseCode == 302 || httpResponseCode == 301) {
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