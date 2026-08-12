# BlimpSoftware

BlimpSoftware is a lightweight teleoperation stack for a single blimp. It combines a Python base station, browser dashboard, and ESP32-P4 flight firmware to provide low-latency manual control, live video streaming, and real-time telemetry over Wi-Fi.

The reference aircraft uses:

- two thrust motors driven by ESCs
- two motor-tilt servos
- an IMX219/OV5647 camera
- an MPU6050 IMU
- an ESP32-P4 with an ESP32-C6 for Wi-Fi

The system is designed around direct human control rather than autonomous flight.

## Features

- Single-aircraft teleoperation
- PS4 controller support with arcade-drive controls
- Independent thrust and synchronized motor-tilt control
- Aircraft-side safety features including:
    - latched emergency stop
    - 400 ms command watchdog
    - output clamping and direction handling
- Low-latency JPEG video streamed directly into the browser dashboard
- Live attitude visualization (roll, pitch, and gyro-integrated yaw)
- Real-time telemetry for:
    - actuator outputs
    - controller status
    - connection health
    - network latency
- Simple UDP/NDJSON control protocol
- Separate fragmented UDP video stream
- `--sim` mode for UI and controller development without aircraft hardware

## System overview

```text
PS4 controller -> Python base station -> UDP/5004 -> ESP32-P4 flight control
Browser UI     <- WebSocket/HTTP 8000 <- Telemetry and ping/pong
Camera view    <- MJPEG HTTP bridge   <- UDP/5005 JPEG fragments <- Camera/P4
                                           |
                                   ESP32-C6 Wi-Fi over SDIO
```

The base station manages:

- controller input
- command transmission
- telemetry
- video reassembly
- browser APIs

The ESP32-P4 firmware is responsible for controlling all actuators and safety-critical behavior, including:

- actuator output limits
- emergency stop
- command timeout handling
- output direction and clamping

This separation allows the aircraft to remain safe even if communication with the base station is interrupted.

## Controls

Tested with a DualShock4 (PS4) controller.

| Input | Function |
|---|---|
| Right stick up/down | Forward/reverse throttle |
| Right stick left/right | Arcade steering across both motors |
| Left stick up/down | Set both motor-tilt servos from 0–180 degrees |
| Circle | Latch E-stop |
| Cross | Clear E-stop; motors remain stopped until a new control frame |

### Axis layouts

Hosts disagree about which pygame axis carries which stick, so the base station
supports two layouts and picks one automatically at startup by checking which
axes rest at `-1` (the triggers) while the sticks are centred.

| Profile | Left stick Y | Right stick X/Y | L2/R2 |
|---|---|---|---|
| `legacy` | 1 | 3 / 4 | 2 / 5 |
| `standard` | 1 | 2 / 3 | 4 / 5 |

Detection needs the sticks centred and the triggers released when the
controller is first seen. If a control feels wrong, run
`--controller-monitor` — it prints the profile matching each sample — and pin
the result with `--controller-profile legacy|standard` (or the
`BLIMP_CONTROLLER_PROFILE` environment variable).

## Base station quick start

Requirements are Python 3.12+, Node/npm for building the UI, and a supported controller visible to pygame.

```bash
python -m pip install -r requirements.txt
cd basestation-ui
npm install
npm run build
cd ..
python run_basestation.py --blimp-host <P4-IP> --host 0.0.0.0
```

Open `http://localhost:8000`. The Python service serves the built dashboard, WebSocket state, and MJPEG camera bridge from the same port.

Commandline options:

| Option | Purpose |
|---|---|
| `--blimp-host ADDRESS` | P4 address receiving UDP commands |
| `--command-port PORT` | NDJSON command/telemetry port, default 5004 |
| `--video-port PORT` | JPEG-fragment receiver, default 5005 |
| `--host ADDRESS` | Dashboard bind address, default `127.0.0.1` |
| `--web-port PORT` | Dashboard HTTP port, default 8000 |
| `--sim` | Simulate one blimp without UDP hardware |
| `--no-controller` | Run without opening pygame controller input |
| `--controller-profile NAME` | Gamepad axis layout: `auto` (default), `legacy`, or `standard` |
| `--controller-monitor` | Print raw controller axes/buttons without commanding outputs |

For frontend development:

```bash
python run_basestation.py --sim --web-port 8001
cd basestation-ui
npm run dev
```

Open `http://localhost:5173`. Vite proxies the browser API to port 8001.

## ESP32-P4 firmware quick start

The primary firmware is the ESP-IDF 5.4 project in [p4_blimp](p4_blimp/README.md). It supports menuconfig-selectable IMX219 and OV5647 cameras. Its tested defaults are:

| Device | Connection |
|---|---|
| Left/right ESC | GPIO29 / GPIO28 |
| Left/right tilt servo | GPIO5 / GPIO4 |
| Camera + MPU6050 shared SDA | GPIO7 |
| Camera + MPU6050 shared SCL | GPIO8 |
| MPU6050 power | P4 3.3 V and GND |
| Camera XCLK | GPIO20, 24 MHz |

To start, activate ESP-IDF, then:

```bash
cd p4_blimp
idf.py menuconfig
idf.py build
idf.py flash monitor
```

Settings for the blimp can be configured in the SDK Configuration Editor, under Example Configuration.
The repository tracks sanitized defaults in `p4_blimp/sdkconfig.defaults`; the generated `p4_blimp/sdkconfig`, including real Wi-Fi credentials, is ignored.

Keep the aircraft still, at the attitude that should read as level, for the approximately one-second gyro calibration at boot. Startup roll/pitch zeroing is enabled by default. Native/swapped attitude axes and independent roll/pitch inversion are available under **Example Configuration**. Test motor direction, servo direction, failsafe, and E-stop with the propellers removed.

Open `http://<P4-IP>/` for the camera settings page. It provides runtime controls for:

- camera analog gain;
- sensor exposure time;
- software digital brightness;
- software red and blue white-balance gains.

The current implementation streams video at approximately 15–16 FPS.

## Origin and attribution

The BlimpSoftware basestation began as a modification of [NEXT Competition's RoverSoftware](https://github.com/NEXT-Competition/RoverSoftware). It has since been reduced and adapted for one teleoperated ESP32-P4 blimp, while retaining the original dashboard layout as its visual foundation.

## Communications summary

Each control datagram contains one newline-terminated JSON object. The base station sends control, E-stop, clear, and ping messages to UDP 5004. The P4 returns telemetry and pong messages to UDP 5004 at the learned base address. JPEG frames use a compact binary fragmentation protocol on UDP 5005.

```json
{"type":"control","left":0.4,"right":0.4,"left_tilt":90,"right_tilt":90}
{"type":"estop"}
{"type":"clear_estop"}
{"type":"ping","n":12}
```

```json
{"type":"telemetry","from":"blimp","estop":false,"left":0.4,"right":0.4,"left_tilt":90,"right_tilt":90,"roll":1.2,"pitch":-3.4,"yaw":87.0}
{"type":"pong","n":12}
```

## Documentation

- [System architecture](docs/ARCHITECTURE.md)
- [Wire protocols](docs/PROTOCOL.md)
- [P4 wiring, configuration, and bring-up](docs/P4_BRINGUP.md)
- [Detailed P4 firmware reference](p4_blimp/README.md)
