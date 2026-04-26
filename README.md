# Remote Activation Scheduler

An IoT scheduling system that lets users remotely control when a physical device turns on and off. Schedules are managed through a web interface hosted on an Ubuntu server. The ESP32-S3 microcontroller receives commands over MQTT and directly drives the hardware output.

## System Architecture

```
Browser  ──HTTP/WebSocket──►  Ubuntu Server (Node.js)  ──MQTT/TLS──►  ESP32-S3  ──GPIO──►  Device
                                      │                                    │
                               (serves web UI,                      (stores schedules,
                                bridges REST ↔ MQTT)                 controls RTC alarms)
```

Three components work together:

1. **ESP32-S3 firmware** — subscribes to MQTT topics, stores schedules in flash (LittleFS), sets DS3231 RTC alarms to trigger device activation/deactivation, and publishes device status back over MQTT.
2. **Node.js backend** (`web/server.js`) — runs on the Ubuntu server. Bridges the browser and the ESP32: it exposes REST endpoints to the browser, translates those calls into MQTT publishes, and forwards incoming MQTT messages to the browser via WebSocket.
3. **Web frontend** (`web/index.html`) — static HTML page served by the Node.js server. Users add/remove schedules and see live device status via WebSocket.

## How a Schedule Works

A schedule has four fields:

| Field | Type | Description |
|---|---|---|
| `id` | 64-bit integer (string) | Unique identifier |
| `startTime` | Unix epoch (seconds) | When the device should turn on |
| `duration` | uint16, seconds | How long the device stays on |
| `interval` | uint16, seconds | Repeat interval; always ≥ 1 minute and must be greater than `duration` |

**Activation flow:**
1. User submits a schedule in the browser → POST to `/schedule/add` on the Node.js server.
2. Node.js forwards it as JSON to the MQTT topic `ras/schedule/add`.
3. ESP32 receives it, appends it to `/schedule.txt` in LittleFS, then reprograms the DS3231 alarms.
4. **RTC Alarm 2** fires at `startTime` → ESP32 drives GPIO 16 HIGH (device on), publishes `ras/status = true`.
5. **RTC Alarm 1** fires at `startTime + duration` → GPIO 16 LOW (device off), publishes `ras/status = false`.
6. The schedule is rescheduled for `startTime + interval` and saved back to flash, repeating indefinitely.
7. The browser receives the status update over WebSocket and updates the UI instantly.

## Hardware

| Component | Purpose | Pins |
|---|---|---|
| ESP32-S3 | Main controller, Wi-Fi, MQTT client | — |
| DS3231 RTC | Accurate timekeeping and alarm interrupts | SDA=8, SCL=9, SQW=7 |
| Relay or MOSFET | Switches the target device on/off | GPIO 16 |

The DS3231 communicates over I2C. Its SQW interrupt pin is connected to GPIO 7 and triggers a hardware ISR that notifies the FreeRTOS scheduler task.

On startup the ESP32 syncs the RTC time from NTP (`pool.ntp.org`) so the clock stays accurate after power loss.

## Project Structure

```
src/main.cpp              ESP32 firmware (Wi-Fi, MQTT, RTC, scheduling logic)
include/secrets.h         Wi-Fi credentials, MQTT broker URL, TLS certificate
data/schedule.txt         Persistent schedule storage in LittleFS (newline-delimited JSON)
web/server.js             Node.js backend — REST API + WebSocket + MQTT bridge
web/index.html            Web frontend — schedule management UI
web/package.json          Node.js dependencies
platformio.ini            PlatformIO build configuration
```

## MQTT Topics

All communication between the Ubuntu server and the ESP32 goes through a TLS-secured MQTT broker (`mqtts://`, port 8883).

| Topic | Direction | Payload | Description |
|---|---|---|---|
| `ras/schedule/add` | Server → ESP32 | JSON schedule object | Add a new schedule |
| `ras/schedule/delete` | Server → ESP32 | Schedule ID (string) | Remove a schedule by ID |
| `ras/schedule/get` | Server → ESP32 | _(empty)_ | Request the full schedule list |
| `ras/schedule/list` | ESP32 → Server | Newline-delimited JSON | Schedule list response |
| `ras/status` | ESP32 → Server | `"true"` / `"false"` | Device on/off status (retained) |

## Backend REST API

The Node.js server (`web/server.js`) exposes these endpoints to the browser:

| Method | Path | Body | Description |
|---|---|---|---|
| POST | `/schedule/add` | JSON schedule object | Forward add request to ESP32 via MQTT |
| POST | `/schedule/delete` | Schedule ID (plain text) | Forward delete request to ESP32 via MQTT |
| GET | `/schedule/get` | — | Ask ESP32 to publish its schedule list |

Schedule list and device status are pushed to the browser over a persistent **WebSocket** connection. New clients receive the last known device status immediately on connect.

## Setup

### Firmware (ESP32)

1. Install [PlatformIO](https://platformio.org/install).
2. Fill in `include/secrets.h` with your Wi-Fi credentials, MQTT broker address, and TLS CA certificate.
3. Set `gmt_offset` in `src/main.cpp` to your UTC offset in seconds (default is `25200` for GMT+7).
4. Build and flash the firmware:
   ```
   pio run -t upload
   ```
5. Upload the LittleFS partition (schedule storage):
   ```
   pio run -t uploadfs
   ```
6. Open the Serial Monitor at 115200 baud to verify startup and confirm Wi-Fi/MQTT connection.

### Backend Server (Ubuntu)

1. Install Node.js on the Ubuntu server.
2. Copy the `web/` folder to the server.
3. Create a `.env` file inside `web/` with the following variables:
   ```
   PORT=3000
   MQTT_URL=mqtts://<broker-host>:8883
   MQTT_USERNAME=<username>
   MQTT_PASSWORD=<password>
   ```
4. Install dependencies and start the server:
   ```
   cd web
   npm install
   npm start
   ```
5. Open `http://<server-ip>:3000` in a browser to access the Control Panel.

## Key Implementation Details

- Schedules are stored in LittleFS as newline-delimited JSON objects (up to 50 schedules in memory at once).
- Schedule IDs are 64-bit integers represented as strings to avoid the Year 2038 problem.
- `startTime` is stored in flash as a raw UTC epoch. The `gmt_offset` is applied in memory only when loading into the schedule array.
- The DS3231 uses two alarms simultaneously: Alarm 2 for the start event, Alarm 1 for the end (duration) event.
- Alarm ISR and MQTT event handler both communicate with the `ScheduleTask` FreeRTOS task via `xTaskNotify` with bit flags (`NOTIFY_SCHEDULE_UPDATED`, `ALARM_TRIGGERED`), avoiding any blocking calls in interrupt/callback context.
- The firmware is built with C++17 (enforced via `static_assert`).

## Author

Adi Nugroho