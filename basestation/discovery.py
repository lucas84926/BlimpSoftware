"""Find blimps on the network from their mDNS records instead of a fixed IP.

The firmware publishes ``<hostname>.local`` plus a ``_blimp._udp`` service whose
TXT record carries the aircraft id and its command/video ports. Resolution uses
the system resolver first, and falls back to a direct multicast query so hosts
without system-wide mDNS (Linux without avahi/nss-mdns) still work.
"""

from __future__ import annotations

import ipaddress
import socket
import struct
import time
from dataclasses import dataclass, field

SERVICE = "_blimp._udp.local"
MDNS_GROUP = "224.0.0.251"
MDNS_PORT = 5353
TYPE_A, TYPE_PTR, TYPE_TXT, TYPE_SRV = 1, 12, 16, 33
# Ask for a unicast reply so the query works without binding port 5353, which
# a system responder such as avahi already holds.
QU_CLASS_IN = 0x8001


@dataclass
class Blimp:
    """One aircraft seen on the network."""
    name: str = ""            # service instance label, e.g. "BlimpSoftware blimp"
    host: str = ""            # mDNS hostname without .local, e.g. "blimp2"
    address: str = ""
    command_port: int = 0
    video_port: int = 0
    properties: dict[str, str] = field(default_factory=dict)

    @property
    def hostname(self) -> str:
        return f"{self.host}.local" if self.host else ""


def is_ip_address(host: str) -> bool:
    try:
        ipaddress.ip_address(host)
        return True
    except ValueError:
        return False


def _resolve_via_os(host: str) -> str | None:
    """Ask the system resolver. Works natively on macOS and Windows, and on
    Linux with avahi-daemon plus nss-mdns."""
    try:
        info = socket.getaddrinfo(host, None, socket.AF_INET, socket.SOCK_DGRAM)
    except socket.gaierror:
        return None
    return info[0][4][0] if info else None


def _encode_name(name: str) -> bytes:
    return b"".join(bytes([len(p)]) + p.encode()
                    for p in name.rstrip(".").split(".")) + b"\0"


def _read_name(packet: bytes, offset: int) -> tuple[str, int]:
    """Decode a DNS name, following compression pointers."""
    labels: list[str] = []
    end = None
    seen = 0
    while offset < len(packet):
        length = packet[offset]
        if length == 0:
            offset += 1
            break
        if length & 0xC0 == 0xC0:                      # compression pointer
            pointer = struct.unpack_from("!H", packet, offset)[0] & 0x3FFF
            if end is None:
                end = offset + 2
            offset = pointer
            seen += 1
            if seen > 64:                              # malformed/looping packet
                break
            continue
        offset += 1
        labels.append(packet[offset:offset + length].decode(errors="replace"))
        offset += length
    return ".".join(labels), (end if end is not None else offset)


def _parse_records(packet: bytes):
    """Yield (name, rtype, rdata_offset, rdata_length) for every record."""
    try:
        counts = struct.unpack_from("!6H", packet, 0)
    except struct.error:
        return
    # counts is (id, flags, questions, answers, authority, additional).
    offset = 12
    for _ in range(counts[2]):                         # skip the questions
        _, offset = _read_name(packet, offset)
        offset += 4
    for _ in range(sum(counts[3:])):                   # answers + auth + extra
        name, offset = _read_name(packet, offset)
        if offset + 10 > len(packet):
            return
        rtype, _cls, _ttl, length = struct.unpack_from("!HHIH", packet, offset)
        offset += 10
        yield name, rtype, offset, length
        offset += length


def browse(timeout: float = 2.0) -> list[Blimp]:
    """Multicast a PTR query for the service and collect the replies.

    Returns one entry per aircraft that answers within the timeout.
    """
    query = struct.pack("!6H", 0, 0, 1, 0, 0, 0)
    query += _encode_name(SERVICE) + struct.pack("!HH", TYPE_PTR, QU_CLASS_IN)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
    sock.settimeout(.3)

    # Keyed by service instance, which is what identifies one aircraft: SRV and
    # TXT share that name, while A records are keyed by the SRV target instead.
    # The records may also arrive in separate packets, one set per interface.
    blimps: dict[str, Blimp] = {}
    addresses: dict[str, str] = {}
    try:
        sock.sendto(query, (MDNS_GROUP, MDNS_PORT))
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                packet, _ = sock.recvfrom(9000)
            except socket.timeout:
                continue
            except OSError:
                break
            for name, rtype, start, length in _parse_records(packet):
                data = packet[start:start + length]
                if rtype == TYPE_A and length == 4:
                    addresses[name.removesuffix(".local")] = socket.inet_ntoa(data)
                elif rtype == TYPE_SRV and name.endswith(SERVICE):
                    port = struct.unpack_from("!H", packet, start + 4)[0]
                    target, _ = _read_name(packet, start + 6)
                    blimp = blimps.setdefault(name, Blimp())
                    blimp.name = name.removesuffix("." + SERVICE)
                    blimp.host = target.removesuffix(".local")
                    blimp.command_port = port
                elif rtype == TYPE_TXT and name.endswith(SERVICE):
                    properties = {}
                    index = 0
                    while index < len(data):
                        size = data[index]
                        item = data[index + 1:index + 1 + size].decode(errors="replace")
                        key, _, value = item.partition("=")
                        if key:
                            properties[key] = value
                        index += 1 + size
                    blimps.setdefault(name, Blimp()).properties = properties
    finally:
        sock.close()

    found = []
    for blimp in blimps.values():
        if not blimp.host:                             # TXT without a SRV yet
            continue
        blimp.address = addresses.get(blimp.host, "")
        try:
            blimp.video_port = int(blimp.properties.get("video_port", 0))
        except ValueError:
            blimp.video_port = 0
        found.append(blimp)
    return sorted(found, key=lambda b: (b.host, b.name))


def _resolve_via_browse(host: str, timeout: float) -> str | None:
    """Match a name against the service records, so a blimp is reachable even
    where .local names do not resolve through the system."""
    wanted = host.removesuffix(".local").removesuffix(".")
    for blimp in browse(timeout):
        if wanted in (blimp.host, blimp.properties.get("id", "")) and blimp.address:
            return blimp.address
    return None


def resolve_host(host: str, timeout: float = 10.0) -> str:
    """Turn a name such as ``blimp.local`` into an address.

    IP literals are returned untouched. Resolution is retried until the timeout
    expires, so the base station may be started while the aircraft is still
    booting or joining Wi-Fi.
    """
    if is_ip_address(host):
        return host

    deadline = time.monotonic() + max(timeout, 0)
    attempts = 0
    while True:
        address = _resolve_via_os(host)
        if address:
            return address
        # The browse costs a round of multicast traffic, so only fall back to it
        # once the cheap system lookup has had a chance.
        if attempts:
            address = _resolve_via_browse(host, min(2.0, max(timeout, 1.0)))
            if address:
                return address
        attempts += 1
        if time.monotonic() >= deadline:
            raise RuntimeError(
                f"could not resolve {host!r} over mDNS. Check that the blimp is "
                "powered and on this network and that mDNS is enabled in its "
                "firmware. Run --list-blimps to see the aircraft that answer, "
                "or pass --blimp-host with a literal IP to skip discovery.")
        time.sleep(1)
