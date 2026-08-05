"""Thread-safe state for the single BlimpSoftware aircraft."""

from __future__ import annotations

import threading
import time


class FleetManager:
    def __init__(self, blimp_id: str = "blimp"):
        self.blimp_id = blimp_id
        self._lock = threading.Lock()
        self._last_seen = 0.0
        self._state = {
            "robot_id": blimp_id, "mode": "teleop", "estop": False,
            "left": 0.0, "right": 0.0, "left_tilt": 90.0, "right_tilt": 90.0,
            "roll": None, "pitch": None, "yaw": None, "battery": None,
            "latency_ms": None,
        }
        self._pings: dict[int, float] = {}

    @property
    def selected(self) -> str:
        return self.blimp_id

    def note_ping(self, nonce: int, sent: float) -> None:
        with self._lock:
            self._pings[nonce] = sent
            if len(self._pings) > 20:
                self._pings.pop(next(iter(self._pings)))

    def handle(self, message: dict, now: float | None = None) -> None:
        now = time.monotonic() if now is None else now
        if message.get("type") not in ("telemetry", "pong"):
            return
        with self._lock:
            self._last_seen = now
            if message.get("type") == "pong":
                nonce = message.get("n")
                sent = self._pings.pop(nonce, None)
                if sent is not None:
                    self._state["latency_ms"] = round((now - sent) * 1000)
                return
            for key in ("estop", "left", "right", "left_tilt", "right_tilt",
                        "roll", "pitch", "yaw", "battery"):
                if key in message:
                    self._state[key] = message[key]

    def snapshot(self, now: float | None = None) -> dict:
        now = time.monotonic() if now is None else now
        with self._lock:
            state = dict(self._state)
            age = None if not self._last_seen else now - self._last_seen
        state["age"] = None if age is None else round(age, 2)
        state["online"] = age is not None and age < 3.0
        return {"type": "fleet", "selected": self.blimp_id, "robots": [state]}
