"""In-process one-blimp simulator for UI and controller development."""

from __future__ import annotations

import math
import threading
import time


class SimulatedBlimp:
    def __init__(self, on_message):
        self.on_message = on_message
        self._running = False
        self._thread = None
        self._started = 0.0
        self._estop = False
        self._left = self._right = 0.0
        self._left_tilt = self._right_tilt = 90.0

    def start(self) -> None:
        self._started = time.monotonic()
        self._running = True
        self._thread = threading.Thread(target=self._loop, daemon=True, name="blimp-sim")
        self._thread.start()

    def send(self, message: dict) -> bool:
        kind = message.get("type")
        if kind == "control" and not self._estop:
            self._left = float(message.get("left", 0))
            self._right = float(message.get("right", 0))
            self._left_tilt = float(message.get("left_tilt", 90))
            self._right_tilt = float(message.get("right_tilt", 90))
        elif kind == "estop":
            self._estop = True
            self._left = self._right = 0.0
        elif kind == "clear_estop":
            self._estop = False
        elif kind == "ping":
            self.on_message({"type": "pong", "n": message.get("n")})
        return True

    def _loop(self) -> None:
        while self._running:
            t = time.monotonic() - self._started
            self.on_message({
                "type": "telemetry", "from": "blimp", "estop": self._estop,
                "left": self._left, "right": self._right,
                "left_tilt": self._left_tilt, "right_tilt": self._right_tilt,
                "roll": math.sin(t * .8) * 18,
                "pitch": math.sin(t * .45) * 10,
                "yaw": (t * 8) % 360,
                "battery": max(20, 96 - t / 120),
            })
            time.sleep(.1)

    def stop(self) -> None:
        self._running = False
        if self._thread:
            self._thread.join(timeout=1)
