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
const char* googleScriptURL = "https://script.google.com/macros/s/AKfycbzr8zm2aC3-X0aCeXYYpz-D0PX-eWloZIO0lFJA1sveImQrD0KWVeZ_6kDhBfiOFPYtTA/exec";

// NTP Client để lấy thời gian - Cải thiện cấu hình
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 7 * 3600, 60000);  // GMT+7 (Vietnam)

HardwareSerial ReaderSerial(2);  // Sử dụng ReaderSerial cho RX2/TX2

// Cấu trúc để lưu thông tin UID với timestamp
struct UIDRecord {
  String uid;
  unsigned long timestamp;
  bool sent;
  int retryCount;  // Đếm số lần retry
};

// Map để lưu trữ các UID với thời gian phát hiện
std::map<String, UIDRecord> uidRecords;

// Cấu hình thời gian - Tăng thời gian filter để tránh duplicate
const unsigned long DUPLICATE_FILTER_TIME = 10000;  // 10 giây
const unsigned long UID_CLEANUP_TIMEOUT = 300000;   // 5 phút
const unsigned long RETRY_INTERVAL = 15000;         // 15 giây
const int MAX_RETRY_COUNT = 3;                      // Số lần retry tối đa
unsigned long lastCleanupTime = 0;
unsigned long lastNTPUpdate = 0;
const unsigned long NTP_UPDATE_INTERVAL = 300000;  // Update NTP mỗi 5 phút

// Buffer để xử lý dữ liệu serial
String serialBuffer = "";
unsigned long lastDataTime = 0;
const unsigned long DATA_TIMEOUT = 100;  // 100ms timeout để xử lý complete packet

// Khai báo hàm trước
String getCurrentTime();
bool sendToGoogleSheets(String uid);
void processUID(String uid);
void cleanupAndRetryUIDs();
String extractFullUID(byte* buffer, int len);
bool isValidResponse(int httpCode, String response);
bool ensureNTPSync();

void setup() {
  Serial.begin(115200);
  ReaderSerial.begin(57600, SERIAL_8N1, 16, 17);  // RX2=GPIO16, TX2=GPIO17

  Serial.println("ESP32 UHF Reader to Google Sheets - Enhanced Version v2.0");

  // Kết nối WiFi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("Connecting to WiFi...");
  }
  Serial.println("WiFi connected!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  // Khởi tạo và đồng bộ NTP Client
  timeClient.begin();
  Serial.println("Initializing NTP Client...");

  // Đảm bảo NTP đồng bộ thành công trước khi tiếp tục
  if (ensureNTPSync()) {
    Serial.println("NTP synchronized successfully!");
    Serial.println("Current time: " + getCurrentTime());
  } else {
    Serial.println("Warning: NTP sync failed, but continuing...");
  }

  Serial.println("System ready - listening for UHF Reader data...");
  lastCleanupTime = millis();
  lastNTPUpdate = millis();
}

void loop() {
  // Cập nhật thời gian từ NTP server định kỳ
  if (millis() - lastNTPUpdate > NTP_UPDATE_INTERVAL) {
    timeClient.update();
    lastNTPUpdate = millis();
    Serial.println("NTP updated: " + getCurrentTime());
  }

  // Cleanup old UID records và retry gửi failed records
  cleanupAndRetryUIDs();

  // Xử lý dữ liệu serial với buffer để tránh incomplete packets
  while (ReaderSerial.available() > 0) {
    byte incomingByte = ReaderSerial.read();

    // Reset buffer nếu quá lâu không có dữ liệu
    if (millis() - lastDataTime > DATA_TIMEOUT && serialBuffer.length() > 0) {
      Serial.println("Buffer timeout, clearing incomplete data");
      serialBuffer = "";
    }

    // Thêm byte vào buffer (as hex)
    if (incomingByte < 0x10) serialBuffer += "0";
    serialBuffer += String(incomingByte, HEX);
    lastDataTime = millis();

    // Kiểm tra nếu có complete packet (16 hex chars = 8 bytes)
    if (serialBuffer.length() >= 16) {
      processSerialData(serialBuffer);
      serialBuffer = "";  // Clear buffer sau khi xử lý
    }
  }

  delay(10);  // Giảm delay để responsive hơn
}

bool ensureNTPSync() {
  Serial.println("Syncing with NTP server...");
  int attempts = 0;
  const int maxAttempts = 10;

  while (attempts < maxAttempts) {
    timeClient.update();
    unsigned long epochTime = timeClient.getEpochTime();

    // Kiểm tra nếu thời gian hợp lý (sau năm 2020)
    if (epochTime > 1577836800) {  // 01/01/2020 00:00:00 UTC
      Serial.println("NTP sync successful on attempt " + String(attempts + 1));
      return true;
    }

    attempts++;
    Serial.println("NTP sync attempt " + String(attempts) + "/" + String(maxAttempts));
    delay(2000);
  }

  Serial.println("NTP sync failed after " + String(maxAttempts) + " attempts");
  return false;
}

void processSerialData(String hexData) {
  // Convert hex string back to byte array for processing
  int len = hexData.length() / 2;
  byte buffer[len];

  for (int i = 0; i < len; i++) {
    String byteString = hexData.substring(i * 2, i * 2 + 2);
    buffer[i] = (byte)strtol(byteString.c_str(), NULL, 16);
  }

  // In dữ liệu thô để debug
  Serial.print("Processed packet (" + String(len) + " bytes): ");
  for (int i = 0; i < len; i++) {
    if (buffer[i] < 0x10) Serial.print("0");
    Serial.print(buffer[i], HEX);
    Serial.print(" ");
  }
  Serial.println();

  // Kiểm tra định dạng: length = 8, buffer[1] = 0, buffer[2] = 0xEE
  if (len == 8 && buffer[1] == 0x00 && buffer[2] == 0xEE) {
    // Trích xuất full UID từ packet
    String uid = extractFullUID(buffer, len);

    Serial.println("Detected UID: " + uid + " (Status: 0x" + String(buffer[3], HEX) + ")");

    // Xử lý UID với cơ chế chống trùng lặp cải tiến
    processUID(uid);

  } else {
    Serial.println("Invalid packet format - Expected: 8 bytes, [len] [0] [EE] [status] [data1] [data2] [crc1] [crc2]");
    if (len > 0) {
      Serial.println("Received: " + String(len) + " bytes, buffer[1]=0x" + String(buffer[1], HEX) + ", buffer[2]=0x" + String(buffer[2], HEX));
    }
  }
}

String extractFullUID(byte* buffer, int len) {
  // Trích xuất full UID - lấy tất cả data bytes có sẵn
  String uid = "";

  // Lấy tất cả data bytes từ buffer[4] đến buffer[len-3] (trừ 2 bytes CRC cuối)
  // Dựa trên packet: 07 00 EE 00 E2 80 48 50
  // buffer[4] = 0xE2, buffer[5] = 0x80, buffer[6] = 0x48, buffer[7] = 0x50
  // buffer[6] và buffer[7] có thể là CRC, nên chỉ lấy buffer[4] và buffer[5]
  // Hoặc lấy tất cả để có full UID

  for (int i = 4; i < len; i++) {  // Bỏ qua 2 bytes CRC cuối
    if (buffer[i] < 0x10) uid += "0";
    uid += String(buffer[i], HEX);
  }

  // Nếu muốn lấy full UID bao gồm cả CRC, uncomment dòng dưới:
  // for (int i = 4; i < len; i++) {
  //   if (buffer[i] < 0x10) uid += "0";
  //   uid += String(buffer[i], HEX);
  // }

  uid.toUpperCase();
  return uid;
}
void ensureWiFiConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected, reconnecting...");
    WiFi.begin(ssid, password);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      Serial.print(".");
      attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nWiFi reconnected!");
      Serial.println("IP: " + WiFi.localIP().toString());
    } else {
      Serial.println("\nFailed to reconnect WiFi");
    }
  }
}
void processUID(String uid) {
  unsigned long currentTime = millis();

  // Kiểm tra xem UID đã tồn tại trong records chưa
  auto it = uidRecords.find(uid);

  if (it != uidRecords.end()) {
    // UID đã tồn tại, kiểm tra thời gian
    UIDRecord& record = it->second;

    if (currentTime - record.timestamp < DUPLICATE_FILTER_TIME) {
      return;  // Trong thời gian filter, bỏ qua
    } else if (record.sent) {
      record.timestamp = currentTime;
      Serial.println("UID " + uid + " already sent successfully, filter updated");
      return;
    } else if (currentTime - record.timestamp < RETRY_INTERVAL) {
      return;  // Chưa đến thời gian retry
    } else if (record.retryCount >= MAX_RETRY_COUNT) {
      Serial.println("UID " + uid + " exceeded max retry count, removing");
      uidRecords.erase(it);
      return;
    }
  }

  // UID mới hoặc cần retry
  String timestamp = getCurrentTime();
  Serial.println("Processing UID: " + uid + " at " + timestamp);

  // Đảm bảo WiFi kết nối trước khi gửi
  ensureWiFiConnection();

  // Thêm/cập nhật record
  UIDRecord newRecord = { uid, currentTime, false, 0 };
  if (it != uidRecords.end()) {
    newRecord.retryCount = it->second.retryCount + 1;
    Serial.println("Retry attempt #" + String(newRecord.retryCount) + " for UID: " + uid);
  }
  uidRecords[uid] = newRecord;

  // Gửi lên Google Sheets
  if (sendToGoogleSheets(uid)) {
    Serial.println("✓ Data sent to Google Sheets successfully!");
    uidRecords[uid].sent = true;
  } else {
    Serial.println("✗ Failed to send data to Google Sheets - will retry later");
  }
}

void cleanupAndRetryUIDs() {
  unsigned long currentTime = millis();

  // Cleanup mỗi 30 giây
  if (currentTime - lastCleanupTime < 30000) {
    return;
  }

  lastCleanupTime = currentTime;

  // Cleanup old records và retry failed ones
  auto it = uidRecords.begin();
  while (it != uidRecords.end()) {
    UIDRecord& record = it->second;

    // Xóa records cũ đã gửi thành công
    if (record.sent && (currentTime - record.timestamp > UID_CLEANUP_TIMEOUT)) {
      Serial.println("Cleaned up old UID record: " + record.uid);
      it = uidRecords.erase(it);
      continue;
    }

    // Retry failed records
    if (!record.sent && (currentTime - record.timestamp >= RETRY_INTERVAL) && record.retryCount < MAX_RETRY_COUNT) {
      Serial.println("Auto-retrying failed UID: " + record.uid + " (attempt " + String(record.retryCount + 1) + ")");

      record.retryCount++;
      if (sendToGoogleSheets(record.uid)) {
        Serial.println("✓ Auto-retry successful for UID: " + record.uid);
        record.sent = true;
        record.timestamp = currentTime;  // Update timestamp
      } else {
        Serial.println("✗ Auto-retry failed for UID: " + record.uid);
        record.timestamp = currentTime;  // Update timestamp for next retry

        if (record.retryCount >= MAX_RETRY_COUNT) {
          Serial.println("Max retry attempts reached for UID: " + record.uid + ", removing from queue");
          it = uidRecords.erase(it);
          continue;
        }
      }
    }

    ++it;
  }

  // Log current status
  int totalRecords = uidRecords.size();
  int sentRecords = 0;
  int pendingRecords = 0;

  for (const auto& pair : uidRecords) {
    if (pair.second.sent) {
      sentRecords++;
    } else {
      pendingRecords++;
    }
  }

  if (totalRecords > 0) {
    Serial.println("UID Status - Total: " + String(totalRecords) + ", Sent: " + String(sentRecords) + ", Pending: " + String(pendingRecords));
  }
}

String getCurrentTime() {
  unsigned long epochTime = timeClient.getEpochTime();

  // Kiểm tra nếu thời gian không hợp lý
  if (epochTime < 1577836800) {  // Trước 01/01/2020
    Serial.println("Warning: Invalid time from NTP, attempting to resync...");
    timeClient.update();
    epochTime = timeClient.getEpochTime();
  }

  // Tạo ISO timestamp cho Google Sheets
  time_t rawTime = epochTime;
  struct tm* timeInfo = gmtime(&rawTime);  // Sử dụng GMT để tạo ISO format

  char isoString[25];
  sprintf(isoString, "%04d-%02d-%02dT%02d:%02d:%02dZ",
          timeInfo->tm_year + 1900,
          timeInfo->tm_mon + 1,
          timeInfo->tm_mday,
          timeInfo->tm_hour,
          timeInfo->tm_min,
          timeInfo->tm_sec);

  return String(isoString);
}

bool isValidResponse(int httpCode, String response) {
  // Cải thiện xử lý response codes
  if (httpCode == 200 || httpCode == 302) {
    // 302 từ Google Apps Script thường là thành công
    // Kiểm tra nếu response chứa error indicators
    if (response.indexOf("error") != -1 || response.indexOf("Error") != -1 || response.indexOf("Exception") != -1) {
      Serial.println("Response contains error indicators");
      return false;
    }

    // Nếu response là HTML redirect (302), coi như thành công
    if (httpCode == 302 && response.indexOf("<HTML>") != -1) {
      Serial.println("Received HTML redirect (302) - treating as success");
      return true;
    }

    // Kiểm tra JSON response
    if (response.indexOf("\"success\":true") != -1) {
      Serial.println("JSON response indicates success");
      return true;
    }

    // Default: nếu không có error indicators, coi như thành công
    return true;
  }

  Serial.println("Invalid HTTP response code: " + String(httpCode));
  return false;
}

// Chỉ cần sửa hàm sendToGoogleSheets trong code ESP32 của bạn:
bool sendToGoogleSheets(String uid) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected");
    return false;
  }
  
  HTTPClient http;
  http.begin(googleScriptURL);
  
  // Set headers - QUAN TRỌNG: Content-Type phải chính xác
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");
  http.addHeader("User-Agent", "ESP32-UHF-Reader/1.0");
  
  // Set timeout - tăng timeout cho Google Apps Script
  http.setTimeout(30000); // 30 giây
  
  // KHÔNG bật auto redirect cho Google Apps Script POST
  // Vì redirect có thể thay đổi method từ POST thành GET
  
  // Tạo JSON payload - đảm bảo format chính xác
  DynamicJsonDocument doc(256);
  doc["uid"] = uid;
  
  String jsonString;
  serializeJson(doc, jsonString);
  
  Serial.println("=== HTTP Request Details ===");
  Serial.println("URL: " + String(googleScriptURL));
  Serial.println("Payload: " + jsonString);
  Serial.println("Payload length: " + String(jsonString.length()));
  
  // Gửi POST request
  int httpResponseCode = http.POST(jsonString);
  
  Serial.println("=== HTTP Response Details ===");
  Serial.println("Response Code: " + String(httpResponseCode));
  
  if (httpResponseCode > 0) {
    String response = http.getString();
    Serial.println("Response length: " + String(response.length()));
    
    // In response với giới hạn để debug
    if (response.length() > 500) {
      Serial.println("Response (first 500 chars): " + response.substring(0, 500) + "...");
    } else {
      Serial.println("Full Response: " + response);
    }
    
    // Xử lý các response codes
    if (httpResponseCode == 200) {
      // Parse JSON response
      DynamicJsonDocument responseDoc(1024);
      DeserializationError error = deserializeJson(responseDoc, response);
      
      if (!error) {
        // Response là JSON hợp lệ
        bool success = responseDoc["success"] | false;
        String message = responseDoc["message"] | "";
        String errorMsg = responseDoc["error"] | "";
        
        Serial.println("JSON Response parsed successfully");
        Serial.println("Success: " + String(success ? "true" : "false"));
        
        if (message.length() > 0) {
          Serial.println("Message: " + message);
        }
        if (errorMsg.length() > 0) {
          Serial.println("Error: " + errorMsg);
        }
        
        http.end();
        return success;
        
      } else {
        Serial.println("JSON parse error: " + String(error.c_str()));
        
        // Nếu không parse được JSON nhưng HTTP 200, kiểm tra content
        if (response.indexOf("success") != -1 && 
            response.indexOf("true") != -1 &&
            response.indexOf("error") == -1) {
          Serial.println("Response appears successful despite JSON parse error");
          http.end();
          return true;
        }
        
        Serial.println("Response does not indicate success");
        http.end();
        return false;
      }
      
    } else if (httpResponseCode == 302) {
      // Handle redirect - Google Apps Script thường redirect
      String location = http.header("Location");
      Serial.println("Received redirect to: " + location);
      
      // Đối với Google Apps Script, 302 thường có nghĩa là thành công
      // nhưng cần kiểm tra response content
      if (response.indexOf("error") == -1 && response.indexOf("Error") == -1) {
        Serial.println("Treating 302 as success (no error indicators found)");
        http.end();
        return true;
      } else {
        Serial.println("302 response contains error indicators");
        http.end();
        return false;
      }
      
    } else if (httpResponseCode >= 400) {
      // Client/Server errors
      Serial.println("HTTP error " + String(httpResponseCode));
      
      // Kiểm tra xem có phải HTML error page không
      if (response.indexOf("<!DOCTYPE html>") != -1 || 
          response.indexOf("<html") != -1) {
        Serial.println("Received HTML error page");
        
        // Trích xuất title từ HTML để biết lỗi gì
        int titleStart = response.indexOf("<title>");
        int titleEnd = response.indexOf("</title>");
        if (titleStart != -1 && titleEnd != -1) {
          String title = response.substring(titleStart + 7, titleEnd);
          Serial.println("Error page title: " + title);
        }
      }
      
      http.end();
      return false;
      
    }
    
  } else {
    // HTTP request failed completely
    Serial.println("HTTP request failed with code: " + String(httpResponseCode));
    
    // In ra lỗi chi tiết
    String error = http.errorToString(httpResponseCode);
    Serial.println("Error details: " + error);
    
    // Kiểm tra một số lỗi phổ biến
    if (httpResponseCode == HTTPC_ERROR_CONNECTION_REFUSED) {
      Serial.println("Connection refused - check URL and network");
    } else if (httpResponseCode == HTTPC_ERROR_SEND_HEADER_FAILED) {
      Serial.println("Send header failed - check headers");
    } else if (httpResponseCode == HTTPC_ERROR_SEND_PAYLOAD_FAILED) {
      Serial.println("Send payload failed - check JSON format");
    } else if (httpResponseCode == HTTPC_ERROR_NOT_CONNECTED) {
      Serial.println("Not connected - check WiFi");
    } else if (httpResponseCode == HTTPC_ERROR_CONNECTION_LOST) {
      Serial.println("Connection lost during request");
    } else if (httpResponseCode == HTTPC_ERROR_READ_TIMEOUT) {
      Serial.println("Read timeout - server too slow");
    }
  }
  
  http.end();
  return false;
}

// Thêm function để test HTTP connection
void testHTTPConnection() {
  Serial.println("=== Testing HTTP Connection ===");
  
  HTTPClient http;
  http.begin(googleScriptURL);
  
  // Test với GET request trước
  Serial.println("Testing with GET request...");
  int getResponse = http.GET();
  Serial.println("GET Response Code: " + String(getResponse));
  
  if (getResponse > 0) {
    String getResponseString = http.getString();
    Serial.println("GET Response (first 200 chars): " + 
                   getResponseString.substring(0, min(200, (int)getResponseString.length())));
  }
  
  http.end();
  
  // Test với POST request và payload rỗng
  Serial.println("Testing with empty POST request...");
  http.begin(googleScriptURL);
  http.addHeader("Content-Type", "application/json");
  
  int postResponse = http.POST("{}");
  Serial.println("Empty POST Response Code: " + String(postResponse));
  
  if (postResponse > 0) {
    String postResponseString = http.getString();
    Serial.println("Empty POST Response (first 200 chars): " + 
                   postResponseString.substring(0, min(200, (int)postResponseString.length())));
  }
  
  http.end();
  Serial.println("=== HTTP Connection Test Complete ===");
}
