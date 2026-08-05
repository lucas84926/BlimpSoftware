# BlimpSoftware wire protocol

## Command and telemetry transport

Commands and telemetry use IPv4 UDP port 5004 by default. Each datagram contains one UTF-8 JSON object followed by `\n`. Receivers tolerate a datagram without the final newline, but senders should include it. Unknown message types and invalid JSON are ignored.

The P4 binds `0.0.0.0:5004`. The Python base station sends to the address given by `--blimp-host` and also binds its local UDP port 5004. When the P4 receives a valid packet, it remembers that packet's source IP. Telemetry and video are sent to that learned IP on their configured destination ports.

No authentication, encryption, acknowledgement, or replay protection is provided. Operate on a trusted private Wi-Fi network.

## Base-to-blimp messages

### Control

```json
{"type":"control","left":0.4,"right":0.2,"left_tilt":95,"right_tilt":95}
```

| Field | Type | Accepted range | Missing-field default |
|---|---|---:|---:|
| `left` | number | clamped to -1..+1 | 0 |
| `right` | number | clamped to -1..+1 | 0 |
| `left_tilt` | number | clamped to 0..180 degrees | 90 |
| `right_tilt` | number | clamped to 0..180 degrees | 90 |

Receiving control refreshes the 400 ms watchdog only when E-stop is not latched. Values are logical; output reversal is an aircraft hardware setting.

### E-stop

```json
{"type":"estop"}
```

Immediately latches E-stop and commands both ESC outputs to neutral. Servo angles are retained.

### Clear E-stop

```json
{"type":"clear_estop"}
```

Clears the latch but leaves both motors stopped. A subsequent control message is required before thrust resumes.

### Ping

```json
{"type":"ping","n":12}
```

`n` is a numeric nonce chosen by the base station. The P4 echoes it in a pong.

## Blimp-to-base messages

### Telemetry

```json
{"type":"telemetry","from":"blimp","estop":false,"left":0.4,"right":0.2,"left_tilt":95.0,"right_tilt":95.0,"roll":1.25,"pitch":-2.50,"yaw":84.10}
```

Telemetry is sent every 100 ms after the P4 has learned a base-station address. Motor and servo values are logical command values before hardware reversal.

| Field | Meaning |
|---|---|
| `from` | Aircraft identifier; currently always `blimp` |
| `estop` | Aircraft-side latched E-stop state |
| `left`, `right` | Logical motor values in -1..+1 |
| `left_tilt`, `right_tilt` | Logical servo angles in degrees |
| `roll`, `pitch` | Configured blimp-axis attitude in degrees, relative to the startup reference when enabled |
| `yaw` | Gyro-integrated heading in degrees; expected to drift |

### Pong

```json
{"type":"pong","n":12}
```

The base station matches `n` to its send time and reports round-trip latency.

## Controller mapping before transmission

The base station applies an 0.08 deadzone. With throttle `t` and steering `s`:

```text
scale = max(1, abs(t) + abs(s))
left  = (t + s) / scale
right = (t - s) / scale
```

This preserves the requested turn ratio while preventing either motor command from exceeding magnitude 1. Right-stick vertical is negated because pygame reports upward movement as a negative axis value.

## JPEG video fragmentation

JPEG frames are sent from the P4 to UDP port 5005. The maximum datagram is 1400 bytes: a 28-byte header and up to 1372 bytes of JPEG payload.

All integer fields use network byte order (big endian).

| Offset | Size | Field | Description |
|---:|---:|---|---|
| 0 | 4 | magic | ASCII `UCV1` |
| 4 | 16 | aircraft ID | UTF-8 `blimp`, NUL padded |
| 20 | 4 | frame sequence | Unsigned 32-bit counter |
| 24 | 2 | chunk index | Zero-based chunk number |
| 26 | 2 | chunk count | Total chunks in this JPEG |
| 28 | <=1372 | payload | Slice of JPEG data |

The receiver groups chunks by aircraft ID and sequence. A frame becomes visible only after all indices are present. If a newer sequence arrives first, the incomplete older frame is discarded. Sequence comparison handles 32-bit wrap.

## Browser-facing interfaces

The Python base station exposes:

| Endpoint | Transport | Purpose |
|---|---|---|
| `/` | HTTP | Built dashboard |
| `/ws` | WebSocket JSON | Fleet snapshots and browser E-stop actions |
| `/video/blimp.mjpg` | HTTP multipart MJPEG | Latest reassembled camera frames |

The P4 also exposes standalone diagnostics:

| Endpoint | Port | Purpose |
|---|---:|---|
| `/` | 80 | Camera page with exposure/gain/color controls |
| `/control?...` | 80 | Apply camera control values |
| `/stream` | 81 | Direct MJPEG diagnostic stream |
