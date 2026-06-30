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
| F-WIFI-05 | Disable WiFi modem sleep after connecting to ensure the web server remains reachable at all times. |

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
| F-DISP-04 | Show the device IP address and installed firmware version on page 0 at scale ×1, e.g. `192.168.86.147 v1.2`. |
| F-DISP-05 | Show the current date in `DD.MM.YYYY` format centered horizontally at scale ×1 (page 1). |
| F-DISP-06 | Refresh the display once per second. |

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
│ 192.168.86.147 v1.2   page 0  │  ← IP + version, scale 1 (7 px tall)
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
| `v` | 14 |

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

1. Hold BOOT button
2. Press and release EN/RST button
3. Release BOOT button
4. Flash with `--before no-reset`

---

## 9. Web Server

The system exposes an HTTP server on port 80, accessible from any device on the local network, showing live device status and providing OTA firmware management.

### 9.1 Requirements

| ID | Requirement |
|----|-------------|
| F-WEB-01 | Start an HTTP server on port 80 after WiFi connects. |
| F-WEB-02 | Serve a status page at `/` displaying firmware version, active OTA slot, current date, current time, and uptime. |
| F-WEB-03 | The firmware version shall be derived from the ESP-IDF app description (`esp_app_get_description()`), matching the git-based version shown in boot logs. |
| F-WEB-04 | Date and time displayed on the web page shall use the same Norwegian timezone (CET/CEST) as the OLED. |
| F-WEB-05 | The page shall refresh automatically every second via JavaScript `setInterval`. |
| F-WEB-06 | The status page shall display the current free heap in bytes via `esp_get_free_heap_size()`. |

### 9.2 Status Page Format

```
Firmware: v1.2
Slot:     ota_0
Date:     30.06.2026
Time:     12:34:56
Uptime:   00:05:32
Free heap: 211240 B
```

### 9.3 OTA Firmware Upload

| ID | Requirement |
|----|-------------|
| F-OTA-01 | The status page shall include a "Choose File" button and "Upload & Reboot" button for OTA firmware update. |
| F-OTA-02 | Tapping "Choose File" shall stop the auto-refresh timer before opening the file dialog, preventing the page from reloading while the picker is open. |
| F-OTA-03 | The selected filename shall be displayed in a `<span>` next to the button; "Upload & Reboot" shall remain disabled until a file is confirmed selected. |
| F-OTA-04 | Clicking "Upload & Reboot" shall POST the selected `.bin` file to `/update` as `application/octet-stream`. |
| F-OTA-05 | The `/update` handler shall write the received binary to the inactive OTA partition using `esp_ota_write`. |
| F-OTA-06 | On success, the handler shall call `esp_ota_set_boot_partition` and reboot after 1 second. |
| F-OTA-07 | On any error (no OTA partition, write failure, receive error), the handler shall return HTTP 500 with a plain-text description and abort the OTA handle. |
| F-OTA-08 | The partition table shall provide two equally-sized OTA slots (`ota_0` / `ota_1`, 1 MB each) on 4 MB flash, with no factory partition. |

### 9.5 Firmware Download

| ID | Requirement |
|----|-------------|
| F-DL-01 | Each slot entry in the "Installed slots" section shall include a `[Download]` link pointing to `/download?slot=<0\|1>`. |
| F-DL-02 | The `/download` handler shall determine the exact firmware image size by parsing the ESP image header: reading `segment_count`, summing segment sizes, adding the checksum byte, and adding the SHA256 hash if `hash_appended` is set. |
| F-DL-03 | The handler shall stream the exact image bytes from flash using `esp_partition_read` and `httpd_resp_send_chunk`, with no trailing padding. |
| F-DL-04 | The response shall use `Content-Type: application/octet-stream` and `Content-Disposition: attachment; filename="esp32_clock_vX.X.bin"`, where the version is read from the partition's app description. |

### 9.4 OTA Slot Switching

| ID | Requirement |
|----|-------------|
| F-SLOT-01 | The status page shall display an "Installed slots" section listing both OTA partitions with their firmware version. |
| F-SLOT-02 | Each slot entry shall include a "Boot this" button that switches the active boot partition and reboots immediately. |
| F-SLOT-03 | The button for the currently-running slot shall be disabled. |
| F-SLOT-04 | Slot switching shall be handled by a POST to `/boot?slot=<0|1>`, which calls `esp_ota_set_boot_partition` and reboots after 1 second. |
| F-SLOT-05 | If a slot contains no valid firmware, its version shall be shown as `(empty)` and the button shall still render (the partition check is deferred to the boot loader). |

---

## 10. Known Limitations

| ID | Description |
|----|-------------|
| L-01 | WiFi credentials are compiled into the binary; no runtime configuration. |
| L-02 | No graceful handling if NTP never syncs after initial boot (display stays at `--:--:--` indefinitely). |
| L-03 | Single NTP server; no fallback. |
| L-04 | Display I2C address is hardcoded to `0x3C`; boards with `0x3D` require a source change. |
| L-05 | The web server HTTP stack size is 8 kB; uploading firmware larger than ~850 kB may require increasing `config.stack_size`. |
