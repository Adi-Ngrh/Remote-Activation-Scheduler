# Remote Activation Scheduler

A scheduling system built on the ESP32-S3 microcontroller that allows users to remotely control when an external device turns on and off. Schedules are configured through a web-based control panel accessible from any browser on the same network.

## Overview

This project addresses the need for precise, automated control of device activation over time. The user defines a schedule by specifying a **start date/time**, an **activation duration**, and a **repeat interval**. At the scheduled moment, the ESP32 switches the connected device on, keeps it running for the specified duration, then switches it off. If a repeat interval is set, the schedule automatically advances to the next occurrence.

Timekeeping is handled by a DS3231 real-time clock module, which maintains accurate time independently of the microcontroller. On startup, the RTC synchronises with an NTP time server over Wi-Fi, ensuring the clock stays correct even after power loss.

## How It Works

1. The ESP32 hosts a local web page (the Control Panel) on its IP address.
2. The user opens the page in a browser, fills in the schedule form, and submits it.
3. The ESP32 stores the schedule in its internal file system (LittleFS) and programmes the RTC alarms accordingly.
4. When the RTC alarm triggers at the scheduled start time, the device pin is driven HIGH (device turns on).
5. A second alarm fires after the specified duration elapses, driving the pin LOW (device turns off).
6. If a repeat interval was configured, the schedule is automatically rescheduled for the next interval.

Device status (ON/OFF) is pushed to the web page in real time via Server-Sent Events, so the indicator updates without refreshing the page.

## Hardware Requirements

| Component | Purpose |
|---|---|
| ESP32-S3 development board | Main controller, Wi-Fi, and web server |
| DS3231 RTC module | Accurate timekeeping and alarm interrupts |
| Relay module or MOSFET | Switching the target device on/off (connected to GPIO 16) |

The DS3231 communicates over I2C (SDA on GPIO 8, SCL on GPIO 9). The alarm interrupt output (SQW) is connected to GPIO 7.

## Software Dependencies

The project is built with [PlatformIO](https://platformio.org/) using the Arduino framework and C++17. The following libraries are required (managed automatically by PlatformIO):

- **RTClib** (Adafruit) — DS3231 driver
- **ArduinoJson** (Benoît Blanchon) — JSON serialisation for schedule data
- **ESPAsyncWebServer** — asynchronous HTTP server and Server-Sent Events

## Project Structure

```
src/main.cpp          Firmware source (Wi-Fi, RTC, web server, scheduling logic)
data/index.html       Web interface — uploaded to ESP32 flash (LittleFS)
data/schedule.txt     Persistent schedule storage on flash
web/index.html        Web interface — intended for external server hosting
platformio.ini        Build and dependency configuration
```

### Deployment Modes

- **Self-hosted (current implementation):** The HTML file inside `data/` is uploaded to the ESP32's LittleFS partition. The ESP32 serves the page directly — no external server is needed. This is the default and fully functional mode.

- **Externally hosted (planned):** The HTML file inside `web/` is designed to be served from a separate web server (e.g., a Raspberry Pi or cloud instance). In this configuration, the ESP32 would only expose its REST API endpoints, while the front-end is loaded from an external host. This approach decouples the interface from the microcontroller and allows richer front-end tooling in the future.

## Building and Flashing

1. Install [PlatformIO](https://platformio.org/install) (VS Code extension or CLI).
2. Open the project folder in PlatformIO.
3. Set your Wi-Fi credentials in `src/main.cpp` (`wifi_ssid` and `wifi_password`).
4. Build and upload the firmware:
   ```
   pio run -t upload
   ```
5. Upload the web interface to flash:
   ```
   pio run -t uploadfs
   ```
6. Open the Serial Monitor at 115200 baud to verify initialisation and obtain the ESP32's IP address.
7. Navigate to that IP address in a browser to access the Control Panel.

## API Endpoints

| Method | Path | Description |
|---|---|---|
| GET | `/` | Serves the Control Panel web page |
| GET | `/update` | Returns all stored schedules as newline-delimited JSON |
| POST | `/upload` | Accepts a JSON schedule object and stores it |
| POST | `/delete` | Accepts a schedule ID (plain text) and removes it |
| SSE | `/events` | Pushes real-time device status (`true`/`false`) to connected clients |

## Configuration

The firmware defaults to **GMT+7**. To change the timezone, modify the `gmt_offset` constant in `main.cpp` (value in seconds, e.g., 3600 × your UTC offset).

## Author

Adi Nugroho