import socket
import time

from basestation.fleet import FleetManager
from basestation.simulator import SimulatedBlimp
from basestation.controller_input import (
    DEFAULT_PROFILE, LEGACY_PROFILE, STANDARD_PROFILE, ControllerReader, detect_profile,
)
from basestation.simulator import SimulatedBlimp as _SimulatedBlimp
from basestation.discovery import (
    SERVICE, Blimp, _encode_name, _parse_records, _read_name, is_ip_address,
    resolve_host,
)
from robot.comms.protocol import decode, encode


def test_minimal_ndjson_roundtrip():
    message = {"type": "control", "left": 0.5, "right": -0.25,
               "left_tilt": 80, "right_tilt": 100}
    assert decode(encode(message)) == message


def test_one_blimp_attitude_and_outputs():
    fleet = FleetManager("blimp")
    fleet.handle({"type": "telemetry", "roll": 4.0, "pitch": -2.0,
                  "yaw": 91.0, "left": .3, "right": .4}, now=10)
    frame = fleet.snapshot(now=10.1)
    assert frame["selected"] == "blimp"
    assert len(frame["robots"]) == 1
    assert frame["robots"][0]["online"] is True
    assert frame["robots"][0]["pitch"] == -2.0


def test_ping_latency_is_reported():
    fleet = FleetManager()
    fleet.note_ping(7, 1.0)
    fleet.handle({"type": "pong", "n": 7}, now=1.025)
    assert fleet.snapshot(now=1.026)["robots"][0]["latency_ms"] == 25


def test_simulator_obeys_control_estop_and_ping():
    messages = []
    sim = SimulatedBlimp(messages.append)
    sim.send({"type": "control", "left": .5, "right": .25,
              "left_tilt": 70, "right_tilt": 70})
    sim.send({"type": "ping", "n": 4})
    assert messages[-1] == {"type": "pong", "n": 4}
    sim.send({"type": "estop"})
    assert sim._estop is True
    assert sim._left == sim._right == 0


def test_right_stick_arcade_mix():
    assert ControllerReader.arcade(0, 0) == (0, 0)
    assert ControllerReader.arcade(1, 0) == (1, 1)
    assert ControllerReader.arcade(-1, 0) == (-1, -1)
    assert ControllerReader.arcade(0, 1) == (1, -1)
    assert ControllerReader.arcade(0, -1) == (-1, 1)


def test_observed_ps4_axis_mapping():
    # Raw samples captured from the controller used with this base station.
    neutral = [-.027, -.035, -1, .012, .020, -1]
    up = [-.027, -.035, -1, .012, -1, -1]
    down = [-.027, -.035, -1, .012, 1, -1]
    left = [-.027, -.027, -1, -1, .012, -1]
    right = [-.027, -.027, -1, 1, .012, -1]
    assert ControllerReader.control_from_axes(neutral)[:2] == (0, 0)
    assert ControllerReader.control_from_axes(up)[:2] == (1, 1)
    assert ControllerReader.control_from_axes(down)[:2] == (-1, -1)
    assert ControllerReader.control_from_axes(left)[:2] == (-1, 1)
    assert ControllerReader.control_from_axes(right)[:2] == (1, -1)

    left_up = [.082, -1, -1, .004, .012, -1]
    left_down = [-.012, 1, -1, .004, .012, -1]
    assert ControllerReader.control_from_axes(left_up)[2] == 180
    assert ControllerReader.control_from_axes(left_down)[2] == 0

    assert detect_profile(neutral) is LEGACY_PROFILE


def test_standard_ps4_axis_mapping():
    # Raw samples from a host whose driver keeps the right stick on axes 2/3.
    neutral = [-.051, -.035, .012, -.012, -1, -1]
    up = [-.051, -.035, .004, -1, -1, -1]
    down = [-.051, -.035, .027, 1, -1, -1]
    left = [-.051, -.035, -1, .012, -1, -1]
    right = [-.051, -.035, 1, .012, -1, -1]
    control = lambda axes: ControllerReader.control_from_axes(axes, STANDARD_PROFILE)
    assert control(neutral)[:2] == (0, 0)
    assert control(up)[:2] == (1, 1)
    assert control(down)[:2] == (-1, -1)
    assert control(left)[:2] == (-1, 1)
    assert control(right)[:2] == (1, -1)

    left_up = [-.059, -1, .004, .012, -1, -1]
    left_down = [.004, 1, .004, .012, -1, -1]
    assert control(left_up)[2] == 180
    assert control(left_down)[2] == 0

    assert detect_profile(neutral) is STANDARD_PROFILE


def test_profile_detection_falls_back_when_ambiguous():
    # A trigger held down at startup matches neither layout cleanly.
    assert detect_profile([0, 0, 0, 0, 0, 0]) is DEFAULT_PROFILE
    assert detect_profile([0, 0, 0, 0, 0, 0], fallback=None) is None


def test_ip_literals_skip_mdns_discovery():
    assert is_ip_address("192.168.4.1") and is_ip_address("::1")
    assert not is_ip_address("blimp.local")
    # An address must be returned as given, without any lookup delay.
    started = time.monotonic()
    assert resolve_host("192.168.4.1", timeout=30) == "192.168.4.1"
    assert time.monotonic() - started < .5


def test_unresolvable_name_reports_how_to_recover():
    started = time.monotonic()
    try:
        resolve_host("blimp-that-does-not-exist.local", timeout=0)
    except RuntimeError as error:
        assert "--blimp-host" in str(error)
    else:
        raise AssertionError("expected a RuntimeError")
    # A zero timeout still costs one system lookup, which a .local name makes
    # slow, but it must not retry or start a multicast browse after that.
    assert time.monotonic() - started < 12


def _mdns_response(instance: str, host: str, address: str, port: int,
                   txt: dict[str, str]) -> bytes:
    """Build a PTR/SRV/TXT/A reply like the firmware's responder sends."""
    import struct
    service = _encode_name(SERVICE)
    full = _encode_name(f"{instance}.{SERVICE}")
    target = _encode_name(f"{host}.local")
    records = b""
    for name, rtype, data in (
        (service, 12, full),
        (full, 33, struct.pack("!HHH", 0, 0, port) + target),
        (full, 16, b"".join(bytes([len(f"{k}={v}")]) + f"{k}={v}".encode()
                            for k, v in txt.items())),
        (target, 1, socket.inet_aton(address)),
    ):
        records += name + struct.pack("!HHIH", rtype, 1, 120, len(data)) + data
    return struct.pack("!6H", 0, 0x8400, 0, 4, 0, 0) + records


def test_service_records_are_parsed_into_a_blimp():
    import struct
    packet = _mdns_response("BlimpSoftware blimp 2", "blimp2", "192.168.1.42",
                            5004, {"id": "blimp2", "video_port": "5005"})
    seen = {}
    for name, rtype, start, length in _parse_records(packet):
        seen.setdefault(rtype, []).append((name, packet[start:start + length]))
    assert sorted(seen) == [1, 12, 16, 33]
    assert seen[33][0][0] == f"BlimpSoftware blimp 2.{SERVICE}"
    assert socket.inet_ntoa(seen[1][0][1]) == "192.168.1.42"
    assert struct.unpack_from("!H", seen[33][0][1], 4)[0] == 5004


def test_compressed_names_are_followed():
    # "blimp2.local" written once, then referenced by a pointer to offset 12.
    import struct
    packet = struct.pack("!6H", 0, 0, 0, 0, 0, 0) + _encode_name("blimp2.local")
    pointer_at = len(packet)
    packet += b"\xc0\x0c"
    assert _read_name(packet, 12)[0] == "blimp2.local"
    name, after = _read_name(packet, pointer_at)
    assert name == "blimp2.local"
    assert after == pointer_at + 2


def test_two_blimps_are_distinguished_by_hostname():
    one = Blimp(host="blimp", address="192.168.1.10")
    two = Blimp(host="blimp2", address="192.168.1.11")
    assert one.hostname == "blimp.local" and two.hostname == "blimp2.local"
    assert one.address != two.address


def _hold_l2(profile, value):
    """A resting axis sample with only L2 moved to the given value."""
    axes = [0.0] * 6
    for index in profile.triggers:
        axes[index] = -1.0
    axes[profile.l2_axis] = value
    return axes


def test_l2_captures_and_l1_releases_on_each_press_edge():
    for profile in (LEGACY_PROFILE, STANDARD_PROFILE):
        state: dict = {}
        idle = _hold_l2(profile, -1.0)
        action, state = ControllerReader.gate_action(idle, [], profile, state)
        assert action is None

        # L2 pressed: capture, once, not repeated while it stays held.
        action, state = ControllerReader.gate_action(
            _hold_l2(profile, 1.0), [], profile, state)
        assert action == "gate_close"
        action, state = ControllerReader.gate_action(
            _hold_l2(profile, 1.0), [], profile, state)
        assert action is None

        # L1 pressed: release, once per press.
        action, state = ControllerReader.gate_action(
            idle, [profile.l1_button], profile, state)
        assert action == "gate_open"
        action, state = ControllerReader.gate_action(
            idle, [profile.l1_button], profile, state)
        assert action is None
        action, state = ControllerReader.gate_action(idle, [], profile, state)
        assert action is None


def test_half_pressed_trigger_does_not_chatter():
    profile = LEGACY_PROFILE
    state: dict = {}
    # Rising through the threshold fires once.
    for value in (-1.0, .2, .49):
        action, state = ControllerReader.gate_action(
            _hold_l2(profile, value), [], profile, state)
        assert action is None
    action, state = ControllerReader.gate_action(
        _hold_l2(profile, .51), [], profile, state)
    assert action == "gate_close"
    # Falling back to just under the press threshold must not re-fire, because
    # hysteresis keeps it held until it drops below the release level.
    for value in (.49, .1):
        action, state = ControllerReader.gate_action(
            _hold_l2(profile, value), [], profile, state)
        assert action is None
        assert state["l2"] is True
    action, state = ControllerReader.gate_action(
        _hold_l2(profile, -1.0), [], profile, state)
    assert action is None and state["l2"] is False


def test_gate_release_button_can_be_overridden():
    profile = STANDARD_PROFILE
    idle = _hold_l2(profile, -1.0)
    action, _ = ControllerReader.gate_action(idle, [6], profile, {}, l1_button=6)
    assert action == "gate_open"
    action, _ = ControllerReader.gate_action(idle, [6], profile, {})
    assert action is None


def test_simulator_tracks_the_gate():
    sim = _SimulatedBlimp(lambda message: None)
    assert sim._gate_open is False and sim._gate == 60.0
    sim.send({"type": "gate_open"})
    assert sim._gate_open is True and sim._gate == 120.0
    sim.send({"type": "gate_close"})
    assert sim._gate_open is False and sim._gate == 60.0
    # E-stop must not drop a captured ball.
    sim.send({"type": "estop"})
    assert sim._gate == 60.0


def test_gate_state_reaches_the_dashboard_frame():
    fleet = FleetManager("blimp")
    frame = fleet.snapshot(now=1.0)["robots"][0]
    assert frame["gate"] is None and frame["gate_open"] is False
    fleet.handle({"type": "telemetry", "gate": 118.5, "gate_open": True}, now=2.0)
    frame = fleet.snapshot(now=2.1)["robots"][0]
    assert frame["gate"] == 118.5 and frame["gate_open"] is True
