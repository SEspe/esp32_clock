# ESP32 NTP Clock

An ESP32 project that displays the current Norwegian time (CET/CEST) on an SSD1306 OLED display, synchronized over WiFi via NTP.

## Features

- Connects to WiFi and syncs time from `pool.ntp.org`
- Displays current time in 24-hour format (HH:MM:SS), large text
- Displays current date in DD.MM.YYYY format
- Shows assigned IP address on the top of the screen
- Shows `--:--:--` / `--.--.----` while waiting for NTP sync
- Shows `0.0.0.0` if WiFi is not connected
- Correct Norwegian timezone (UTC+1 CET in winter, auto-switches to UTC+2 CEST in summer)
- OLED page 0 shows IP address and firmware version together, e.g. `192.168.86.147 v1.4`
- HTTP status page on port 80: firmware version, active OTA slot, date, time, uptime, free heap
- OTA firmware update via the web page (no USB required after initial flash)
- OTA slot switching: boot either installed firmware version from the web page
- Firmware download: save any installed slot's binary directly from the web page
- **Alarm:** configurable daily alarm via web UI — GPIO14 LED flashes (1 s period) and OLED inverts; persisted in NVS; auto-silences after 60 s or on manual Dismiss
- **Snooze:** web **Snooze 5 min** button re-fires the alarm after 5 minutes; status shows `Snoozed — rings at HH:MM`
- **Physical ack button:** press GPIO0 (BOOT button) to instantly dismiss the alarm without opening the web UI

## Hardware

| Component | Detail |
|-----------|--------|
| MCU | ESP32 (tested on ESP32-D0WDQ6) |
| Display | SSD1306 OLED 128×64, I2C address `0x3C` |
| SDA | GPIO 5 |
| SCL | GPIO 4 |
| Alarm LED | GPIO 14 (external LED + resistor to GND) |
| Ack button | GPIO 0 (BOOT button, active-low, internal pull-up) |

## Wiring

```
ESP32          SSD1306
-----          -------
3.3V    →     VCC
GND     →     GND
GPIO5   →     SDA
GPIO4   →     SCL

ESP32          LED
-----          ---
GPIO14  →     Anode (via ~330 Ω resistor)
GND     →     Cathode

ESP32          Button
-----          ------
GPIO0   →     One leg
GND     →     Other leg   (internal pull-up; press = active low)
```

## Configuration

Edit `main/main.c` to set your WiFi credentials:

```c
#define WIFI_SSID  "YourNetworkName"
#define WIFI_PASS  "YourPassword"
```

## Build & Flash

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/) v6.0 or later. Flash is 4 MB; the partition table uses two 1 MB OTA slots.

```bash
idf.py build
idf.py -p COM4 flash
```

> **Note:** If auto-reset fails (common when WiFi is active), enter download mode manually:
> hold BOOT → press and release EN/RST → release BOOT → then run the flash command.

## Web Status Page

After booting, open `http://<device-ip>` in a browser. The page shows:

```
Firmware: v1.2
Slot:     ota_0
Date:     30.06.2026
Time:     12:34:56
Uptime:   00:05:32
```

The page refreshes automatically every second. Refreshing stops when a file is selected for upload.

## OTA — Uploading New Firmware

1. Build the new firmware: `idf.py build` → produces `build/esp32_clock.bin`
2. Open `http://<device-ip>` in a browser
3. Select `esp32_clock.bin` with the file picker and click **Upload & Reboot**
4. The device writes the image to the inactive OTA slot, sets it as the boot target, and reboots

## OTA — Switching Between Installed Versions

The **Installed slots** section on the web page shows both OTA partitions with their firmware versions:

```
Installed slots
ota_0: v1.1   [Download]  [Boot this]
ota_1: v1.2   [Download]  [Boot this]  ← running
```

Click **Boot this** next to any slot to switch to that version immediately — no file upload needed. The currently-running slot's button is disabled.

Click **[Download]** to save the firmware from any slot as a `.bin` file (e.g. `esp32_clock_v1.1.bin`). The file is streamed directly from flash and can be uploaded back via the OTA file picker.

## Pre-built Binaries

Ready-to-flash binaries are in the `releases/` folder:

| File | Description |
|------|-------------|
| `esp32_clock_v1.1.bin` | Slot switching, uptime |
| `esp32_clock_v1.2.bin` | Adds free heap, download links, mobile file picker |
| `esp32_clock_v1.3.bin` | OLED shows firmware version alongside IP address |
| `esp32_clock_v1.4.bin` | Alarm: GPIO14 LED + OLED inversion, web UI, NVS persistence |
| `esp32_clock_v1.5.bin` | Snooze (5 min, web button) + physical ack button on GPIO0 |

## Dependencies

All dependencies are part of ESP-IDF. The following components are used:

- `nvs_flash` / `nvs` — non-volatile storage (settings + alarm persistence)
- `esp_wifi` — WiFi station mode
- `esp_event` — event loop
- `esp_netif` — network interface
- `esp_driver_i2c` — I2C master driver (new API)
- `esp_driver_gpio` — GPIO output (alarm LED)
- `lwip` — TCP/IP stack (NTP via SNTP)
- `esp_http_server` — HTTP server
- `app_update` — OTA update API
- `esp_timer` — uptime counter
