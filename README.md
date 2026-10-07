# ⚙️ Machine Monitor: industrial monitoring and anomaly detection

[![Tests](https://github.com/bouabdallah-yacine/predictive-maintenance-edge-ai/actions/workflows/ci.yml/badge.svg)](https://github.com/bouabdallah-yacine/predictive-maintenance-edge-ai/actions/workflows/ci.yml)
[![Live demo](https://img.shields.io/badge/%E2%96%B6%20Live%20demo-open%20in%20browser-F5A524)](https://bouabdallah-yacine.github.io/predictive-maintenance-edge-ai/)

> **▶ [Try the live demo](https://bouabdallah-yacine.github.io/predictive-maintenance-edge-ai/)**: the embedded AI running in your browser, no install needed.

An IoT **predictive maintenance** system: sensors monitor a machine (temperature, vibration, current),
a microcontroller running **FreeRTOS** detects drifts, an **embedded neural network (TinyML)** spots
abnormal behaviour, and a **real-time web dashboard** shows the machine state and alerts.

> ✅ Project **fully validated in simulation** (Wokwi, Renode, MQTT simulator), designed to be ported directly to real hardware.

![Architecture](docs/architecture.svg)

```
Sensors (DHT22, MPU6050, ACS712)
   ↓  GPIO / I2C / ADC
STM32 + FreeRTOS  ── UART frame "$MM,...*CS" ──►  ESP32 + FreeRTOS
                                                   ↓ Wi-Fi / MQTT
                                                 Broker (Mosquitto / HiveMQ)
                                                   ↓
                                                 Node.js: analysis (thresholds, z-score, trend)
                                                   ↓                    ↓ Socket.io
                                                 MongoDB             React Dashboard
```

## Features

| Detection | Method | Alert |
|---|---|---|
| Temperature too high | 60 / 75 °C thresholds with hysteresis | ⚠️ Warning → 🚨 Overheating |
| Abnormal vibration | RMS over a 1 s window (100 Hz) + **z-score** | 🚨 Anomaly |
| Abnormal current | ADC average, 3.5 / 4.5 A thresholds | 🚨 Possible failure |
| Upcoming overheating | **Linear regression** on temperature | 🔮 "Overheating in ~N min" |
| Machine disconnected | **MQTT Last Will** + server watchdog | 📡 Offline |
| Abnormal combination (e.g. 45 °C with no load) | **Neural network embedded in the ESP32** (TinyML) | 🤖 AI anomaly + probable cause |
| Alert on your phone | **Telegram** bot (critical, predictive and AI alerts, 60 s anti-spam) | 📱 Notification |

Two acquisition nodes: an **ESP32** (DHT22, MPU6050, ACS712, OLED) and an **STM32 Nucleo-C031C6 running FreeRTOS**
(NTC, HIH-4030, MPU6050, ACS712) connected to an **ESP32 gateway** over UART. History is stored in **MongoDB** (7 days).

Also included: local LEDs, buzzer and OLED display (the alarm works **without a network**), a button and an MQTT
"inject a fault" command for demos, buffering of measurements during Wi-Fi outages, alert
acknowledgement, CSV export, multiple machines, Docker Compose, GitHub Actions CI.

## Structure

```
firmware/
├── esp32-wokwi/      Standalone ESP32: 6 FreeRTOS tasks, sensors, OLED, MQTT  ← run this on Wokwi
├── esp32-bridge/     ESP32 UART → MQTT gateway (full architecture with the STM32)
├── esp32-bridge-vscode/ the same gateway, as a PlatformIO project that runs on Wokwi
├── esp32-vscode/     ESP32 PlatformIO project for Wokwi in VS Code
├── stm32-nucleo/      STM32 Nucleo-C031C6 + FreeRTOS (PlatformIO), runs on Wokwi
ai/                   Edge AI: network training (NumPy) → C code for the ESP32
└── stm32/            STM32 FreeRTOS (CMSIS-RTOS v2), sensor drivers, UART protocol, Renode
gateway/              Python UART/Renode → MQTT gateway (replaces the ESP32 in simulation)
simulator/            Machine simulator (realistic fault scenarios)
backend/              Node.js: MQTT → analysis → MongoDB → Socket.io + REST API
frontend/             React dashboard (Vite + Recharts)
docker-compose.yml    Mosquitto + MongoDB + backend + dashboard (+ simulator)
```

---

## 🚀 Quick start

### Option 1: Everything locally with Docker (the simplest)

```bash
docker compose --profile sim up --build
```
Open **http://localhost:5173**. Two simulated machines automatically cycle through:
normal → overheating → bearing wear → overcurrent.

### Option 2: Without Docker

Requirements: Node.js 20+. MongoDB is optional (without it, data is stored in memory).

```bash
# Terminal 1 — backend
cd backend && cp .env.example .env && npm install && npm start

# Terminal 2 — dashboard
cd frontend && npm install && npm run dev          # http://localhost:5173

# Terminal 3 — simulator (keys: n/o/b/c to switch scenario)
cd simulator && npm install && npm start
```
By default everything goes through the public broker `broker.hivemq.com`: no Mosquitto to install.

### Option 3: ESP32 simulated on Wokwi (the most impressive)

1. Go to **https://wokwi.com/projects/new/esp32**.
2. Replace `sketch.ino` and `diagram.json` with the ones from `firmware/esp32-wokwi/`.
3. **Library Manager** tab → add the libraries listed in `libraries.txt` (or create a `libraries.txt` file with that content).
4. Start the simulation ▶. The serial monitor shows `[MQTT] connected`.
5. Start the **backend** and the **dashboard** (option 2, without the simulator): Wokwi data arrives live.

**Triggering anomalies in Wokwi:**
- Click the **DHT22** → raise the temperature above 60 °C, then 75 °C.
- Vibration is computed from the **variation** of the signal (RMS with the mean removed), so a constant MPU6050 value gives 0 g. To simulate vibration, use the "Fault" button (25 Hz imbalance).
- Turn the **potentiometer** (it simulates the ACS712 current sensor) to one end → overcurrent.
- Press the red **"Fault" button** → full simulated failure (25 Hz imbalance, overheating, overcurrent).
- Or click **"Inject a fault"** in the dashboard: the command travels down over MQTT to the ESP32.

> ⚠️ The broker is public: change `TOPIC_PREFIX` (same value in `sketch.ino`, `backend/.env` and the simulator) so you do not receive another user's data.

### Option 4: STM32 Nucleo-C031C6 + FreeRTOS simulated on Wokwi (VS Code)

Open `firmware/stm32-nucleo/` in VS Code → PlatformIO **Build** → **F1 › Wokwi: Start Simulator**,
then in `backend/`: `npm run stm32`. Wokwi exposes the STM32 UART on `localhost:4100`
(RFC2217); the gateway decodes the frames and publishes them over MQTT (machine **stm32-01**).
"Inject a fault" commands from the dashboard travel all the way down to the STM32.

> 💡 Wokwi pauses the simulation when its tab is not visible: keep the simulation window on screen (for example side by side with the dashboard), otherwise no frames arrive.

### Option 4b: full STM32 → UART → ESP32 → Wi-Fi architecture (two simulations)

1. STM32 simulation: `firmware/stm32-nucleo/` (serial port exposed on 4100).
2. ESP32 gateway simulation: `firmware/esp32-bridge-vscode/` (serial port exposed on 4200).
3. Virtual UART cable between the two: `cd backend && npm run cable`.

The STM32 takes the measurements, the ESP32 receives the frames over UART and publishes them over MQTT via Wi-Fi;
dashboard commands travel back down ESP32 → STM32. Keep both simulations visible.

### Option 5: STM32CubeIDE (HAL) + Renode

See [`firmware/stm32/README.md`](firmware/stm32/README.md): creating the CubeIDE project, running the `.elf`
under Renode, then `python gateway/uart_gateway.py --tcp localhost:3456`.

---

## 🤖 Edge AI (TinyML)

A neural network with 369 weights (≈ 1.5 KB) runs **inside the ESP32** and assesses every measurement.
It detects what thresholds cannot see: 45 °C while the motor draws no current
(cooling failure), ambient temperature despite 3 A (faulty sensor), vibration shocks
(bearing). Details, method and results: [`ai/README.md`](ai/README.md).

**Demo in Wokwi:** raise the temperature (STM32 NTC or ESP32 DHT22) to **45 °C**:
the thresholds stay "Normal", but the AI reports an anomaly (cause: temperature) in the
dashboard and on Telegram.

## 🧪 Tests

```bash
cd backend && npm test                                  # anomalies, AI, frames, Telegram (18 tests)
python ai/train_model.py && gcc -I ai ai/test_tinyml.c -lm -o t && ./t   # AI: C == Python
cd firmware/stm32/test && gcc -Wall -Wextra -I../Core/Inc test_protocol.c ../Core/Src/protocol.c -o t && ./t
python gateway/uart_gateway.py --stdin --dry-run < gateway/sample_frames.txt
```
GitHub Actions CI (`.github/workflows/ci.yml`) compiles both ESP32 firmwares, runs the tests and builds the dashboard.

## 📡 Data format

**MQTT** `<prefix>/<deviceId>/telemetry` (1 Hz):
```json
{ "deviceId": "machine01", "seq": 128, "temperature": 42.3, "humidity": 45.1,
  "vibRms": 0.052, "vibPeak": 0.089, "current": 1.62, "state": "NORMAL",
  "levels": { "temperature": "NORMAL", "vibration": "NORMAL", "current": "NORMAL" },
  "faultInjected": false, "health": { "dht": true, "mpu": true },
  "ai": { "score": 0.002, "anomaly": false, "cause": "" } }
```
Also: `<prefix>/<id>/status` (`online` / `offline`, retained + Last Will) and `<prefix>/<id>/cmd`
(`fault_on`, `fault_off`, `mute`, `unmute`).

**STM32 → ESP32 UART**: `$MM,<seq>,<temp×10>,<hum×10>,<vib mg>,<peak mg>,<current mA>,<flags>*<XOR>\r\n`

**REST API**: `GET /api/health`, `/api/devices`, `/api/telemetry?deviceId=&minutes=`, `/api/alerts`,
`/api/stats/:id`, `/api/export.csv`, `POST /api/alerts/:id/ack`, `POST /api/devices/:id/cmd`.

## 🔒 Security

| Mode | Broker | Encryption | Authentication |
|---|---|---|---|
| Demo (default) | public `broker.hivemq.com` | ❌ | ❌ |
| **Production** | private **EMQX Cloud Serverless** (or HiveMQ Cloud) | ✅ TLS 1.2, port 8883, root certificate verified by the ESP32 | ✅ username / password |

- ESP32 firmwares: copy `include/secrets.example.h` to `include/secrets.h` (excluded from Git) and fill it in.
- Backend: `MQTT_URL=mqtts://<broker-address>:8883`, `MQTT_USERNAME`, `MQTT_PASSWORD` in `.env`.
- Secrets (`.env`, `secrets.h`, Telegram token) are never committed.

## 🗺️ Possible improvements

- FFT on vibration (ESP-DSP / CMSIS-DSP) to identify the fault frequency
- Training the AI on real machine data (instead of the physical model)
- Per-device access rights (ACLs), OTA updates for the ESP32 firmware

## License

© 2026 Yacine — all rights reserved. Code published for viewing only (see [`LICENSE`](LICENSE)).
