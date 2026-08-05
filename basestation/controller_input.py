"""Headless PS4 input for blimp thrust, steering, tilt, and E-stop."""

from __future__ import annotations

import os
import threading
import time

os.environ.setdefault("SDL_VIDEODRIVER", "dummy")
os.environ.setdefault("SDL_AUDIODRIVER", "dummy")
try:
    import pygame
except Exception:  # pragma: no cover
    pygame = None

# Observed pygame mapping for the project's PS4 controller/driver.
AXIS_LEFT_Y = 1
AXIS_L2 = 2
AXIS_RIGHT_X = 3
AXIS_RIGHT_Y = 4
AXIS_R2 = 5


class ControllerReader:
    def __init__(self, hz: float = 30):
        if pygame is None:
            raise RuntimeError("pygame is not installed")
        self.hz = hz
        self.connected = False
        self.name = None
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
    def control_from_axes(cls, axes: list[float]) -> tuple[float, float, float]:
        """Convert one raw pygame sample into left/right thrust and tilt."""
        axis = lambda i: axes[i] if i < len(axes) else 0.0
        throttle = cls._deadzone(-axis(AXIS_RIGHT_Y))
        steering = cls._deadzone(axis(AXIS_RIGHT_X))
        left, right = cls.arcade(throttle, steering)
        tilt = round(90 - cls._deadzone(axis(AXIS_LEFT_Y)) * 90, 1)
        return left, right, tilt

    def start(self) -> None:
        self._running = True
        self._thread = threading.Thread(target=self._loop, daemon=True, name="ps4")
        self._thread.start()

    def _loop(self) -> None:
        pygame.init()
        joystick = None
        previous = {}
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
            axes = [joystick.get_axis(i) for i in range(joystick.get_numaxes())]
            # Right Y is positive downward. Axes 2 and 5 are L2/R2 and are
            # deliberately ignored; both rest at -1 on this driver.
            left, right, tilt = self.control_from_axes(axes)
            if self.on_control:
                self.on_control(left, right, tilt, tilt)
            for button, action in ((1, "estop"), (0, "clear_estop")):
                current = joystick.get_button(button) if button < joystick.get_numbuttons() else 0
                if current and not previous.get(button) and self.on_action:
                    self.on_action(action)
                previous[button] = current
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
    try:
        while True:
            pygame.event.pump()
            axes = [round(joystick.get_axis(i), 3)
                    for i in range(joystick.get_numaxes())]
            buttons = [i for i in range(joystick.get_numbuttons())
                       if joystick.get_button(i)]
            print(f"axes={axes} buttons={buttons}", flush=True)
            time.sleep(.15)
    except KeyboardInterrupt:
        pass
