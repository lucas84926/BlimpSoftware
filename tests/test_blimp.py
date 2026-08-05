import time

from basestation.fleet import FleetManager
from basestation.simulator import SimulatedBlimp
from basestation.controller_input import ControllerReader
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
