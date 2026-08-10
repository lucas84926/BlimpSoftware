# BlimpSoftware firmware: ESP32-P4 + ESP32-C6 + IMX219/OV5647

This is the primary aircraft firmware for the Waveshare ESP32-P4-WiFi6 board. The P4 runs camera processing, flight I/O, the MPU6050 attitude estimator, and the BlimpSoftware protocol. Its companion ESP32-C6 supplies 2.4 GHz Wi-Fi through ESP-Hosted over the board's internal SDIO connection.

The firmware is currently teleoperation-only. It controls one blimp with two bidirectional thrust motors and one tilt servo per motor.

## Implemented features

- two 50 Hz bidirectional ESC outputs with 1000–2000 us pulses;
- two 50 Hz tilt-servo outputs with 500–2500 us pulses;
- independent reversal settings for all four outputs;
- latched E-stop and a 400 ms command-loss motor failsafe;
- PS4-derived arcade-drive commands from the Python base station;
- UDP NDJSON control, telemetry, ping, and pong messages;
- 10 Hz MPU6050 roll, pitch, and integrated-yaw telemetry;
- IMX219 RAW10 or OV5647 RAW8 capture, software demosaic, hardware JPEG encoding, and UDP video;
- standalone HTTP camera diagnostics and exposure/color controls;
- automatic discovery of the base-station return address from received commands.

## Hardware and default pins

These are the project defaults and the currently tested arrangement. Confirm the carrier-board labels and disconnect propellers before first power-up.

| Function | P4 GPIO | Interface / notes |
|---|---:|---|
| Left ESC signal | 29 | LEDC channel 1, 50 Hz |
| Right ESC signal | 28 | LEDC channel 2, 50 Hz |
| Left tilt servo signal | 5 | LEDC channel 3, 50 Hz |
| Right tilt servo signal | 4 | LEDC channel 4, 50 Hz |
| Camera SCCB SDA | 7 | Shared I2C bus; IMX219 `0x10`, OV5647 `0x36` |
| MPU6050 SDA | 7 | Shared I2C bus, address `0x68` or `0x69` |
| Camera SCCB SCL | 8 | Shared I2C bus, 100 kHz |
| MPU6050 SCL | 8 | Shared I2C bus, 100 kHz |
| Camera XCLK | 20 or module | IMX219 default: GPIO20; OV5647 default: module-provided |
| MPU6050 VCC | 3.3 V | External power; power-enable GPIO is `-1` |
| MPU6050 GND | GND | Ground must be shared with the P4 |

GPIO7 and GPIO8 are deliberately one shared bus. `app_main` creates one ESP-IDF I2C master and passes the same handle to the MPU6050 and camera SCCB drivers. Do not create a second I2C controller on these pads; doing so remaps the pins away from the first controller and causes repeated transaction failures.

The internal P4-to-C6 SDIO link uses GPIO14–19 and reset GPIO54 in the current board configuration. Do not assign aircraft peripherals to those pins.

### PWM behavior

| Logical value | ESC pulse | Meaning |
|---:|---:|---|
| `-1.0` | 1000 us | Full reverse |
| `0.0` | 1500 us | Neutral / stopped |
| `+1.0` | 2000 us | Full forward |

Servo angles are clamped to 0–180 degrees and mapped linearly to 500–2500 us. Reversal is applied only at the PWM layer: a reversed motor negates its logical command around 1500 us, and a reversed servo uses `180 - angle`. Telemetry continues to report logical, non-reversed values.

The default physical configuration reverses the left motor and left servo.

## Software architecture and tasks

| Task / context | Priority | Period or trigger | Responsibility |
|---|---:|---|---|
| `app_main` / capture loop | ESP-IDF main task | Every camera frame | Startup, RAW10 capture, demosaic, JPEG encode, video dispatch |
| `blimp_udp_rx` | 14 | Blocking UDP receive | Parse control, E-stop, clear, and ping messages |
| `control_watchdog` | 12 | 20 ms | Stop both motors after 400 ms without a control packet |
| `mpu6050` | 10 | 10 ms | Read IMU and update the attitude estimate at about 100 Hz |
| `blimp_telemetry` | 8 | 100 ms | Send actuator and attitude telemetry at 10 Hz |
| HTTP server tasks | ESP-IDF HTTP defaults | Client-driven | Serve camera controls on port 80 and MJPEG on port 81 |
| ESP-Hosted tasks | Component-managed | Event-driven | Carry Wi-Fi traffic between the P4 and C6 over SDIO |

Shared actuator and attitude state is protected by FreeRTOS mutexes. The camera HTTP queue has depth one: a stale frame is discarded when a new frame arrives, which prevents a slow browser from adding flight-video latency.

## Communications

| Direction | Transport | Default port | Purpose |
|---|---|---:|---|
| Base station -> P4 | UDP NDJSON | 5004 | Control, E-stop, clear E-stop, ping |
| P4 -> base station | UDP NDJSON | 5004 | Telemetry and pong |
| P4 -> base station | Fragmented JPEG/UDP | 5005 | Low-latency camera video |
| Browser -> P4 | HTTP | 80 | Standalone camera page and controls |
| Browser -> P4 | HTTP MJPEG | 81 | Standalone diagnostic stream |
| Browser <-> Python base | HTTP/WebSocket | 8000 | Dashboard, UI state, and E-stop actions |

The P4 initially knows only the base-station destination configured on the Python command sender. When the P4 receives any valid NDJSON datagram, it remembers the source IP and uses that IP for telemetry and video. This avoids compiling a base-station IP into the aircraft firmware.

See [the repository protocol reference](../docs/PROTOCOL.md) for message fields and the 28-byte video-fragment header.

## Camera pipeline and settings

| Setting | Current value |
|---|---|
| Sensor | Sony IMX219 or OmniVision OV5647, MIPI CSI |
| Sensor capture | IMX219: 1536 x 1232 RAW10; OV5647: 800 x 800 RAW8 |
| Bayer interpretation | IMX219: RGGB; OV5647: GBRG |
| Flight output | 480 x 384 RGB565 |
| JPEG encoder | ESP32-P4 hardware encoder |
| Chroma sampling | YUV 4:2:2 |
| JPEG quality | 65 |
| XCLK | IMX219: 24 MHz on GPIO20; OV5647 default: supplied by module |
| SCCB | 100 kHz on shared GPIO7/8 bus |
| Frame buffers | Two V4L2 capture buffers |
| HTTP frame queue | One, newest frame wins |
| Typical observed rate | Approximately 15–16 FPS; hardware-dependent |

The conversion loop uses precomputed source offsets, RGB565 output, and `-O2`. Wi-Fi power saving is disabled to favor predictable command and video latency. One encoded JPEG is reused for both the UDP flight stream and HTTP diagnostics.

Open `http://<P4-IP>/` for the diagnostic page. It provides runtime controls for:

- IMX219 analog gain and sensor exposure time (ignored by the OV5647 build);
- software digital brightness;
- software red and blue white-balance gains.

Select the sensor under **Example Configuration > Camera sensor**. The raw modes and 480 x 384 flight output are compile-time settings in `main/main.c`. JPEG quality is in `main/jpeg_enc.c`.

## MPU6050 behavior

The IMU driver probes addresses `0x68` and `0x69`, configures the bus at 100 kHz, wakes the device, and selects:

- accelerometer range: +/-2 g;
- gyroscope range: +/-250 degrees/second;
- digital low-pass filter: approximately 44 Hz;
- sampling loop: approximately 100 Hz.

At boot it collects 200 gyro samples over about one second. Keep the blimp still during this calibration. Roll and pitch use an angle-wrapped 98/2 percent complementary filter, so an upside-down mounting does not cause a discontinuity at +/-180 degrees. Yaw is gyro-integrated and will drift because the MPU6050 has no magnetometer.

By default, the first filtered pose after gyro calibration becomes zero roll and zero pitch. Hold the blimp still at the attitude that should appear level until the startup-zero log appears. The default axis mapping is **swapped**, meaning sensor pitch is displayed as blimp left/right roll and sensor roll as blimp up/down pitch. Select the native mapping when the sensor board is aligned conventionally. Roll and pitch can also be inverted independently; mapping and inversion happen before the startup reference is subtracted.

## ESP-IDF example configuration

Run `idf.py -B build-local menuconfig`, then open **Example Configuration**.

| Option | Default | Description |
|---|---:|---|
| Camera sensor | IMX219 | Select the local IMX219 or Espressif OV5647 driver |
| Camera XCLK GPIO | 20 | Defaults to -1 (module-provided clock) for OV5647 |
| Rotate camera image 180 degrees | enabled | Correct an upside-down camera mounting in software |
| Wi-Fi SSID | `myssid` | 2.4 GHz network name |
| Wi-Fi password | `mypassword` | WPA2/WPA3 network password |
| Command/telemetry port | 5004 | UDP NDJSON socket |
| Video port | 5005 | UDP JPEG fragments |
| Left ESC GPIO | 29 | Left motor signal |
| Right ESC GPIO | 28 | Right motor signal |
| Left tilt GPIO | 5 | Left servo signal |
| Right tilt GPIO | 4 | Right servo signal |
| Reverse left motor | enabled | Physical direction correction |
| Reverse right motor | disabled | Physical direction correction |
| Left/right ESC neutral | 1500 us | Per-ESC pulse that produces a complete stop |
| Left motor forward/reverse scale | 100% / 100% | Logical direction compensation |
| Right motor forward/reverse scale | 100% / 100% | Logical direction compensation |
| Steering sensitivity | 50% | Scale differential steering without changing straight-line throttle |
| Reverse left servo | enabled | Mirrored tilt correction |
| Reverse right servo | disabled | Mirrored tilt correction |
| Left/right servo neutral angle | 90 degrees | Physical output at logical 90 degrees |
| MPU6050 SCL GPIO | 8 | Must match shared camera SCL in this build |
| MPU6050 SDA GPIO | 7 | Must match shared camera SDA in this build |
| MPU6050 power GPIO | -1 | Disabled because VCC is connected to 3.3 V |
| Zero roll/pitch at startup | enabled | Treat the post-calibration pose as level |
| Tilt-axis mapping | swapped | Sensor pitch -> roll; sensor roll -> pitch |
| Invert displayed roll/pitch | disabled / disabled | Correct either displayed direction independently |

When the MPU shares GPIO7/8 with the camera, changing only the MPU Kconfig pins is not sufficient: the camera constants in `main/main.c` must describe the same bus. A separate IMU bus may be used on other pins by passing `NULL` rather than the shared camera bus to `mpu6050_start()`.

## ESC calibration and thrust matching

ESC calibration belongs in the P4 firmware because it describes the physical ESC, motor, and propeller pair. It therefore remains correct if a different basestation controls the blimp, and it is used by the onboard failsafe.

The two **ESC neutral pulse** settings replace the fixed 1500 us stop pulse. They are applied at boot, at zero command, during E-stop, and after the 400 ms command timeout. Adjust them with propellers removed, in small increments, until each motor is completely stopped. Typical changes should be only a few microseconds; investigate ESC calibration if a large correction is necessary.

The four **motor scale** settings compensate each motor independently in logical blimp directions. They are applied before the motor inversion option, so `forward` means blimp-forward even when the left ESC signal is reversed. Only reduce the faster side rather than trying to boost beyond 100 percent. For the observed case where the right motor is faster forward and the left motor is faster in reverse, begin with:

```text
Left forward:   100%       Right forward:  95%
Left reverse:    95%       Right reverse: 100%
```

Tune at a moderate fixed command, compare actual thrust or RPM, and adjust in one- or two-percent steps. The basestation motor bars intentionally continue to show requested logical commands; they cannot measure actual motor RPM.

## Servo neutral calibration

The left and right **servo neutral angle** settings independently map the logical 90-degree command (centered left stick) to a physical servo output. The default is 90 degrees. If one motor pod points slightly down at center, change only that servo's neutral angle by one or two degrees, flash, and test again. Whether the value needs to increase or decrease depends on horn mounting.

This calibration is applied after servo reversal, so increasing the configured neutral always increases the physical PWM angle. The mapping is piecewise: 0 degrees remains 0, neutral maps to the configured value, and 180 remains 180. This corrects center alignment without simply shifting and clipping one end.

## Build, flash, and run

The tracked `sdkconfig.defaults` provides the reference-board, actuator, and IMU defaults. ESP-IDF generates the ignored, machine-local `sdkconfig`; use that file for the real Wi-Fi credentials. `dependencies.lock` is tracked so ESP-IDF Component Manager resolves the same component versions on another checkout.

Activate ESP-IDF, then:

```bash
cd p4_blimp
idf.py menuconfig
idf.py build
idf.py flash monitor
```

On a clean checkout, the first configure/build downloads the managed components declared in `main/idf_component.yml`. The resulting `managed_components/` and `build/` directories are generated and intentionally ignored.

After `Got IP:` appears, start the base station from the repository root:

```bash
python run_basestation.py --blimp-host <P4-IP> --host 0.0.0.0
```

Then open `http://localhost:8000` on the base-station computer.

## Safety and troubleshooting

- Remove propellers while verifying pins, reversal, and E-stop behavior.
- Power ESCs and servos from an appropriate external supply; connect grounds.
- Do not power motors or servos from the P4's 3.3 V rail.
- If the P4 or IMU becomes hot, disconnect power immediately and inspect VCC, GND, SDA, and SCL for reversed connections or shorts before reconnecting.
- Repeated I2C errors after a successful IMU probe usually mean two controllers assigned to GPIO7/8. Use the shared bus handle implemented here.
- ESP32-C6 supports 2.4 GHz Wi-Fi. Disconnect reason 201 means AP not found, 202 means authentication failure, and 210/211 indicates security mismatch.
- If an ESP-Hosted version-mismatch warning accompanies RPC timeouts or Wi-Fi instability, update the C6 firmware to a compatible ESP-Hosted release.

More bring-up checks are in [P4 bring-up](../docs/P4_BRINGUP.md).
