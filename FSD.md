# Functional Specification Document — ESP32 NTP Clock

## 1. Overview

A bare-metal ESP32 firmware that connects to WiFi, synchronizes time via NTP, and displays the current Norwegian local time on an SSD1306 OLED display. No RTOS abstractions beyond FreeRTOS task delays; no external libraries beyond ESP-IDF.

---

## 2. Hardware

| Component | Detail |
|-----------|--------|
| MCU | ESP32 (ESP32-D0WDQ6) |
| Display | SSD1306 OLED 128×64, I2C |
| I2C address | `0x3C` |
| SDA | GPIO 5 |
| SCL | GPIO 4 |
| I2C speed | 400 kHz (fast mode) |
| Power | 3.3 V |

---

## 3. Functional Requirements

### 3.1 WiFi

| ID | Requirement |
|----|-------------|
| F-WIFI-01 | Connect to a pre-configured WPA2 access point (SSID + password compiled in). |
| F-WIFI-02 | Automatically reconnect on disconnect without rebooting. |
| F-WIFI-03 | Expose the assigned IPv4 address to the display layer as soon as DHCP completes. |
| F-WIFI-04 | Wait up to 60 seconds for initial connection before proceeding; if no IP is obtained, display `0.0.0.0`. |

### 3.2 Time Synchronization

| ID | Requirement |
|----|-------------|
| F-NTP-01 | Synchronize wall-clock time from `pool.ntp.org` via SNTP in poll mode. |
| F-NTP-02 | Apply Norwegian timezone with automatic DST transitions: CET (UTC+1) in winter, CEST (UTC+2) in summer. |
| F-NTP-03 | Retry NTP sync up to 30 seconds (60 × 500 ms polls) after WiFi connects. |
| F-NTP-04 | Display `--:--:--` and `--.--.----` until a valid time (year ≥ 2020) is received. |

### 3.3 Display

| ID | Requirement |
|----|-------------|
| F-DISP-01 | Drive SSD1306 directly via the ESP-IDF new I2C master API; no third-party display library. |
| F-DISP-02 | Render characters from a built-in 5×7 pixel font with configurable integer scale. |
| F-DISP-03 | Show the current time in 24-hour format (`HH:MM:SS`) centered horizontally, scaled ×2, starting at page 3. |
| F-DISP-04 | Show the device IP address in the top-left corner at scale ×1 (page 0). |
| F-DISP-05 | Show the current date in `DD.MM.YYYY` format centered horizontally at scale ×1 (page 1). |
| F-DISP-06 | Refresh the display once per second. |
| F-DISP-07 | On startup, run a brief checkerboard test pattern (alternating 0xFF / 0x00 pages, 1 second) to verify the display is alive. |

---

## 4. Non-Functional Requirements

| ID | Requirement |
|----|-------------|
| NF-01 | Firmware must build cleanly with ESP-IDF v6.0 or later using `idf.py build`. |
| NF-02 | No heap allocations in the display or font rendering path — all framebuffer state is static. |
| NF-03 | I2C internal pull-ups enabled in firmware; no external pull-up resistors required. |
| NF-04 | Single-file implementation (`main/main.c`) with no external component dependencies beyond ESP-IDF built-ins. |

---

## 5. Display Layout

```
┌────────────────────────────────┐  ← 128 px wide
│ 192.168.1.42          page 0  │  ← IP, scale 1 (7 px tall)
│       30.06.2026      page 1  │  ← Date, scale 1 (7 px tall)
│                        page 2  │
│     12:34:56          page 3  │  ← Time, scale 2 (14 px tall)
│                        page 4  │
│                        page 5  │
│                        page 6  │
│                        page 7  │
└────────────────────────────────┘  ← 64 px tall
```

Character widths at scale ×1: each glyph is `(5+1) = 6 px` wide.  
`DD.MM.YYYY` = 10 characters × 6 px = 60 px, left-offset at x=34 to center in 128 px.

Character widths at scale ×2: each glyph is `(5+1) × 2 = 12 px` wide.  
`HH:MM:SS` = 8 characters × 12 px = 96 px, left-offset at x=17 to center in 128 px.

---

## 6. Font

5×7 bitmap font, column-encoded (LSB = top pixel). Supported characters:

| Char | Index |
|------|-------|
| `0`–`9` | 0–9 |
| `:` | 10 |
| ` ` (space) | 11 |
| `-` | 12 |
| `.` | 13 |

---

## 7. Timezone

POSIX TZ string: `CET-1CEST,M3.5.0,M10.5.0/3`

- Standard time: CET = UTC+1 (winter)
- Daylight time: CEST = UTC+2 (summer)
- DST starts: last Sunday of March at 02:00
- DST ends: last Sunday of October at 03:00

---

## 8. Build & Flash

```bash
idf.py build
idf.py -p COM4 flash monitor
```

If auto-reset fails (common when WiFi is active):

1. Unplug USB
2. Hold BOOT button
3. Plug USB back in
4. Wait 2 seconds, release BOOT
5. Flash with `--before no-reset`

---

## 9. Web Server

The system shall expose an HTTP server on port 80, accessible from any device on the local network, showing live device status.

### 9.1 Requirements

| ID | Requirement |
|----|-------------|
| F-WEB-01 | Start an HTTP server on port 80 after WiFi connects. |
| F-WEB-02 | Serve a single status page at `/` displaying the firmware version, current date, and current time. |
| F-WEB-03 | The firmware version shall be derived from the ESP-IDF app description (`esp_app_get_description()`), matching the git-based version shown in boot logs. |
| F-WEB-04 | Date and time displayed on the web page shall use the same Norwegian timezone (CET/CEST) as the OLED. |
| F-WEB-05 | The page shall refresh automatically every second. |

### 9.2 Response Format

The status page shall return `Content-Type: text/html` with a minimal HTML body, for example:

```
Firmware: v1.0
Date:     30.06.2026
Time:     12:34:56
```

### 9.3 OTA Firmware Update

| ID | Requirement |
|----|-------------|
| F-OTA-01 | The status page shall include a file-picker and "Upload & Reboot" button for OTA firmware update. |
| F-OTA-02 | Clicking the button shall POST the selected `.bin` file to `/update` as `application/octet-stream`. |
| F-OTA-03 | The `/update` handler shall write the received binary to the inactive OTA partition using `esp_ota_write`. |
| F-OTA-04 | On success, the handler shall call `esp_ota_set_boot_partition` and reboot after 1 second. |
| F-OTA-05 | On any error (no OTA partition, write failure, receive error), the handler shall return HTTP 500 with a plain-text description and abort the OTA handle. |
| F-OTA-06 | The partition table shall provide two equally-sized OTA slots (`ota_0` / `ota_1`, 1 MB each) on 4 MB flash, with no factory partition. |

---

## 10. Known Limitations

| ID | Description |
|----|-------------|
| L-01 | WiFi credentials are compiled into the binary; no runtime configuration or OTA update support. |
| L-02 | No graceful handling if NTP never syncs after initial boot (display stays at `--:--:--` indefinitely). |
| L-03 | Single NTP server; no fallback. |
| L-04 | Display I2C address is hardcoded to `0x3C`; boards with `0x3D` require a source change. |
