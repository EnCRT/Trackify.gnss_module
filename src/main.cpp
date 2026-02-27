#include <Arduino.h>
#include <U8g2lib.h>
#include <TinyGPS++.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <WebServer.h>
#include <OneButton.h>
#include <Adafruit_NeoPixel.h>
#include <qrcode.h>
#include "wifi_manager.h"

// --- Pin Definitions (SAFE for Heltec V3) ---
#define OLED_RST    21
#define VEXT_PIN    36
#define GPS_RX      4
#define GPS_TX      5
#define SD_MOSI     40
#define SD_MISO     41
#define SD_SCK      42
#define SD_CS       39
#define BTN_PIN     1
#define LED_PIN     38
#define LED_COUNT   1

// --- Objects ---
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, OLED_RST, 18, 17);
TinyGPSPlus gps;
HardwareSerial ss(1);
OneButton button(BTN_PIN, true);
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
WiFiManager wifiManager;

// --- State Machine ---
enum DeviceState { STATE_IDLE, STATE_READY, STATE_LOGGING, STATE_WIFI_AP };
DeviceState currentState = STATE_IDLE;

bool isLogging = false;
bool sdDetected = false;
bool gpsCommunicating = false;
unsigned long loggingStartTime = 0;
uint8_t uiPage = 0;
unsigned long lastPageSwitch = 0;
File logFile;
char currentFileName[32];

// --- LED Variables ---
uint32_t lastLedColor = 0xFFFFFFFF; // Track current LED color to minimize strip.show()

// --- QR Code drawing function ---
void drawQRCode(const char* text, int xOffset, int yOffset) {
  QRCode qrcode;
  uint8_t qrcodeData[qrcode_getBufferSize(3)];
  qrcode_initText(&qrcode, qrcodeData, 3, 0, text);

  int size = qrcode.size;
  int scale = 2; // Each QR module is 2x2 pixels
  
  for (uint8_t y = 0; y < size; y++) {
    for (uint8_t x = 0; x < size; x++) {
      if (qrcode_getModule(&qrcode, x, y)) {
        u8g2.drawBox(xOffset + x * scale, yOffset + y * scale, scale, scale);
      }
    }
  }
}

// --- GPS Frequency Configuration ---
#define GPS_FREQ_HZ 25 // Change to 25 for 25Hz operation

// --- GPS UBX Commands ---
// 10Hz (100ms)
const uint8_t UBX_CFG_RATE_10HZ[] = {
  0xB5, 0x62, 0x06, 0x08, 0x06, 0x00, 0x64, 0x00, 0x01, 0x00, 0x01, 0x00, 0x7A, 0x12
};

// 25Hz (40ms)
const uint8_t UBX_CFG_RATE_25HZ[] = {
  0xB5, 0x62, 0x06, 0x08, 0x06, 0x00, 0x28, 0x00, 0x01, 0x00, 0x01, 0x00, 0x3E, 0xAA
};

void sendUBX(const uint8_t *msg, uint8_t len) {
  for (uint8_t i = 0; i < len; i++) {
    ss.write(msg[i]);
  }
}

void setupGPS() {
  Serial.print("Initializing GPS at ");
  Serial.print(GPS_FREQ_HZ);
  Serial.println("Hz...");
  ss.begin(115200, SERIAL_8N1, GPS_RX, GPS_TX);
  delay(100);

  #if GPS_FREQ_HZ == 25
    sendUBX(UBX_CFG_RATE_25HZ, sizeof(UBX_CFG_RATE_25HZ));
  #else
    sendUBX(UBX_CFG_RATE_10HZ, sizeof(UBX_CFG_RATE_10HZ));
  #endif
}

void setupSD() {
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (SD.begin(SD_CS, SPI, 20000000)) {
    sdDetected = true;
    Serial.println("SD card detected (20MHz).");
  } else {
    sdDetected = false;
    Serial.println("SD card NOT detected.");
  }
}

void startLogging() {
  if (gps.satellites.value() < 4 || !sdDetected) return;
  
  int n = 0;
  do {
    sprintf(currentFileName, "/log_%03d.txt", n++);
  } while (SD.exists(currentFileName));

  logFile = SD.open(currentFileName, FILE_WRITE);
  if (logFile) {
    isLogging = true;
    currentState = STATE_LOGGING;
    loggingStartTime = millis();
    Serial.print("Started logging to: "); Serial.println(currentFileName);
  } else {
    Serial.println("Failed to open log file!");
  }
}

void stopLogging() {
  if (isLogging) {
    logFile.flush();
    logFile.close();
    isLogging = false;
    currentState = STATE_READY;
    Serial.println("Logging stopped and file closed.");
  }
}

void stopWiFi() {
  wifiManager.stop();
  currentState = STATE_IDLE;
  Serial.println("WiFi AP Stopped. Restarting GPS.");
  setupGPS(); // Restore GPS serial connection and configuration
}

void handleButton() {
  Serial.println("Button Clicked!");
  if (currentState == STATE_WIFI_AP) {
    stopWiFi();
    return;
  }

  if (isLogging) {
    stopLogging();
  } else {
    bool hasFix = gps.location.isValid() && gps.location.age() < 2000 && gps.satellites.value() >= 8;
    if (hasFix) {
      startLogging();
    } else {
      Serial.print("Cannot start logging. Fix: "); Serial.print(gps.location.isValid());
      Serial.print(" Sats: "); Serial.println(gps.satellites.value());
    }
  }
}

void startWiFi() {
  if (isLogging) stopLogging();

  // Shut down GPS hardware serial to prioritize WiFi/SPI...
  Serial.println("Shutting down GPS to prioritize WiFi/SPI...");
  ss.end();
  
  wifiManager.begin();
  currentState = STATE_WIFI_AP;

  // Draw WiFi status once and stop OLED updates
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 10, "WiFi:");
  u8g2.drawStr(0, 30, "Trackify");
  u8g2.drawStr(0, 40, "12345678");
  drawQRCode("WIFI:S:Trackify;T:WPA;P:12345678;;", 65, 5);
  u8g2.sendBuffer();
}

  // Draw WiFi status once and stop OLED updates
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 10, "WiFi:");
  u8g2.drawStr(0, 30, "Trackify");
  u8g2.drawStr(0, 40, "12345678");
  drawQRCode("WIFI:S:Trackify;T:WPA;P:12345678;;", 65, 5);
  u8g2.sendBuffer();
}

void updateLED() {
  uint32_t targetColor = 0;

  if (digitalRead(BTN_PIN) == LOW) {
    targetColor = strip.Color(255, 0, 0); // Solid Red on press
  } else {
    switch (currentState) {
      case STATE_IDLE:    targetColor = strip.Color(255, 165, 0); break; // static Orange
      case STATE_READY:   targetColor = strip.Color(255, 255, 255);   break; // static White
      case STATE_LOGGING: targetColor = strip.Color(255, 0, 0);   break; // static Red
      case STATE_WIFI_AP: targetColor = strip.Color(0, 0, 255);   break; // static Blue
    }
  }

  // Only update hardware if color changed
  if (targetColor != lastLedColor) {
    strip.setPixelColor(0, targetColor);
    strip.show();
    lastLedColor = targetColor;
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(VEXT_PIN, OUTPUT);
  digitalWrite(VEXT_PIN, LOW); 
  delay(100);

  u8g2.begin();
  u8g2.setContrast(255);
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB08_tr);
  u8g2.drawStr(0, 30, "SYSTEM STARTING...");
  u8g2.sendBuffer();

  setupGPS();
  setupSD();
  
  strip.begin();
  strip.setBrightness(50);
  strip.show();

  button.attachClick(handleButton);
  button.attachLongPressStop(startWiFi);
  button.setLongPressIntervalMs(4000);
}

// --- Icons (XBM 10x10) ---
static const unsigned char icon_gps_bits[] = {
  0x10, 0x00, 0x38, 0x00, 0x7c, 0x00, 0xfe, 0x00, 0x10, 0x00, 
  0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x38, 0x00, 0x7c, 0x00
};
static const unsigned char icon_sd_bits[] = {
  0xfe, 0x00, 0xfe, 0x01, 0x02, 0x02, 0xaa, 0x02, 0x02, 0x02, 
  0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0xfe, 0x03, 0xfe, 0x03
};

void loop() {
  button.tick();
  
  // WiFi mode optimization: suspend all other tasks
  if (currentState == STATE_WIFI_AP) {
    wifiManager.handle();
    updateLED();
    vTaskDelay(2 / portTICK_PERIOD_MS); 
    return;
  }

  while (ss.available() > 0) {
    char c = ss.read();
    gps.encode(c);
    if (isLogging) logFile.write(c);
  }

  // Check GPS communication
  if (gps.charsProcessed() > 10) gpsCommunicating = true;

  bool hasFix = gps.location.isValid() && gps.location.age() < 2000 && gps.satellites.value() >= 8;
  if (hasFix && currentState == STATE_IDLE) {
    currentState = STATE_READY;
  } else if (!hasFix && currentState == STATE_READY) {
    currentState = STATE_IDLE;
  }

  // --- SINGLE CONSOLIDATED UI ---
  u8g2.clearBuffer();
  
  // 1. TOP BAR: Branding + Icons
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 10, "Trackify");
  u8g2.drawXBM(90, 1, 10, 10, icon_gps_bits);
  u8g2.setCursor(102, 10); u8g2.print(gpsCommunicating ? "+" : "-");
  u8g2.drawXBM(110, 1, 10, 10, icon_sd_bits);
  u8g2.setCursor(122, 10); u8g2.print(sdDetected ? "+" : "-");
  u8g2.drawHLine(0, 13, 128);

  // 2. MAIN CONTENT: Speed and Sats
  u8g2.setCursor(0, 25);
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.print("Sats: "); u8g2.print(gps.satellites.value());

  u8g2.setCursor(0, 50);
  u8g2.setFont(u8g2_font_ncenB18_tr);
  if (gps.speed.isValid()) {
    u8g2.print(gps.speed.kmph(), 1);
  } else {
    u8g2.print("0.0");
  }
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.print(" km/h");

  // 3. FOOTER: Mode or Timer
  if (currentState == STATE_LOGGING) {
    unsigned long elapsed = (millis() - loggingStartTime) / 1000;
    int h = elapsed / 3600;
    int m = (elapsed % 3600) / 60;
    int s = elapsed % 60;
    char timeStr[20];
    sprintf(timeStr, "REC [%02d:%02d:%02d]", h, m, s);
    u8g2.drawStr(60, 25, timeStr);
  } else {
    const char* modeText = (currentState == STATE_READY) ? "READY" : "WAIT FIX";
    u8g2.drawStr(80, 25, modeText);
  }

  if (!sdDetected) {
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 64, "!!! NO SD CARD !!!");
  }

  u8g2.sendBuffer();
  updateLED();
}
