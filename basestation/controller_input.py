"""Headless PS4 input for blimp thrust, steering, tilt, and E-stop."""

from __future__ import annotations

import os
import threading
import time
from dataclasses import dataclass

os.environ.setdefault("SDL_VIDEODRIVER", "dummy")
os.environ.setdefault("SDL_AUDIODRIVER", "dummy")
try:
    import pygame
except Exception:  # pragma: no cover
    pygame = None


@dataclass(frozen=True)
class AxisProfile:
    """Which pygame axis index carries each stick/trigger on one driver."""
    name: str
    left_y: int
    right_x: int
    right_y: int
    triggers: tuple[int, int]        # (L2, R2); L2 captures the ball
    l1_button: int                   # releases the ball

    @property
    def l2_axis(self) -> int:
        return self.triggers[0]


# L2 is an analog trigger, not a button, so it is read from its axis and
# debounced with hysteresis: press past 0.5, release below 0.0 (idle is -1).
TRIGGER_PRESS = .5
TRIGGER_RELEASE = 0.0


# Drivers disagree about where the triggers sit. On "legacy" they take axes 2
# and 5, pushing the right stick to 3/4; on "standard" they take 4 and 5 and
# the right stick keeps 2/3.
# The two drivers also number the shoulder buttons differently. Confirm L1
# with --controller-monitor, which prints the index of each button pressed,
# and override it with --gate-release-button if this guess is wrong.
LEGACY_PROFILE = AxisProfile("legacy", left_y=1, right_x=3, right_y=4,
                             triggers=(2, 5), l1_button=4)
STANDARD_PROFILE = AxisProfile("standard", left_y=1, right_x=2, right_y=3,
                               triggers=(4, 5), l1_button=9)
AXIS_PROFILES = {p.name: p for p in (LEGACY_PROFILE, STANDARD_PROFILE)}
DEFAULT_PROFILE = LEGACY_PROFILE


def detect_profile(axes: list[float],
                   fallback: AxisProfile | None = DEFAULT_PROFILE
                   ) -> AxisProfile | None:
    """Guess the layout from a resting sample: triggers idle at -1, sticks at 0.

    Ambiguous samples (a trigger held down at startup, an unusual pad) fall
    back rather than guessing, so an explicit profile always wins.
    """
    def at(index: int) -> float:
        return axes[index] if index < len(axes) else 0.0

    matches = [profile for profile in (LEGACY_PROFILE, STANDARD_PROFILE)
               if all(at(i) < -.9 for i in profile.triggers)
               and all(abs(at(i)) < .5
                       for i in (profile.left_y, profile.right_x, profile.right_y))]
    return matches[0] if len(matches) == 1 else fallback


class ControllerReader:
    def __init__(self, hz: float = 30, profile: str = "auto",
                 l1_button: int | None = None):
        if pygame is None:
            raise RuntimeError("pygame is not installed")
        if profile != "auto" and profile not in AXIS_PROFILES:
            raise RuntimeError(f"unknown controller profile: {profile}")
        self.hz = hz
        self.connected = False
        self.name = None
        self.requested_profile = profile
        self.profile = DEFAULT_PROFILE if profile == "auto" else AXIS_PROFILES[profile]
        # An explicit override survives profile auto-detection.
        self.l1_button = l1_button
        self.on_control = None  # left motor, right motor, left tilt, right tilt
        self.on_action = None
        self._running = False
        self._thread = None

    @staticmethod
    def _deadzone(value: float, zone: float = .08) -> float:
        return 0.0 if abs(value) < zone else max(-1.0, min(1.0, value))

    @staticmethod
    def arcade(throttle: float, steering: float) -> tuple[float, float]:
        """Mix a forward/turn request without clipping one side early."""
        scale = max(1.0, abs(throttle) + abs(steering))
        return ((throttle + steering) / scale,
                (throttle - steering) / scale)

    @classmethod
    def control_from_axes(cls, axes: list[float],
                          profile: AxisProfile = DEFAULT_PROFILE
                          ) -> tuple[float, float, float]:
        """Convert one raw pygame sample into left/right thrust and tilt."""
        axis = lambda i: axes[i] if i < len(axes) else 0.0
        throttle = cls._deadzone(-axis(profile.right_y))
        steering = cls._deadzone(axis(profile.right_x))
        left, right = cls.arcade(throttle, steering)
        tilt = round(90 - cls._deadzone(axis(profile.left_y)) * 90, 1)
        return left, right, tilt

    @classmethod
    def gate_action(cls, axes: list[float], buttons: list[int],
                    profile: AxisProfile = DEFAULT_PROFILE,
                    previous: dict | None = None,
                    l1_button: int | None = None) -> tuple[str | None, dict]:
        """Map L2/L1 to one gate command on the press edge.

        L2 captures (closes the gate) and L1 releases (opens it). Returns the
        action to send, if any, and the held state to pass in next time.
        """
        previous = previous or {}
        value = axes[profile.l2_axis] if profile.l2_axis < len(axes) else -1.0
        # Hysteresis keeps a trigger resting near the threshold from chattering.
        l2 = value > (TRIGGER_RELEASE if previous.get("l2") else TRIGGER_PRESS)
        l1 = (profile.l1_button if l1_button is None else l1_button) in buttons

        action = None
        if l2 and not previous.get("l2"):
            action = "gate_close"
        elif l1 and not previous.get("l1"):
            action = "gate_open"
        return action, {"l2": l2, "l1": l1}

    def start(self) -> None:
        self._running = True
        self._thread = threading.Thread(target=self._loop, daemon=True, name="ps4")
        self._thread.start()

    def _loop(self) -> None:
        pygame.init()
        joystick = None
        previous = {}
        gate: dict = {}
        while self._running:
            pygame.event.pump()
            if joystick is None or pygame.joystick.get_count() == 0:
                self.connected = False
                joystick = None
                if pygame.joystick.get_count() == 0:
                    time.sleep(.5)
                    continue
                joystick = pygame.joystick.Joystick(0)
                joystick.init()
                self.connected, self.name = True, joystick.get_name()
                if self.requested_profile == "auto":
                    # Sample once while the sticks are still at rest; a pad
                    # plugged in mid-flight is already being held, so a bad
                    # sample only costs us the fallback profile.
                    pygame.event.pump()
                    self.profile = detect_profile(
                        [joystick.get_axis(i) for i in range(joystick.get_numaxes())])
                print(f"[base] controller: {self.name} "
                      f"(axis profile: {self.profile.name})")
            axes = [joystick.get_axis(i) for i in range(joystick.get_numaxes())]
            # Right Y is positive downward. R2 is unused; L2 drives the gate
            # and is read below.
            left, right, tilt = self.control_from_axes(axes, self.profile)
            if self.on_control:
                self.on_control(left, right, tilt, tilt)
            for button, action in ((1, "estop"), (0, "clear_estop")):
                current = joystick.get_button(button) if button < joystick.get_numbuttons() else 0
                if current and not previous.get(button) and self.on_action:
                    self.on_action(action)
                previous[button] = current
            buttons = [i for i in range(joystick.get_numbuttons())
                       if joystick.get_button(i)]
            action, gate = self.gate_action(axes, buttons, self.profile,
                                            gate, self.l1_button)
            if action and self.on_action:
                self.on_action(action)
            time.sleep(1 / self.hz)

    def stop(self) -> None:
        self._running = False
        if self._thread:
            self._thread.join(timeout=1)


def monitor() -> None:
    """Print raw controller values without ever creating a control callback."""
    if pygame is None:
        raise SystemExit("pygame is not installed")
    pygame.init()
    pygame.joystick.init()
    if pygame.joystick.get_count() == 0:
        raise SystemExit("No controller detected")
    joystick = pygame.joystick.Joystick(0)
    joystick.init()
    print(f"Controller: {joystick.get_name()}")
    print("Move one requested control at a time. Press Ctrl-C to stop.")
    print("Press L1 to read off its button index for --gate-release-button.")
    try:
        while True:
            pygame.event.pump()
            axes = [round(joystick.get_axis(i), 3)
                    for i in range(joystick.get_numaxes())]
            buttons = [i for i in range(joystick.get_numbuttons())
                       if joystick.get_button(i)]
            detected = detect_profile(axes, fallback=None)
            label = detected.name if detected else "?"
            print(f"axes={axes} buttons={buttons} profile={label}", flush=True)
            time.sleep(.15)
    except KeyboardInterrupt:
        pass
