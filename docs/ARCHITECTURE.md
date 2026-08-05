# BlimpSoftware architecture

## Scope

BlimpSoftware controls one teleoperated blimp. There is no autonomous mode,
route planner, GPS, ML inference, routine engine, or multi-aircraft scheduler.
The narrow scope keeps the command path and aircraft safety behavior easy to
inspect.

## Components

| Component | Runtime | Responsibilities |
|---|---|---|
| Browser dashboard | Preact application | Camera display, attitude indicator, status, outputs, E-stop UI |
| Python base station | FastAPI/uvicorn | Web API, WebSocket broadcast, ping timing, command link, video bridge |
| PS4 reader | pygame thread | Read axes/buttons, deadzone, arcade mixing, E-stop actions |
| UDP command link | Python socket thread | Send NDJSON and receive telemetry/pong on port 5004 |
| UDP video receiver | Python socket thread | Reassemble newest complete JPEG frame on port 5005 |
| ESP32-P4 application | ESP-IDF/FreeRTOS | Actuator PWM, safety, IMU, camera, telemetry, UDP protocol |
| ESP32-C6 companion | ESP-Hosted firmware | 2.4 GHz Wi-Fi radio transported to P4 over SDIO |

## End-to-end data flow

### Control

1. pygame samples the PS4 controller at approximately 30 Hz.
2. Right-stick Y becomes throttle and right-stick X becomes steering.
3. Arcade mixing produces logical left and right values in `[-1, +1]`.
4. Left-stick Y becomes a shared servo angle in `[0, 180]` degrees.
5. The Python UDP link sends a `control` NDJSON datagram to the P4.
6. The P4 clamps all values and stores them under a mutex.
7. Hardware-only reversal is applied while converting values to PWM pulses.
8. The watchdog returns both ESCs to 1500 us if control is absent for 400 ms.

### Telemetry and latency

1. The P4 sends logical actuator state and attitude at 10 Hz.
2. IMU-native roll/pitch are mapped to the configured blimp axes, optionally
   inverted, and expressed relative to the startup reference pose.
3. The base station records the newest state and considers it offline after
   three seconds without telemetry or pong.
4. Once per second, the base station sends a numbered ping.
5. The P4 immediately echoes its nonce in a pong.
6. Python computes round-trip latency and broadcasts state to browsers at 20 Hz.

### Video

1. IMX219 captures 1536 x 1232 RAW10 over MIPI CSI.
2. The P4 downsamples and demosaics RGGB into 480 x 384 RGB565.
3. The hardware JPEG unit encodes YUV 4:2:2 at quality 65.
4. Each JPEG is split into MTU-sized UDP packets and sent to port 5005.
5. Python retains only the newest frame assembly per aircraft.
6. Incomplete frames are abandoned when a newer sequence arrives.
7. FastAPI exposes the newest complete frame as MJPEG to the dashboard.

This newest-frame policy favors low latency over guaranteed delivery. A lost
UDP packet drops one frame instead of delaying later frames through retransmits.

## P4 FreeRTOS task model

| Task | Priority | Stack | Activity |
|---|---:|---:|---|
| `blimp_udp_rx` | 14 | 6144 B | Blocking receive and command parsing |
| `control_watchdog` | 12 | 3072 B | Safety check every 20 ms |
| `mpu6050` | 10 | 4096 B | IMU read/filter every 10 ms |
| `blimp_telemetry` | 8 | 4096 B | Telemetry every 100 ms |
| `app_main` capture loop | main-task context | project default | Camera capture, conversion, encode, send |

ESP-IDF HTTP-server and ESP-Hosted tasks are created by their respective
components. Their priorities and stacks use component configuration defaults.

## Resource ownership

- One I2C master owns GPIO7/8. Both IMX219 SCCB (`0x10`) and MPU6050
  (`0x68`/`0x69`) use the same bus handle.
- LEDC timer 0/channel 0 generates the camera XCLK.
- LEDC timer 1/channels 1–4 generate ESC and servo pulses.
- A control-state mutex protects logical actuator state and E-stop state.
- An attitude mutex protects the latest roll/pitch/yaw estimate.
- A peer-address mutex protects the learned base-station destination.
- The HTTP JPEG queue contains at most one frame.

## Safety model

The aircraft does not rely on the browser or Python process to stop motors.
Safety is enforced at the final P4 output layer:

- boot initializes both ESC signals to neutral;
- all commands are range-clamped;
- E-stop is latched on the P4;
- clearing E-stop leaves motors neutral until a new control packet arrives;
- control loss longer than 400 ms neutralizes both motors;
- reversal changes physical PWM direction without changing logical state;
- servo output is constrained to its configured pulse range.

The watchdog stops thrust but retains the last servo angle, avoiding an abrupt
tilt movement during a momentary network interruption.

## Base-station process model

The normal production path is one Python process:

- uvicorn/FastAPI event loop serves HTTP and WebSocket clients;
- one pygame thread reads the PS4 controller;
- one command-link thread receives NDJSON;
- one video thread receives and reassembles JPEG fragments.

The built Preact dashboard is mounted at `/`. `/ws` carries UI snapshots and
browser E-stop actions. `/video/blimp.mjpg` carries the reconstructed stream.

In `--sim` mode the UDP link and video receiver are replaced by an in-process
simulated blimp. The same browser and controller paths are exercised, but no
camera frames are synthesized.
