# ESP32 NTP Clock

An ESP32 project that displays the current Norwegian time (CET/CEST) on an SSD1306 OLED display, synchronized over WiFi via NTP.

## Features

- Connects to WiFi and syncs time from `pool.ntp.org`
- Displays current time in 24-hour format (HH:MM:SS)
- Shows assigned IP address on the top of the screen
- Shows `--:--:--` while waiting for NTP sync
- Shows `0.0.0.0` if WiFi is not connected
- Correct Norwegian timezone (UTC+2 CEST in summer, auto-switches to UTC+1 CET in winter)

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

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/) v6.0 or later.

```bash
idf.py build
idf.py -p COM4 flash
```

> **Note:** If auto-reset fails (common when WiFi is active), enter download mode manually:
> unplug USB → hold BOOT button → plug USB back in → wait 2 seconds → release BOOT → flash with `--before no-reset`.

## Dependencies

All dependencies are part of ESP-IDF. The following components are used:

- `nvs_flash` — non-volatile storage
- `esp_wifi` — WiFi station mode
- `esp_event` — event loop
- `esp_netif` — network interface
- `esp_driver_i2c` — I2C master driver (new API)
- `lwip` — TCP/IP stack (NTP via SNTP)
