#include <Arduino.h>
#include <U8g2lib.h>
#include <TinyGPS++.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <OneButton.h>
#include <Adafruit_NeoPixel.h>
#include <qrcode.h>

// Undefine conflicting macros before including SdFat
#ifdef FILE_READ
#undef FILE_READ
#endif
#ifdef FILE_WRITE
#undef FILE_WRITE
#endif

#include "SdFat.h"
#include "sdios.h"
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

// --- GPS Frequency Configuration ---
#define GPS_FREQ_HZ 25 // Change to 10 for 10Hz operation

// --- UART Buffer Size (protection against overflow at 25Hz) ---
#define GPS_UART_BUFFER_SIZE 2048

// --- SD Write Buffer (sector-aligned for optimal performance) ---
#define SD_WRITE_BUFFER_SIZE 512
#define SD_FLUSH_INTERVAL_MS 200

// --- FreeRTOS Task Configuration ---
#define GPS_TASK_STACK_SIZE  8192
#define GPS_TASK_PRIORITY    5
#define GPS_TASK_CORE        1

#define UI_TASK_STACK_SIZE   8192
#define UI_TASK_PRIORITY     2
#define UI_TASK_CORE         0

// --- Objects ---
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, OLED_RST, 18, 17);
TinyGPSPlus gps;
HardwareSerial ss(1);
OneButton button(BTN_PIN, true);
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
WiFiManager wifiManager;
SdFs sd;

// --- State Machine ---
enum DeviceState { STATE_IDLE, STATE_READY, STATE_PREALLOCATING, STATE_LOGGING, STATE_WIFI_AP };
volatile DeviceState currentState = STATE_IDLE;

volatile bool isLogging = false;
bool sdDetected = false;
unsigned long loggingStartTime = 0;
FsFile logFile;
char currentFileName[32];

// --- Shared GPS Data (protected by mutex) ---
struct SharedGpsData {
  uint32_t satellites;
  double speed;
  bool speedValid;
  bool hasFix;
  bool gpsCommunicating;
};

volatile SharedGpsData sharedGpsData = {0, 0.0, false, false, false};
SemaphoreHandle_t gpsMutex = NULL;

// --- Inter-core Command Queue ---
enum Command { CMD_START_LOGGING, CMD_STOP_LOGGING, CMD_START_WIFI, CMD_STOP_WIFI };
QueueHandle_t commandQueue = NULL;

// --- SD Write Buffer ---
static uint8_t sdWriteBuffer[SD_WRITE_BUFFER_SIZE];
static volatile size_t sdBufPos = 0;
static unsigned long lastFlushTime = 0;

// --- LED Variables ---
uint32_t lastLedColor = 0xFFFFFFFF;

// --- FreeRTOS Task Handles ---
TaskHandle_t gpsTaskHandle = NULL;
TaskHandle_t uiTaskHandle = NULL;

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
  Serial.print("[GPS] Initializing at ");
  Serial.print(GPS_FREQ_HZ);
  Serial.println("Hz...");
  ss.begin(115200, SERIAL_8N1, GPS_RX, GPS_TX);
  ss.setRxBufferSize(GPS_UART_BUFFER_SIZE);
  delay(100);

  #if GPS_FREQ_HZ == 25
    sendUBX(UBX_CFG_RATE_25HZ, sizeof(UBX_CFG_RATE_25HZ));
  #else
    sendUBX(UBX_CFG_RATE_10HZ, sizeof(UBX_CFG_RATE_10HZ));
  #endif
}

void setupSD() {
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (sd.begin(SdSpiConfig(SD_CS, SHARED_SPI, SD_SCK_MHZ(20), &SPI))) {
    sdDetected = true;
    Serial.println("[SD] Card detected (SdFat 20MHz).");
  } else {
    sdDetected = false;
    Serial.println("[SD] Card NOT detected.");
  }
}

// --- SD Buffer Flush (called from GPS Task only) ---
void flushSdBuffer() {
  if (sdBufPos > 0) {
    logFile.write(sdWriteBuffer, sdBufPos);
    sdBufPos = 0;
  }
}

// --- Logging Control (called from GPS Task only) ---
void startLogging() {
  // Read shared GPS data safely
  uint32_t sats = 0;
  bool hasFix = false;
  if (xSemaphoreTake(gpsMutex, pdMS_TO_TICKS(10))) {
    sats = sharedGpsData.satellites;
    hasFix = sharedGpsData.hasFix;
    xSemaphoreGive(gpsMutex);
  }

  if (sats < 4 || !sdDetected || !hasFix) {
    Serial.println("[GPS] Cannot start logging — no fix or no SD.");
    return;
  }

  int n = 0;
  do {
    sprintf(currentFileName, "/log_%03d.txt", n++);
  } while (sd.exists(currentFileName));

  if (logFile.open(currentFileName, O_WRONLY | O_CREAT | O_TRUNC)) {
    currentState = STATE_PREALLOCATING;
    Serial.print("[GPS] Creating file: "); Serial.println(currentFileName);
    Serial.println("[GPS] Starting pre-allocation (200MB)...");

    const uint64_t PRE_ALLOC_SIZE = 200ULL * 1024 * 1024;
    if (logFile.preAllocate(PRE_ALLOC_SIZE)) {
      isLogging = true;
      currentState = STATE_LOGGING;
      loggingStartTime = millis();
      sdBufPos = 0;
      lastFlushTime = millis();
      Serial.println("[GPS] Pre-allocation successful. Logging started.");
    } else {
      Serial.println("[GPS] Pre-allocation FAILED!");
      logFile.close();
      currentState = STATE_READY;
    }
  } else {
    Serial.println("[GPS] Failed to open log file!");
  }
}

void stopLogging() {
  if (isLogging || currentState == STATE_PREALLOCATING) {
    if (isLogging) {
      flushSdBuffer(); // Flush remaining buffer
      logFile.truncate(); // Truncate to actual written size
    }
    logFile.close();
    isLogging = false;
    sdBufPos = 0;
    currentState = STATE_READY;
    Serial.println("[GPS] Logging stopped and file closed.");
  }
}

// ========================================================
// GPS Task — runs on Core 1, high priority
// Handles: UART reading, TinyGPSPlus parsing, SD writing
// ========================================================
void gpsTask(void *param) {
  Serial.print("[GPS] Task started on Core ");
  Serial.println(xPortGetCoreID());

  for (;;) {
    // --- 1. Process inter-core commands ---
    Command cmd;
    if (xQueueReceive(commandQueue, &cmd, 0) == pdTRUE) {
      switch (cmd) {
        case CMD_START_LOGGING:
          startLogging();
          break;
        case CMD_STOP_LOGGING:
          stopLogging();
          break;
        case CMD_START_WIFI:
          // Stop logging if active, then shut down GPS UART
          if (isLogging) stopLogging();
          Serial.println("[GPS] Shutting down GPS for WiFi mode...");
          ss.end();
          wifiManager.begin(GPS_FREQ_HZ);
          currentState = STATE_WIFI_AP;
          break;
        case CMD_STOP_WIFI:
          wifiManager.stop();
          currentState = STATE_IDLE;
          Serial.println("[GPS] WiFi stopped. Restarting GPS.");
          setupGPS();
          break;
      }
    }

    // --- 2. Read GPS UART and write to SD ---
    if (currentState != STATE_WIFI_AP) {
      // Periodic SD check if not detected
      static uint32_t lastSdCheck = 0;
      if (!sdDetected && (millis() - lastSdCheck) > 10000) {
        lastSdCheck = millis();
        if (sd.begin(SdSpiConfig(SD_CS, SHARED_SPI, SD_SCK_MHZ(20), &SPI))) {
          sdDetected = true;
          Serial.println("[SD] Card detected (Hot-plug).");
        }
      }

      int available = ss.available();
      while (available-- > 0) {
        char c = ss.read();
        gps.encode(c);

        if (isLogging) {
          sdWriteBuffer[sdBufPos++] = (uint8_t)c;
          if (sdBufPos >= SD_WRITE_BUFFER_SIZE) {
            logFile.write(sdWriteBuffer, SD_WRITE_BUFFER_SIZE);
            sdBufPos = 0;
            lastFlushTime = millis();
          }
        }
      }

      // Periodic flush of partial buffer
      if (isLogging && sdBufPos > 0 && (millis() - lastFlushTime) >= SD_FLUSH_INTERVAL_MS) {
        flushSdBuffer();
        lastFlushTime = millis();
      }

      // --- 3. Update shared GPS data under mutex ---
      bool hasFix = gps.location.isValid() && gps.location.age() < 2000 && gps.satellites.value() >= 8;
      bool communicating = gps.charsProcessed() > 10;

      if (xSemaphoreTake(gpsMutex, pdMS_TO_TICKS(5))) {
        sharedGpsData.satellites = gps.satellites.value();
        sharedGpsData.speed = gps.speed.kmph();
        sharedGpsData.speedValid = gps.speed.isValid();
        sharedGpsData.hasFix = hasFix;
        sharedGpsData.gpsCommunicating = communicating;
        xSemaphoreGive(gpsMutex);
      }

      // Update device state based on fix and SD detection
      if (hasFix && sdDetected && currentState == STATE_IDLE) {
        currentState = STATE_READY;
      } else if ((!hasFix || !sdDetected) && currentState == STATE_READY) {
        currentState = STATE_IDLE;
      }
    }

    // Minimal yield — 1 tick (~1ms) is enough for 25Hz GPS
    vTaskDelay(1);
  }
}

// --- QR Code drawing function ---
void drawQRCode(const char* text, int xOffset, int yOffset) {
  QRCode qrcode;
  uint8_t qrcodeData[qrcode_getBufferSize(3)];
  qrcode_initText(&qrcode, qrcodeData, 3, 0, text);

  int size = qrcode.size;
  int scale = 2;
  
  for (uint8_t y = 0; y < size; y++) {
    for (uint8_t x = 0; x < size; x++) {
      if (qrcode_getModule(&qrcode, x, y)) {
        u8g2.drawBox(xOffset + x * scale, yOffset + y * scale, scale, scale);
      }
    }
  }
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

// --- LED Update (called from UI Task) ---
void updateLED() {
  uint32_t targetColor = 0;
  uint32_t baseColor = 0;
  bool shouldFlash = false;

  // Define double flash pattern: 2 pulses of 50ms every 2 seconds
  uint32_t ms = millis() % 2000;
  bool flashActive = (ms < 50) || (ms > 200 && ms < 250);

  if (digitalRead(BTN_PIN) == LOW) {
    targetColor = strip.Color(255, 0, 0); // Solid Red on press
  } else {
    switch (currentState) {
      case STATE_IDLE:          
        baseColor = strip.Color(255, 165, 0); // Orange
        shouldFlash = true;
        break;
      case STATE_READY:         
        baseColor = strip.Color(255, 255, 255); // White
        shouldFlash = true;
        break;
      case STATE_WIFI_AP:       
        baseColor = strip.Color(0, 0, 255); // Blue
        shouldFlash = true;
        break;
      case STATE_PREALLOCATING: 
        baseColor = strip.Color(255, 0, 0); // Red
        shouldFlash = true;
        break;
      case STATE_LOGGING:       
        targetColor = strip.Color(255, 0, 0); // Solid Red
        shouldFlash = false;
        break;
    }

    if (shouldFlash) {
      targetColor = flashActive ? baseColor : 0;
    }
  }

  if (targetColor != lastLedColor) {
    strip.setPixelColor(0, targetColor);
    strip.show();
    lastLedColor = targetColor;
  }
}

// ========================================================
// LED Task — runs on Core 0, low priority but fast poll
// Handles: Smooth LED status indication
// ========================================================
void ledTask(void *param) {
  for (;;) {
    updateLED();
    vTaskDelay(pdMS_TO_TICKS(20)); // Poll every 20ms for precise flashes
  }
}

// --- Button Handlers (run on UI Task / Core 0, send commands to GPS Task) ---
void handleButton() {
  Serial.println("[UI] Button Clicked!");
  if (currentState == STATE_WIFI_AP) {
    Command cmd = CMD_STOP_WIFI;
    xQueueSend(commandQueue, &cmd, pdMS_TO_TICKS(100));
    return;
  }

  if (isLogging || currentState == STATE_PREALLOCATING) {
    Command cmd = CMD_STOP_LOGGING;
    xQueueSend(commandQueue, &cmd, pdMS_TO_TICKS(100));
  } else {
    // Check fix state from shared data
    bool hasFix = false;
    if (xSemaphoreTake(gpsMutex, pdMS_TO_TICKS(10))) {
      hasFix = sharedGpsData.hasFix;
      xSemaphoreGive(gpsMutex);
    }

    if (hasFix) {
      Command cmd = CMD_START_LOGGING;
      xQueueSend(commandQueue, &cmd, pdMS_TO_TICKS(100));
    } else {
      Serial.println("[UI] Cannot start logging — no GPS fix.");
    }
  }
}

void handleLongPress() {
  Serial.println("[UI] Long press — starting WiFi AP.");
  Command cmd = CMD_START_WIFI;
  xQueueSend(commandQueue, &cmd, pdMS_TO_TICKS(100));
}

// --- OLED Update (called from UI Task) ---
void updateOLED() {
  // In WiFi mode, draw WiFi screen once (static)
  if (currentState == STATE_WIFI_AP) {
    // WiFi screen is drawn once when entering WiFi mode, skip updates
    return;
  }

  // Read shared GPS data under mutex
  uint32_t sats = 0;
  double speed = 0.0;
  bool speedValid = false;
  bool gpsCom = false;

  if (xSemaphoreTake(gpsMutex, pdMS_TO_TICKS(10))) {
    sats = sharedGpsData.satellites;
    speed = sharedGpsData.speed;
    speedValid = sharedGpsData.speedValid;
    gpsCom = sharedGpsData.gpsCommunicating;
    xSemaphoreGive(gpsMutex);
  }

  u8g2.clearBuffer();
  
  // 1. TOP BAR: Branding + Icons
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 10, "Trackify");
  u8g2.drawXBM(90, 1, 10, 10, icon_gps_bits);
  u8g2.setCursor(102, 10); u8g2.print(gpsCom ? "+" : "-");
  u8g2.drawXBM(110, 1, 10, 10, icon_sd_bits);
  u8g2.setCursor(122, 10); u8g2.print(sdDetected ? "+" : "-");
  u8g2.drawHLine(0, 13, 128);

  // 2. MAIN CONTENT: Speed and Sats
  u8g2.setCursor(0, 25);
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.print("Sats: "); u8g2.print(sats);

  u8g2.setCursor(0, 50);
  u8g2.setFont(u8g2_font_ncenB18_tr);
  if (speedValid) {
    u8g2.print(speed, 1);
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
  } else if (currentState == STATE_PREALLOCATING) {
    u8g2.drawStr(60, 25, "ALLOCATING...");
  } else {
    const char* modeText = (currentState == STATE_READY) ? "READY" : "WAIT FIX";
    u8g2.drawStr(80, 25, modeText);
  }

  if (!sdDetected) {
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 64, "!!! NO SD CARD !!!");
  }

  u8g2.sendBuffer();
}

// ========================================================
// UI Task — runs on Core 0, normal priority
// Handles: OLED display, LED, button, WiFi DNS processing
// ========================================================
void uiTask(void *param) {
  Serial.print("[UI] Task started on Core ");
  Serial.println(xPortGetCoreID());

  // Draw initial WiFi QR screen flag
  bool wifiScreenDrawn = false;

  for (;;) {
    button.tick();

    if (currentState == STATE_WIFI_AP) {
      // Draw WiFi screen only once
      if (!wifiScreenDrawn) {
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_6x10_tf);
        u8g2.drawStr(0, 10, "WiFi:");
        u8g2.drawStr(0, 30, "Trackify");
        u8g2.drawStr(0, 40, "12345678");
        drawQRCode("WIFI:S:Trackify;T:WPA;P:12345678;;", 65, 5);
        u8g2.sendBuffer();
        wifiScreenDrawn = true;
      }
      wifiManager.handle();
      vTaskDelay(pdMS_TO_TICKS(10));
    } else {
      wifiScreenDrawn = false;
      updateOLED();
      vTaskDelay(pdMS_TO_TICKS(50)); // UI updates at ~20 FPS
    }
  }
}

// ========================================================
// Setup — runs once on Core 1 (default Arduino core)
// ========================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("=================================");
  Serial.println("  Trackify GNSS — Dual Core Init");
  Serial.println("=================================");

  pinMode(VEXT_PIN, OUTPUT);
  digitalWrite(VEXT_PIN, LOW); 
  delay(100);

  // OLED init
  u8g2.begin();
  u8g2.setContrast(255);
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB08_tr);
  u8g2.drawStr(0, 20, "DUAL CORE INIT...");
  u8g2.drawStr(0, 40, "Core0: UI");
  u8g2.drawStr(0, 55, "Core1: GPS + SD");
  u8g2.sendBuffer();

  // GPS init
  setupGPS();

  // SD init
  setupSD();
  
  // NeoPixel init
  strip.begin();
  strip.setBrightness(50);
  strip.show();

  // Button init
  button.attachClick(handleButton);
  button.attachLongPressStop(handleLongPress);
  button.setLongPressIntervalMs(4000);

  // Create FreeRTOS synchronization primitives
  gpsMutex = xSemaphoreCreateMutex();
  commandQueue = xQueueCreate(8, sizeof(Command));

  if (gpsMutex == NULL || commandQueue == NULL) {
    Serial.println("[FATAL] Failed to create FreeRTOS primitives!");
    while (1) { delay(1000); }
  }

  // Create GPS Task on Core 1 (high priority)
  xTaskCreatePinnedToCore(
    gpsTask,             // Task function
    "GPS_Task",          // Name
    GPS_TASK_STACK_SIZE, // Stack size
    NULL,                // Parameters
    GPS_TASK_PRIORITY,   // Priority (high)
    &gpsTaskHandle,      // Task handle
    GPS_TASK_CORE        // Core 1
  );

  // Create UI Task on Core 0 (normal priority)
  xTaskCreatePinnedToCore(
    uiTask,              // Task function
    "UI_Task",           // Name
    UI_TASK_STACK_SIZE,  // Stack size
    NULL,                // Parameters
    UI_TASK_PRIORITY,    // Priority (normal)
    &uiTaskHandle,       // Task handle
    UI_TASK_CORE         // Core 0
  );

  // Create LED Task on Core 0 (low priority, dedicated to patterns)
  xTaskCreatePinnedToCore(
    ledTask,
    "LED_Task",
    2048,
    NULL,
    1,                   // Lower priority than UI
    NULL,
    0                    // Core 0
  );

  Serial.println("[INIT] All tasks created. Setup complete.");
  Serial.print("[INIT] GPS Task on Core "); Serial.println(GPS_TASK_CORE);
  Serial.print("[INIT] UI  Task on Core "); Serial.println(UI_TASK_CORE);
}

// ========================================================
// loop() — empty, all work is done in FreeRTOS tasks
// ========================================================
void loop() {
  // Not used — FreeRTOS tasks handle everything
  vTaskDelay(portMAX_DELAY);
}
