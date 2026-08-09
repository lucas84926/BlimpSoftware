# ESP32-P4 blimp bring-up

This checklist covers the integrated P4+C6, IMX219/OV5647 camera sensor, MPU6050, two-ESC, and two-servo build. For safety, perform initial actuator testing/calibration without propellers.

## 1. Wiring

| Function | Default connection |
|---|---|
| Left ESC signal | GPIO29 |
| Right ESC signal | GPIO28 |
| Left servo signal | GPIO5 |
| Right servo signal | GPIO4 |
| Camera/MPU6050 SDA | GPIO7 / board `SDA` label |
| Camera/MPU6050 SCL | GPIO8 / board `SCL` label |
| MPU6050 VCC | 3.3 V |
| MPU6050 ground | GND |
| Camera XCLK | GPIO20, normally routed by carrier |

The IMU and camera share SDA/SCL. They coexist because their I2C addresses are different. They must share one software bus controller as well as the wires.

Use an external supply sized for the motors and servos. Connect its ground to the P4 ground, but do not draw actuator power from the P4 3.3 V rail.

## 2. Configure

Activate ESP-IDF, then:

```bash
cd p4_blimp
idf.py menuconfig
```

Under **Example Configuration** set:

- the 2.4 GHz Wi-Fi SSID and password;
- UDP ports if 5004/5005 conflict locally;
- actuator GPIOs;
- the motor and servo reversal checkboxes;
- each ESC neutral pulse and logical forward/reverse motor scale;
- each tilt servo's physical neutral angle;
- IMU SCL 8, SDA 7, and power GPIO -1 for direct 3.3 V power.
- whether the startup pose is zero roll/pitch (enabled by default);
- native or swapped roll/pitch axes, plus independent direction inversion.

The P4-to-C6 transport is using ESP-Hosted, with SDIO slot 1, four data lines, GPIO14–19, and reset GPIO54.

## 3. Build and flash

```bash
idf.py build
idf.py flash monitor
```

A successful boot should include messages similar to:

```text
blimp_control: ESC GPIOs 29/28, tilt GPIOs 5/4
blimp_control: Reversed: motors L=1 R=0, servos L=1 R=0
blimp_control: Servo neutral angles: L=90 R=90 degrees
mpu6050: MPU6050 detected at 0x68 on SCL=8 SDA=7
mpu6050: Attitude mapping: axes=swapped invert roll=0 pitch=0 startup zero=1
mpu6050: Startup attitude zero captured: roll=... pitch=...
app_main: Camera Started (RAW10 Mode). Streaming active...
app_main: Got IP: 192.168.x.x
app_main: FPS: 16
```

Keep the aircraft motionless in the attitude that should be displayed as level during the approximately one-second gyro-bias calibration after IMU detection. With startup zero enabled, that pose is captured immediately afterward.

## 4. Camera-only validation

Open these addresses using the IP printed by the P4:

```text
http://<P4-IP>/
http://<P4-IP>:81/stream
```

The first page includes exposure, gain, digital brightness, and white-balance controls. Expected output is 480 x 384 at roughly 15–16 FPS, depending on Wi-Fi and scene complexity.

## 5. Start the base station

From the repository root:

```bash
python -m pip install -r requirements.txt
cd basestation-ui && npm install && npm run build && cd ..
python run_basestation.py --blimp-host <P4-IP> --host 0.0.0.0
```

Open `http://localhost:8000`. Confirm, in this order:

1. the blimp becomes online and ping latency appears;
2. the camera background updates;
3. roll and pitch respond correctly when the unpowered aircraft is tilted;
4. the PS4 controller is shown as connected;
5. Circle latches E-stop and Cross clears it;
6. with propellers removed, motor and servo directions match the UI;
7. stopping the base station returns both ESC signals to neutral within 400 ms.

Use `python run_basestation.py --controller-monitor` to inspect raw controller values without sending commands. Use `--sim` to validate the dashboard without binding aircraft UDP sockets.

## Troubleshooting

### IMU probe succeeds, then repeated I2C failures begin

This indicates GPIO7/8 were assigned to two independent I2C controllers. The current firmware creates `s_sensor_bus` once and passes that handle to both the camera SCCB and MPU6050 code. Do not change camera SCCB back to `init_sccb=true` while the IMU is on the same pins.

### IMU does not ACK

- Verify VCC is 3.3 V and ground is common.
- Verify SDA is GPIO7 and SCL is GPIO8; swapped lines will not work.
- Confirm the startup log probes `0x68` or `0x69`.
- Most breakout boards contain pull-ups. If using a bare sensor, add suitable pull-ups to 3.3 V; internal pulls are weak.
- If any board or sensor becomes hot, disconnect power immediately. Heat is an electrical wiring/short or damaged-device symptom, not a software condition.

### Attitude uses the wrong axis or direction

Under **Example Configuration**, choose **Native** when sensor roll should be blimp left/right roll and sensor pitch should be blimp up/down pitch. Choose **Swapped** for a sensor mounted a quarter-turn from that orientation. If the correct axis moves in the wrong direction, invert only that output axis. Startup zero removes a fixed mounting angle but does not change axis selection or direction.

### Wi-Fi retries

- ESP32-C6 supports 2.4 GHz Wi-Fi; ensure that band is enabled.
- Reason 201 means the AP was not found.
- Reason 202 means authentication failed.
- Reasons 210/211 mean the AP security mode did not satisfy the threshold.
- If ESP-Hosted reports a host/co-processor version mismatch and RPC instability continues, update the C6 slave firmware to a compatible ESP-Hosted release.

### Camera colors are wrong

The current module is interpreted as RGGB and converted to RGB565. A different camera module or sensor flip setting can change Bayer phase. Red/blue reversal means the Bayer order should be checked before adjusting white balance.

### Motors move unexpectedly

Disconnect propellers and tune each configured ESC neutral pulse until logical zero and E-stop both produce a complete stop. Then check the four reversal options in menuconfig. Reversal must not be implemented by swapping clamp
bounds. See the firmware README for direction-dependent thrust matching.

If a motor pod is slightly misaligned with the left stick centered, adjust only that side's servo neutral angle in one- or two-degree steps. The required sign depends on servo-horn mounting, so test the change with propulsion disabled.

### Basestation says UDP 5005 is already in use

Only one basestation video receiver should bind UDP port 5005. This usually means an earlier `run_basestation.py` process is still running. Identify it:

```bash
sudo lsof -nP -iUDP:5005
# or
sudo ss -lunp 'sport = :5005'
```

Stop the specific PID cleanly with `kill <PID>` and start the basestation again. Do not use `kill -9` unless the process refuses normal termination. A custom video port is possible, but `BLIMP_VIDEO_PORT` in the P4 firmware and
`--video-port` on the basestation must be changed to the same value.
