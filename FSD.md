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
| F-NTP-04 | Display `--:--:--` until a valid time (year ≥ 2020) is received. |

### 3.3 Display

| ID | Requirement |
|----|-------------|
| F-DISP-01 | Drive SSD1306 directly via the ESP-IDF new I2C master API; no third-party display library. |
| F-DISP-02 | Render characters from a built-in 5×7 pixel font with configurable integer scale. |
| F-DISP-03 | Show the current time in 24-hour format (`HH:MM:SS`) centered horizontally, scaled ×2, in the lower half of the screen (pages 3–6). |
| F-DISP-04 | Show the device IP address in the top-left corner at scale ×1 (page 0). |
| F-DISP-05 | Refresh the display once per second. |
| F-DISP-06 | On startup, run a brief checkerboard test pattern (alternating 0xFF / 0x00 pages, 1 second) to verify the display is alive. |

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
│                        page 1  │
│                        page 2  │
│   1 2 : 3 4 : 5 6     page 3  │  ← Time, scale 2 (14 px tall)
│                        page 4  │
│                        page 5  │
│                        page 6  │
│                        page 7  │
└────────────────────────────────┘  ← 64 px tall
```

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

## 9. Known Limitations

| ID | Description |
|----|-------------|
| L-01 | WiFi credentials are compiled into the binary; no runtime configuration or OTA update support. |
| L-02 | No graceful handling if NTP never syncs after initial boot (display stays at `--:--:--` indefinitely). |
| L-03 | Single NTP server; no fallback. |
| L-04 | Display I2C address is hardcoded to `0x3C`; boards with `0x3D` require a source change. |
