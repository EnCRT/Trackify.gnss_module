# 🏍️ Trackify GNSS Module v.0.1.2

<p align="center">
  <img src="https://img.shields.io/badge/board-ESP32--S3-00979D?style=for-the-badge&logo=espressif" alt="ESP32-S3">
  <img src="https://img.shields.io/badge/framework-Arduino-00979D?style=for-the-badge&logo=arduino" alt="Arduino">
  <img src="https://img.shields.io/badge/rtos-FreeRTOS-5C6BC0?style=for-the-badge" alt="FreeRTOS">
  <img src="https://img.shields.io/badge/gnss-25Hz-10B981?style=for-the-badge" alt="25Hz GNSS">
  <img src="https://img.shields.io/badge/version-0.1.2-blue?style=for-the-badge" alt="v0.1.2">
</p>

> 🎯 **25 Hz sampling rate GPS tracker** — 25× more detail than a standard 1-second GPS. Captures every movement of the motorcycle: corner entry, apex, exit — not a single track point is lost. 25 coordinates per second, not 1.

Purpose-built for maximum lap-analysis precision. Powered by ESP32-S3 with dual wireless sync (WiFi + BLE) to the Android app Trackify.app [Moto Lap Timer](https://github.com/EnCRT/Trackify.app).

One long button press turns the tracker into an access point: WiFi captive portal for quick log downloads from a browser + BLE server for seamless mobile app integration.

---

## 🔧 Hardware

| 🧩 Component | Model |
|---|---|
| 🧠 MCU | Heltec ESP32 WiFi Kit 32 (V3) — ESP32-S3, 16MB Flash |
| 🛰️ GNSS | HGLRC M100-5883 M10 GPS Module + Compass (Quectel N16R8) |
| 💾 Storage | Micro SD Card Module SPI 3.3V TF Reader + MicroSD 32GB V30 |
| 📟 Display | SSD1306 128×64 OLED I²C |
| 💡 LED | WS2812B SMD RGB 3×10mm |
| 🔘 Button | 12mm Waterproof Momentary ON/OFF Push Button |

---

## 🧬 Architecture

### Dual-Core FreeRTOS — hard real-time on two cores

```
┌──────────────────────────────────────────────────────────┐
│                       ESP32-S3                           │
│                                                          │
│  🖥️  Core 0 (Protocol)         ⚡ Core 1 (Real-time)    │
│  ┌─────────────────────┐       ┌────────────────────┐    │
│  │ 🖥️  UI Task  (P=2) │◄─────►│  ⚡ GPS Task (P=5)  │   │
│  │ • 🖼️  OLED render  │ Queue │ • 📡 UART read      │   │
│  │ • 🔘 Button tick   │       │ • 📊 NMEA parse     │   │
│  │ • 🌐 WiFi DNS      │       │ • 💾 SD buffering   │   │
│  │ • 📶 BLE handling  │       │ • 🎯 Fix detection  │   │
│  └─────────┬───────────┘       └─────────┬──────────┘    │
│            │                            │                │
│  ┌─────────┴──────────┐       ┌─────────┴──────────┐     │
│  │ 💡 LED Task (P=1) │        │ 🔒 GPS Mutex      │     │
│  │  poll 20ms         │       │  SharedGpsData     │     │
│  └────────────────────┘       └────────────────────┘     │
└──────────────────────────────────────────────────────────┘
```

> **Why?** 25 Hz GPS parsing generates ~1 KB/s of raw NMEA data. Core 1 at priority 5 handles it without drops — zero byte loss. Core 0 handles UI and wireless comms without interfering with logging.


| 🏷️ State | 💡 LED Pattern | 🎨 Color | What's happening |
|---|---|---|---|
| `IDLE` | Double flash | 🟠 Orange | Waiting for satellites / SD card |
| `READY` | Double flash | ⚪ White | Ready to log, waiting for button |
| `PREALLOCATING` | Double flash | 🔴 Red | Allocating 200 MB on SD |
| `LOGGING` | Solid | 🔴 Red | Logging in progress! |
| `WIRELESS_SYNC` | Double flash | 🩵 Cyan | WiFi AP + BLE active |
| Button held | Solid | 🔴 Red | Button pressed |

---

## 📌 Pinout

| 🔌 Signal | 📍 Pin | Purpose |
|---|---|---|
| `GPS_RX` | 4 | UART RX ← Quectel N16R8 TX |
| `GPS_TX` | 5 | UART TX → Quectel N16R8 RX |
| `SD_MOSI` | 40 | SPI MOSI |
| `SD_MISO` | 41 | SPI MISO |
| `SD_SCK` | 42 | SPI SCK (20 MHz) |
| `SD_CS` | 39 | SPI CS |
| `BTN_PIN` | 1 | Button (ACTIVE_LOW, internal pull-up) |
| `LED_PIN` | 38 | WS2812B data |
| `OLED_SDA` | 18 | I²C SDA |
| `OLED_SCL` | 17 | I²C SCL |
| `OLED_RST` | 21 | OLED reset |
| `VEXT_PIN` | 36 | Peripheral power enable |

---

## ⚡ 25 Hz — why it matters

| Frequency | Interval | Points per 10-sec corner | Level of detail |
|---|---|---|---|
| 1 Hz (standard GPS) | 1 000 ms | 10 | 🤷 Entry and exit only |
| **25 Hz (Trackify)** | **40 ms** | **250** | 🔬 Full trajectory |

At 150 km/h a motorcycle travels **1.7 meters every 40 ms**. Only 25 Hz reveals the real line through a corner: where braking starts, the apex, where throttle opens. At 1 Hz all you see is a blurry trace with 42-meter gaps.

```cpp
// src/main.cpp:38 — toggle mode
#define GPS_FREQ_HZ 25  // 10 or 25 Hz
```

---

## 🛰️ GPS & SD Logging

- **25 Hz** — UBX command configures the Quectel N16R8 module at boot
- **UART buffer 2048 bytes** — overflow protection at peak data rates
- **Sector-aligned buffer 512 bytes** — optimized for SD card physical sectors
- **Flush every 200 ms** — balances data safety with SPI bus load
- **200 MB preallocation** — eliminates FAT fragmentation, critical for 25 Hz sustained writes
- **Truncate on stop** — file trimmed to actual written size
- **Auto-increment filenames** — `log_000.txt` → `log_001.txt` → ...
- **Hot-plug SD** — re-checked every 10 sec, card can be inserted anytime

---

## 📶 Wireless Sync

### 🌐 WiFi Captive Portal

```
SSID:    Trackify
Pass:    12345678
IP:      192.168.0.11 (static)
DNS:     :53 → captive portal
```

| 🌍 Endpoint | Method | Description |
|---|---|---|
| `/` | GET | SPA with log table, dark/light theme, animated background |
| `/download?file=` | GET | Download raw TXT file |
| `/delete?file=` | POST | Delete a file |

> 💡 The web UI does **on-the-fly NMEA → GPX conversion in the browser** — no extra software needed, just open the page on your phone and download GPX.

### 📶 Bluetooth Low Energy (NimBLE)

| 🔑 Parameter | Value |
|---|---|
| Device name | `Trackify` |
| Service UUID | `4fafc201-1fb5-459e-8fcc-c5c9c331914b` |

| 📨 Characteristic | UUID | Props |
|---|---|---|
| Command | `beb5483e-...` | WRITE |
| Data | `2c27702b-...` | NOTIFY |

**Commands:** `LIST` — file listing from SD (in ~200-byte chunks), `WIFI` — SSID and password for direct connection.

---

## 📚 Tech Stack

| 🧱 Layer | 📦 Library | Purpose |
|---|---|---|
| RTOS | FreeRTOS | Dual-core scheduling, mutex, queue |
| GNSS | TinyGPSPlus | Parsing $GPRMC, $GPGGA |
| Display | U8g2 | OLED SSD1306 128×64 I²C |
| SD | SdFat v2 | SPI 20MHz, preAllocate(), truncate() |
| WiFi | ESPAsyncWebServer + DNSServer | Captive portal, streaming download |
| BLE | NimBLE-Arduino | GATT server, MTU 517 |
| Button | OneButton | Debounce, click, long-press 4s |
| LED | Adafruit NeoPixel | WS2812B patterns |

---

## 🚀 Build & Flash

```bash
git clone <repo-url>
cd Trackify.gnss_module

pio run                         # build
pio run --target upload         # flash
pio device monitor -b 115200    # serial monitor
```

**Requirements:** [PlatformIO](https://platformio.org/), board `heltec_wifi_kit_32_v3`.

---

## 📱 Integration with moto_lap_timer

> 📖 Full workflow: [bluetooth_integration_docs.md](bluetooth_integration_docs.md)

```
🏍️ Finished a session → 🔘 Held button 4s → 📶 BLE/WiFi active
→ 📱 Opened Moto Lap Timer → 🔵 Bluetooth → Found Trackify
→ 📋 LIST → 📥 Downloaded track → 🗺️ Auto-parsing & analysis screen
```

---

## 📁 Project Structure

```
Trackify.gnss_module/
├── platformio.ini                  # 🧪 PlatformIO configuration
├── src/
│   ├── main.cpp                    # 🚀 Entry point, FreeRTOS, GPS/SD/OLED/LED
│   ├── ble_manager.cpp / .h        # 📶 BLE GATT server (NimBLE)
│   └── wifi_manager.cpp            # 🌐 WiFi AP + AsyncWebServer + DNS
├── include/
│   ├── wifi_manager.h              # 🌐 WiFiManager header
│   └── logs_ui.h                   # 🎨 Embedded HTML/CSS/JS SPA
├── bluetooth_integration_docs.md   # 📖 BLE integration with moto_lap_timer
└── test/                           # 🧪 Unit tests
```
