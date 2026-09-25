#include <Arduino.h>
#include <U8g2lib.h>
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
#include "ble_manager.h"
#include "ubx_parser.h"
#include <SparkFun_u-blox_GNSS_v3.h>
#include "esp_mac.h"

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
#define GPS_UART_BUFFER_SIZE 8192

// --- SD Preallocation Size (50MB is ~15 hours of 25Hz binary logging, reduces allocation delay) ---
#define SD_PREALLOC_SIZE (50ULL * 1024 * 1024)

// --- SD Write Buffer (sector-aligned for optimal performance) ---
#define SD_WRITE_BUFFER_SIZE 512
#define SD_FLUSH_INTERVAL_MS 200

// --- LogMeta periodic flush (keeps record_count on SD fresh for crash recovery) ---
#define META_FLUSH_INTERVAL_MS 5000

// --- FreeRTOS Task Configuration ---
#define GPS_TASK_STACK_SIZE  8192
#define GPS_TASK_PRIORITY    5
#define GPS_TASK_CORE        1

#define UI_TASK_STACK_SIZE   8192
#define UI_TASK_PRIORITY     2
#define UI_TASK_CORE         0

// --- Objects ---
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, OLED_RST, 18, 17);
HardwareSerial ss(1);
OneButton button(BTN_PIN, true);
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
WiFiManager wifiManager;
SdFs sd;
UbxParser _ubxParser;
static bool _logHeaderNeeded = false;
static uint16_t actualGpsFreqHz = 0;

// --- Log Metadata (prepared at startup, written to each .bin file) ---
LogMeta g_logMeta;
static void prepareLogMeta() {
  memset(&g_logMeta, 0, sizeof(g_logMeta));

  // Device name
  strncpy(g_logMeta.name, "Trackify MX", sizeof(g_logMeta.name) - 1);

  // Serial number: last 3 bytes of MAC as hex string
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(g_logMeta.serial_number, sizeof(g_logMeta.serial_number),
           "%02X%02X%02X", mac[3], mac[4], mac[5]);

  // Model
  strncpy(g_logMeta.model, "Trackify GNSS", sizeof(g_logMeta.model) - 1);

  // Hardware / firmware
  g_logMeta.hardware_revision = 0;        // prototype
  g_logMeta.firmware_version  = 0x0200;   // v2.0

  // MAC address (raw bytes)
  memcpy(g_logMeta.mac_address, mac, 6);

  // Sample rate — will be updated after GPS init
  // actualGpsFreqHz is 0 until setupGPS() completes

  Serial.println("[META] Log metadata prepared:");
  Serial.printf("  name: %s\n", g_logMeta.name);
  Serial.printf("  serial: %s\n", g_logMeta.serial_number);
  Serial.printf("  model: %s\n", g_logMeta.model);
  Serial.printf("  fw: %04X  hw: %d\n",
                g_logMeta.firmware_version, g_logMeta.hardware_revision);
  Serial.printf("  MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// --- State Machine ---
enum DeviceState { STATE_IDLE, STATE_READY, STATE_PREALLOCATING, STATE_LOGGING, STATE_WIRELESS_SYNC };
volatile DeviceState currentState = STATE_IDLE;

volatile bool isLogging = false;
bool sdDetected = false;
unsigned long loggingStartTime = 0;
FsFile logFile;
char currentFileName[32];

// --- Meta flush + crash recovery state ---
static uint32_t g_recordCount = 0;        // TrackRecords written to current file
static uint32_t g_metaOffset = 0;         // byte offset of LogMeta from file start (= header length)
static uint32_t g_lastFlushedCount = 0;   // last record_count persisted to SD
static unsigned long g_lastMetaFlushMs = 0;

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
enum Command { CMD_START_LOGGING, CMD_STOP_LOGGING, CMD_START_WIRELESS, CMD_STOP_WIRELESS };
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
// Rate commands kept for fallback; primary config uses SparkFun v3 VALSET
const uint8_t UBX_CFG_RATE_10HZ[] = {
  0xB5, 0x62, 0x06, 0x08, 0x06, 0x00, 0x64, 0x00, 0x01, 0x00, 0x01, 0x00, 0x7A, 0x12
};

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

  SFE_UBLOX_GNSS_SERIAL gnss;
  if (!gnss.begin(ss)) {
    Serial.println("[GPS] SparkFun init FAILED — falling back to UBX CFG-RATE");
    #if GPS_FREQ_HZ == 25
      sendUBX(UBX_CFG_RATE_25HZ, sizeof(UBX_CFG_RATE_25HZ));
    #else
      sendUBX(UBX_CFG_RATE_10HZ, sizeof(UBX_CFG_RATE_10HZ));
    #endif
    actualGpsFreqHz = GPS_FREQ_HZ;
    return;
  }

  uint16_t measRate = (uint16_t)(1000 / GPS_FREQ_HZ);
  gnss.setMeasurementRate(measRate, VAL_LAYER_RAM_BBR);
  gnss.setUART1Output(COM_TYPE_UBX);

  delay(200);

  uint16_t actualMeasRate = 0;
  if (gnss.getMeasurementRate(&actualMeasRate, VAL_LAYER_RAM)) {
    if (actualMeasRate > 0) {
      actualGpsFreqHz = 1000 / actualMeasRate;
    } else {
      actualGpsFreqHz = GPS_FREQ_HZ;
    }
    Serial.printf("[GPS] MeasRate=%ums → %dHz (requested %dHz)\n", actualMeasRate, actualGpsFreqHz, GPS_FREQ_HZ);
  } else {
    actualGpsFreqHz = GPS_FREQ_HZ;
    Serial.println("[GPS] VALGET failed — using requested frequency");
  }

  gnss.end();
  _ubxParser.reset();
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

// --- Write raw bytes to SD buffer with auto-flush ---
static inline void sdWriteBytes(const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    sdWriteBuffer[sdBufPos++] = data[i];
    if (sdBufPos >= SD_WRITE_BUFFER_SIZE) {
      logFile.write(sdWriteBuffer, SD_WRITE_BUFFER_SIZE);
      sdBufPos = 0;
      lastFlushTime = millis();
    }
  }
}

// --- Write .bin file header ---
static void writeLogHeader(const NavPvtPayload& pvt) {
  char header[128];
  snprintf(header, sizeof(header),
    "#TRACKIFY:VER=2;FREQ=%d;UTC=%04d-%02d-%02dT%02d:%02d:%02d.%09dZ;PROTO=UBX-NAV-PVT\n",
    actualGpsFreqHz,
    pvt.year, pvt.month, pvt.day,
    pvt.hour, pvt.min, pvt.sec,
    pvt.nano >= 0 ? pvt.nano : 0);
  sdWriteBytes((const uint8_t*)header, strlen(header));
  g_metaOffset = strlen(header);   // LogMeta block starts right after the header newline
}

// --- Write service metadata block (64 bytes, VER=2+) ---
static void writeLogMeta() {
  // Update sample rate now that GPS is initialized
  g_logMeta.sample_rate_hz = actualGpsFreqHz;
  sdWriteBytes(reinterpret_cast<const uint8_t*>(&g_logMeta), sizeof(LogMeta));
}

// --- Write compact track record ---
static void writeTrackRecord(const NavPvtPayload& pvt) {
  TrackRecord rec;
  rec.iTOW    = pvt.iTOW;
  rec.nano    = pvt.nano;
  rec.lat     = pvt.lat;
  rec.lon     = pvt.lon;
  rec.height  = pvt.height;
  rec.gSpeed  = pvt.gSpeed;
  rec.headMot = pvt.headMot;
  rec.hAcc    = pvt.hAcc;
  rec.sAcc    = pvt.sAcc;
  rec.numSV   = pvt.numSV;
  rec.fixType = pvt.fixType;
  sdWriteBytes(reinterpret_cast<const uint8_t*>(&rec), sizeof(TrackRecord));
  g_recordCount++;
}

// --- Update shared GPS data from PVT ---
static void updateSharedFromPvt(const NavPvtPayload& pvt) {
  // Strict fix check:
  // 1. fixType >= 2 (2D/3D fix)
  // 2. numSV >= 8 (good constellation)
  // 3. flags & 0x01 (gnssFixOK: valid navigation solution per u-blox M10 ICD)
  // 4. valid & 0x03 == 0x03 (validDate + validTime: confirmed UTC timestamp)
  // 5. hAcc < 10000 (horizontal accuracy estimate < 10m)
  bool gnssOk = (pvt.flags & 0x01) != 0;
  bool timeValid = (pvt.valid & 0x03) == 0x03;
  bool accOk = (pvt.hAcc < 10000);
  bool hasFix = (pvt.fixType >= 2) && (pvt.numSV >= 8) && gnssOk && timeValid && accOk;

  if (xSemaphoreTake(gpsMutex, pdMS_TO_TICKS(5))) {
    sharedGpsData.satellites = pvt.numSV;
    sharedGpsData.speed = (pvt.gSpeed / 1000.0) * 3.6;
    sharedGpsData.speedValid = (pvt.fixType >= 2) && gnssOk;
    sharedGpsData.hasFix = hasFix;
    sharedGpsData.gpsCommunicating = (pvt.fixType != 0) || (pvt.numSV > 0);
    xSemaphoreGive(gpsMutex);
  }
}

// --- Logging Control (called from GPS Task only) ---
void startLogging() {
  bool hasFix = false;
  if (xSemaphoreTake(gpsMutex, pdMS_TO_TICKS(10))) {
    hasFix = sharedGpsData.hasFix;
    xSemaphoreGive(gpsMutex);
  }

  if (!sdDetected || !hasFix) {
    Serial.println("[GPS] Cannot start logging — no reliable fix or no SD.");
    return;
  }

  int n = 0;
  do {
    sprintf(currentFileName, "/log_%03d.bin", n++);
  } while (sd.exists(currentFileName));

  if (logFile.open(currentFileName, O_WRONLY | O_CREAT | O_TRUNC)) {
    currentState = STATE_PREALLOCATING;
    Serial.print("[GPS] Creating file: "); Serial.println(currentFileName);
    Serial.printf("[GPS] Starting pre-allocation (%llu MB)...\n", SD_PREALLOC_SIZE / (1024 * 1024));

    if (logFile.preAllocate(SD_PREALLOC_SIZE)) {
      isLogging = true;
      g_recordCount = 0;
      g_metaOffset = 0;
      g_lastFlushedCount = 0;
      g_lastMetaFlushMs = millis();
      g_logMeta.record_count = 0;   // fresh file: count unknown until first meta flush
      _logHeaderNeeded = true;
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

// --- Periodic LogMeta flush: persists record_count every 5s so a power failure
// loses at most one flush interval of records after boot-time recovery.
// Called from gpsTask only. Must not write over records: flush buffer first,
// then seekSet(meta) -> write 64B -> sync -> restore data-end position.
// NOTE: seekEnd() is NOT used here — the file is preallocated to 200MB, so
// seekEnd() would jump past the written data to the preallocated end.
static void flushLogMetaIfDue() {
  if (!isLogging || g_metaOffset == 0) return;
  if (millis() - g_lastMetaFlushMs < META_FLUSH_INTERVAL_MS) return;
  if (g_recordCount == g_lastFlushedCount) {
    g_lastMetaFlushMs = millis();
    return;
  }
  flushSdBuffer();
  g_logMeta.record_count = g_recordCount;
  uint64_t dataEnd = logFile.curPosition();
  logFile.seekSet(g_metaOffset);
  logFile.write(&g_logMeta, sizeof(LogMeta));
  logFile.sync();               // push sector to SD — survives power failure
  logFile.seekSet(dataEnd);     // continue appending records
  g_lastFlushedCount = g_recordCount;
  g_lastMetaFlushMs = millis();
}

void stopLogging() {
  if (isLogging || currentState == STATE_PREALLOCATING) {
    if (isLogging) {
      flushSdBuffer();
      // Final meta write with exact record count (before truncate)
      if (g_metaOffset > 0) {
        g_logMeta.record_count = g_recordCount;
        uint64_t dataEnd = logFile.curPosition();
        logFile.seekSet(g_metaOffset);
        logFile.write(&g_logMeta, sizeof(LogMeta));
        logFile.sync();
        logFile.seekSet(dataEnd);   // truncate() cuts at the current position
      }
      logFile.truncate();
    } else {
      // Stopped during preallocation: the file holds nothing but its 200MB
      // preallocated extent — cut it back to zero so no garbage file lingers
      // on the SD card.
      logFile.truncate();
    }
    logFile.close();
    isLogging = false;
    _logHeaderNeeded = false;
    sdBufPos = 0;
    g_recordCount = 0;
    g_metaOffset = 0;
    currentState = STATE_READY;
    Serial.println("[GPS] Logging stopped and file closed.");
  }
}

// --- Boot/hot-plug recovery: preallocated log_*.bin files left at 200MB after a
// power failure are truncated to hdrEnd+1+64+record_count*38 using the last
// meta-flushed record_count. Files with count==0, without a #TRACKIFY header,
// or with expected >= file size are left untouched. Uses local FsFile objects —
// safe only when logging is not active (boot, or SD hot-plug re-detect).
void recoverInterruptedLogs() {
  FsFile dir;
  if (!dir.open("/", O_RDONLY)) {
    Serial.println("[RECOVERY] Cannot open SD root.");
    return;
  }
  FsFile f;
  while (f.openNext(&dir, O_RDWR)) {
    if (!f.isDir()) {
      char name[32];
      f.getName(name, sizeof(name));
      if (strncmp(name, "log_", 4) == 0 && strstr(name, ".bin") != NULL) {
        char buf[256];
        int n = f.read(buf, sizeof(buf));
        int hdrEnd = -1;
        for (int i = 0; i < n; i++) {
          if (buf[i] == '\n') { hdrEnd = i; break; }
        }
        if (hdrEnd > 0 && hdrEnd < 255 && memcmp(buf, "#TRACKIFY:", 10) == 0) {
          // LogMeta (and record_count inside it) exists only for VER>=2 files.
          // VER=1 files have records right after the header — reading offset 60
          // there would interpret record bytes as record_count and could corrupt
          // a valid file.
          buf[hdrEnd] = 0;   // null-terminate the header line for parsing
          char* verPos = strstr(buf, "VER=");
          int ver = (verPos != NULL) ? atoi(verPos + 4) : 0;
          if (ver >= 2) {
            uint32_t count = 0;
            // record_count sits at offset 60 inside the 64-byte LogMeta (hdrEnd+1);
            // ESP32 is little-endian, matches the stored uint32.
            if (f.seekSet((uint32_t)hdrEnd + 1 + 60) && f.read(&count, 4) == 4) {
              uint64_t recordStart = (uint64_t)hdrEnd + 1 + 64;
              uint64_t expected = recordStart + (uint64_t)count * 38;
              if (count > 0 && expected < f.fileSize()) {
                f.truncate(expected);
                f.sync();
                Serial.printf("[RECOVERY] %s truncated to %llu bytes (%lu records)\n",
                              name, expected, (unsigned long)count);
              }
            }
          }
        }
      }
    }
    f.close();
  }
  dir.close();
}

// ========================================================
// GPS Task — runs on Core 1, high priority
// Handles: UART reading, UBX frame parsing, SD writing
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
        case CMD_START_WIRELESS:
          if (isLogging) stopLogging();
          Serial.println("[GPS] Shutting down GPS for Wireless mode...");
          ss.end();
          wifiManager.begin(actualGpsFreqHz > 0 ? actualGpsFreqHz : GPS_FREQ_HZ);
          bleManager.begin();
          currentState = STATE_WIRELESS_SYNC;
          break;
        case CMD_STOP_WIRELESS:
          bleManager.stop();
          wifiManager.stop();
          currentState = STATE_IDLE;
          Serial.println("[GPS] Wireless stopped. Restarting GPS.");
          setupGPS();
          break;
      }
    }

    // --- 2. Read GPS UART — parse UBX frames, write compact records to SD ---
    if (currentState != STATE_WIRELESS_SYNC) {
      static uint32_t lastSdCheck = 0;
      if (!sdDetected && (millis() - lastSdCheck) > 10000) {
        lastSdCheck = millis();
        if (sd.begin(SdSpiConfig(SD_CS, SHARED_SPI, SD_SCK_MHZ(20), &SPI))) {
          sdDetected = true;
          Serial.println("[SD] Card detected (Hot-plug).");
          recoverInterruptedLogs();   // not logging here — SD was absent
        }
      }

      int available = ss.available();
      while (available-- > 0) {
        char c = ss.read();

        if (_ubxParser.feed((uint8_t)c)) {
          const NavPvtPayload& pvt = _ubxParser.pvt();

          updateSharedFromPvt(pvt);

          if (isLogging) {
            if (_logHeaderNeeded) {
              writeLogHeader(pvt);
              writeLogMeta();
              _logHeaderNeeded = false;
            }
            writeTrackRecord(pvt);
          }
        }
      }

      if (isLogging && sdBufPos > 0 && (millis() - lastFlushTime) >= SD_FLUSH_INTERVAL_MS) {
        flushSdBuffer();
        lastFlushTime = millis();
      }
      flushLogMetaIfDue();   // periodic record_count persistence (crash survival)

      // --- 3. Update device state based on fix and SD detection ---
      bool hasFix = false;
      if (xSemaphoreTake(gpsMutex, pdMS_TO_TICKS(5))) {
        hasFix = sharedGpsData.hasFix;
        xSemaphoreGive(gpsMutex);
      }

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
      case STATE_WIRELESS_SYNC:
        baseColor = strip.Color(0, 255, 255); // Cyan for Hybrid Sync
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
  if (currentState == STATE_WIRELESS_SYNC) {
    Command cmd = CMD_STOP_WIRELESS;
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
  Serial.println("[UI] Long press — starting Wireless Sync.");
  Command cmd = CMD_START_WIRELESS;
  xQueueSend(commandQueue, &cmd, pdMS_TO_TICKS(100));
}

// --- OLED Update (called from UI Task) ---
void updateOLED() {
  if (currentState == STATE_WIRELESS_SYNC) {
    return; // Handled in uiTask
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

    if (currentState == STATE_WIRELESS_SYNC) {
      if (!wifiScreenDrawn) {
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_ncenB08_tr);
        u8g2.drawStr(5, 15, "Wireless Sync");
        u8g2.setFont(u8g2_font_5x7_tf);
        u8g2.drawStr(0, 30, "WiFi: Trackify / 12345678");
        u8g2.drawStr(0, 45, "BLE: Trackify");
        u8g2.drawStr(0, 60, "Waiting for App...");
        u8g2.sendBuffer();
        wifiScreenDrawn = true;
      }
      wifiManager.handle();
      bleManager.handle();
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

  // Recover log_*.bin files left at preallocated size by a power failure
  if (sdDetected) {
    recoverInterruptedLogs();
  }
  
  // NeoPixel init
  strip.begin();
  strip.setBrightness(50);
  strip.show();

  // Button init
  button.attachClick(handleButton);
  button.attachLongPressStop(handleLongPress);
  button.setLongPressIntervalMs(4000);

  // Prepare log metadata (MAC, serial, version — ready before any logging starts)
  prepareLogMeta();

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
