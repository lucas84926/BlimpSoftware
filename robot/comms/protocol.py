"""Minimal newline-delimited JSON codec shared by the Wi-Fi UDP link."""

import json
from typing import Optional


def encode(message: dict) -> bytes:
    return (json.dumps(message, separators=(",", ":")) + "\n").encode()


def decode(line: bytes) -> Optional[dict]:
    try:
        value = json.loads(line.strip().decode("utf-8"))
        return value if isinstance(value, dict) else None
    except (ValueError, UnicodeDecodeError):
        return None
