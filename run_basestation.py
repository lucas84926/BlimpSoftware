#!/usr/bin/env python3
"""Run the one-blimp Wi-Fi base station."""

import argparse
import os
import time
import uvicorn

from basestation.app import build_app
from basestation.fleet import FleetManager
from robot.comms.udp_link import UDPLink
from robot.comms.video_udp import VideoReceiver


def main():
    parser = argparse.ArgumentParser(description="BlimpSoftware base station")
    parser.add_argument("--blimp-host", default=os.getenv("BLIMP_HOST", "blimp.local"),
                        help="blimp address; an mDNS name such as blimp.local is "
                             "resolved at startup, an IP is used as given "
                             "(default: blimp.local)")
    parser.add_argument("--discover-timeout", type=float,
                        default=float(os.getenv("BLIMP_DISCOVER_TIMEOUT", "10")),
                        help="seconds to keep retrying mDNS resolution, allowing "
                             "the base station to start before the aircraft")
    parser.add_argument("--command-port", type=int, default=int(os.getenv("BLIMP_COMMAND_PORT", "5004")))
    parser.add_argument("--video-port", type=int, default=int(os.getenv("BLIMP_VIDEO_PORT", "5005")))
    parser.add_argument("--host", default=os.getenv("BLIMP_WEB_HOST", "127.0.0.1"))
    parser.add_argument("--web-port", type=int, default=int(os.getenv("BLIMP_WEB_PORT", "8000")))
    parser.add_argument("--sim", action="store_true",
                        help="simulate one blimp; no ESP32 or UDP ports required")
    parser.add_argument("--no-controller", action="store_true")
    parser.add_argument("--controller-profile",
                        default=os.getenv("BLIMP_CONTROLLER_PROFILE", "auto"),
                        choices=("auto", "legacy", "standard"),
                        help="gamepad axis layout: 'legacy' puts L2 on axis 2 and "
                             "the right stick on 3/4, 'standard' puts the right "
                             "stick on 2/3 and the triggers on 4/5 "
                             "(default: auto-detect at rest)")
    parser.add_argument("--gate-release-button", type=int,
                        default=(int(os.getenv("BLIMP_GATE_RELEASE_BUTTON"))
                                 if os.getenv("BLIMP_GATE_RELEASE_BUTTON") else None),
                        help="pygame button index for L1, which releases the "
                             "ball (default: 4 on the legacy layout, 9 on "
                             "standard; check with --controller-monitor)")
    parser.add_argument("--list-blimps", action="store_true",
                        help="list the blimps advertising themselves on this "
                             "network, then exit")
    parser.add_argument("--controller-monitor", action="store_true",
                        help="print raw gamepad axes/buttons; sends no blimp commands")
    args = parser.parse_args()

    if args.list_blimps:
        from basestation.discovery import browse
        found = browse(max(args.discover_timeout, 2))
        if not found:
            print("[base] no blimps answered. Check power, Wi-Fi, and that the "
                  "firmware has mDNS enabled.")
            return
        print(f"{'HOST':28} {'ADDRESS':16} {'CMD':>5} {'VIDEO':>5}  NAME")
        for blimp in found:
            print(f"{blimp.hostname:28} {blimp.address:16} "
                  f"{blimp.command_port:5} {blimp.video_port:5}  {blimp.name}")
        return

    if args.controller_monitor:
        from basestation.controller_input import monitor
        monitor()
        return

    blimp_host = args.blimp_host
    if not args.sim:
        from basestation.discovery import is_ip_address, resolve_host
        if not is_ip_address(blimp_host):
            print(f"[base] resolving {blimp_host} over mDNS...")
            try:
                blimp_host = resolve_host(blimp_host, args.discover_timeout)
            except RuntimeError as error:
                raise SystemExit(f"[base] {error}")
            print(f"[base] {args.blimp_host} -> {blimp_host}")

    fleet = FleetManager("blimp")
    on_message = lambda msg: fleet.handle(msg, time.monotonic())
    if args.sim:
        from basestation.simulator import SimulatedBlimp
        link = SimulatedBlimp(on_message)
    else:
        link = UDPLink(blimp_host, args.command_port, on_message,
                       bind_port=args.command_port)
    controller = None
    if not args.no_controller:
        try:
            from basestation.controller_input import ControllerReader
            controller = ControllerReader(profile=args.controller_profile,
                                          l1_button=args.gate_release_button)
        except Exception as error:
            print(f"[base] PS4 controller disabled: {error}")
    video = None if args.sim else VideoReceiver(port=args.video_port)
    app = build_app(fleet, link, controller, video)
    if args.sim:
        print("[base] simulation: one blimp (animated IMU and telemetry)")
    else:
        print(f"[base] commands -> udp://{blimp_host}:{args.command_port}")
    print(f"[base] dashboard -> http://{args.host}:{args.web_port}")
    uvicorn.run(app, host=args.host, port=args.web_port, log_level="warning")


if __name__ == "__main__":
    main()
