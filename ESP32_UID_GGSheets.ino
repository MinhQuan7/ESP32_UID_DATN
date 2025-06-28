#include "Arduino.h"
#include <HardwareSerial.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <map>

// WiFi credentials
const char* ssid = "eoh.ioo";
const char* password = "Eoh@2020";

// Google Apps Script URL
const char* scriptURL = "https://script.google.com/macros/s/AKfycbwJyAdxmLUjGNTXdB7Gg4xL7LMsilLUPSPY8fc-h4ip6mkfPrM4SsftdkrOkR7J2PSq/exec";

HardwareSerial ReaderSerial(2);  // Use ReaderSerial for RX2/TX2

// Anti-spam mechanism using std::map for UID tracking
std::map<String, unsigned long> lastReadTime;
const unsigned long ANTI_SPAM_INTERVAL = 5000;  // 5 seconds anti-spam interval

// Timer variables
unsigned long lastCheckTime = 0;
const unsigned long CHECK_INTERVAL = 100;  // Check every 100ms

void setup() {
  Serial.begin(115200);                           // For USB debug
  ReaderSerial.begin(57600, SERIAL_8N1, 16, 17);  // Baud rate 57600, 8N1, RX2=GPIO16, TX2=GPIO17
  
  Serial.println("ESP32 UHF Reader starting...");
  
  // Initialize WiFi connection
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("Connecting to WiFi...");
  }
  Serial.println("WiFi connected successfully");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

void loop() {
  // Timer-based checking instead of delay
  unsigned long currentTime = millis();
  
  if (currentTime - lastCheckTime >= CHECK_INTERVAL) {
    lastCheckTime = currentTime;
    
    if (ReaderSerial.available()) {
      processReaderData();
    }
    
    // Clean up old entries from anti-spam map (optional optimization)
    cleanupOldEntries(currentTime);
  }
}
String extractUID(byte* buffer, byte dataLen) {
  String uid = "";
  
  // CHỈNH SỬA: In log để debug
  Serial.print("Raw data (hex): ");
  for (int i = 0; i < dataLen + 4; i++) {
    if(buffer[i] < 0x10) Serial.print("0");
    Serial.print(buffer[i], HEX);
    Serial.print(" ");
  }
  Serial.println();

  // Trích xuất UID dạng hex
  for (int i = 4; i < 4 + dataLen; i++) {
    if (buffer[i] < 0x10) uid += "0";
    uid += String(buffer[i], HEX);
  }
  uid.toUpperCase();
  
  // CHỈNH SỬA QUAN TRỌNG: Cắt bỏ byte cuối nếu cần
  if(uid.length() == 26) {
    uid = uid.substring(0, 24); // Giữ lại 12 byte đầu
    Serial.println("Trimmed UID: " + uid);
  }
  return uid;
}
void processReaderData() {
  byte buffer[32];  // Buffer large enough to contain packet
  int len = ReaderSerial.readBytes(buffer, sizeof(buffer));
  
  // Print raw data for debugging
  Serial.print("Raw data received: ");
  for (int i = 0; i < len; i++) {
    if (buffer[i] < 0x10) Serial.print("0");  // Add leading zero for single digit hex
    Serial.print(buffer[i], HEX);
    Serial.print(" ");
  }
  Serial.println();
  
  // Validate and parse packet

  if (len >= 5 && buffer[2] == 0xEE) {
    byte dataLen = buffer[0] - 4;
    
    Serial.println("Packet length: " + String(buffer[0]) + ", Data length: " + String(dataLen));
    
  if (dataLen >= 1) {
    String uid = extractUID(buffer, dataLen);
    
    // CHỈNH SỬA: Nhận cả UID 24 và 26 ký tự
    if (uid.length() == 24 || uid.length() == 26) { 
      // Chuẩn hóa thành 24 ký tự nếu cần
      if(uid.length() == 26) uid = uid.substring(0, 24);
      
      Serial.println("Valid UID: " + uid);
        
        // Kiểm tra chống spam
        if (shouldProcessUID(uid)) {
          Serial.println("Sending UID to Google Sheets: " + uid);
          sendToGoogleSheets(uid);
          
          // Cập nhật thời gian đọc cuối
          lastReadTime[uid] = millis();
        } else {
          Serial.println("UID blocked by anti-spam mechanism: " + uid);
        }
      } else {
        Serial.println("Invalid UID length: " + String(uid.length()) + " characters (expected 26)");
      }
    }
  } else {
    Serial.println("Invalid packet format");
  }
}


bool shouldProcessUID(String uid) {
  // UID 13 byte sẽ dài hơn nên cần kiểm tra riêng
  // if (uid.length() != 26) return false;
  
  unsigned long currentTime = millis();
  
  // Kiểm tra nếu UID đã được đọc trước đó
  if (lastReadTime.find(uid) != lastReadTime.end()) {
    unsigned long timeDiff = currentTime - lastReadTime[uid];
    return timeDiff >= ANTI_SPAM_INTERVAL;
  }
  
  // Lần đầu đọc UID này
  return true;
}

void sendToGoogleSheets(String uid) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(scriptURL);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    
    // Prepare POST data
    String postData = "uid=" + uid;
    
    Serial.println("Sending POST request to: " + String(scriptURL));
    Serial.println("POST data: " + postData);
    
    int httpResponseCode = http.POST(postData);
    
    if (httpResponseCode > 0) {
      String response = http.getString();
      Serial.println("HTTP Response code: " + String(httpResponseCode));
      Serial.println("Response: " + response);
    } else {
      Serial.println("HTTP POST failed with error: " + String(httpResponseCode));
    }
    
    http.end();
  } else {
    Serial.println("WiFi not connected - cannot send data");
  }
}

void cleanupOldEntries(unsigned long currentTime) {
  // Remove entries older than 1 hour to prevent memory issues
  const unsigned long CLEANUP_THRESHOLD = 3600000;  // 1 hour in milliseconds
  
  auto it = lastReadTime.begin();
  while (it != lastReadTime.end()) {
    if (currentTime - it->second > CLEANUP_THRESHOLD) {
      it = lastReadTime.erase(it);
    } else {
      ++it;
    }
  }
}