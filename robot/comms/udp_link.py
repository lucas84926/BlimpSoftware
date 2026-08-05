"""Minimal newline-delimited JSON link for one Wi-Fi blimp."""

from __future__ import annotations

import socket
import threading
from typing import Callable, Optional

from .protocol import decode, encode


class UDPLink:
    def __init__(self, host: str, port: int, on_message: Callable[[dict], None],
                 bind_host: str = "0.0.0.0", bind_port: int = 0):
        self.address = (host, port)
        self.bind = (bind_host, bind_port)
        self.on_message = on_message
        self._sock: Optional[socket.socket] = None
        self._thread: Optional[threading.Thread] = None
        self._running = False

    def start(self) -> None:
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.bind(self.bind)
        self._sock.settimeout(0.5)
        self._running = True
        self._thread = threading.Thread(target=self._read, name="blimp-udp", daemon=True)
        self._thread.start()

    def send(self, message: dict) -> bool:
        if self._sock is None:
            return False
        try:
            self._sock.sendto(encode(message), self.address)
            return True
        except OSError:
            return False

    def _read(self) -> None:
        while self._running and self._sock is not None:
            try:
                packet, _ = self._sock.recvfrom(8192)
            except socket.timeout:
                continue
            except OSError:
                break
            for line in packet.splitlines():
                message = decode(line)
                if message is not None:
                    self.on_message(message)

    def stop(self) -> None:
        self._running = False
        if self._sock is not None:
            self._sock.close()
        if self._thread is not None:
            self._thread.join(timeout=1)
