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
- HTTP status page on port 80: firmware version, date, time
- OTA firmware update via the web page (no USB required after initial flash)

## Hardware

| Component | Detail |
|-----------|--------|
| MCU | ESP32 (tested on ESP32-D0WDQ6) |
| Display | SSD1306 OLED 128×64, I2C address `0x3C` |
| SDA | GPIO 5 |
| SCL | GPIO 4 |

## Wiring

```
ESP32          SSD1306
-----          -------
3.3V    →     VCC
GND     →     GND
GPIO5   →     SDA
GPIO4   →     SCL
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

## OTA Update

After the initial USB flash, subsequent updates can be uploaded over WiFi:

1. Build the new firmware: `idf.py build` → produces `build/esp32_clock.bin`
2. Open `http://<device-ip>` in a browser (device on same network)
3. Pick `esp32_clock.bin` with the file picker and click **Upload & Reboot**
4. The device writes the image to the inactive OTA slot, sets it as the boot target, and reboots automatically

## Dependencies

All dependencies are part of ESP-IDF. The following components are used:

- `nvs_flash` — non-volatile storage
- `esp_wifi` — WiFi station mode
- `esp_event` — event loop
- `esp_netif` — network interface
- `esp_driver_i2c` — I2C master driver (new API)
- `lwip` — TCP/IP stack (NTP via SNTP)
