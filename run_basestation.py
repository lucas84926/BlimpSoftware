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
    parser.add_argument("--blimp-host", default=os.getenv("BLIMP_HOST", "192.168.4.1"))
    parser.add_argument("--command-port", type=int, default=int(os.getenv("BLIMP_COMMAND_PORT", "5004")))
    parser.add_argument("--video-port", type=int, default=int(os.getenv("BLIMP_VIDEO_PORT", "5005")))
    parser.add_argument("--host", default=os.getenv("BLIMP_WEB_HOST", "127.0.0.1"))
    parser.add_argument("--web-port", type=int, default=int(os.getenv("BLIMP_WEB_PORT", "8000")))
    parser.add_argument("--sim", action="store_true",
                        help="simulate one blimp; no ESP32 or UDP ports required")
    parser.add_argument("--no-controller", action="store_true")
    parser.add_argument("--controller-monitor", action="store_true",
                        help="print raw gamepad axes/buttons; sends no blimp commands")
    args = parser.parse_args()

    if args.controller_monitor:
        from basestation.controller_input import monitor
        monitor()
        return

    fleet = FleetManager("blimp")
    on_message = lambda msg: fleet.handle(msg, time.monotonic())
    if args.sim:
        from basestation.simulator import SimulatedBlimp
        link = SimulatedBlimp(on_message)
    else:
        link = UDPLink(args.blimp_host, args.command_port, on_message,
                       bind_port=args.command_port)
    controller = None
    if not args.no_controller:
        try:
            from basestation.controller_input import ControllerReader
            controller = ControllerReader()
        except Exception as error:
            print(f"[base] PS4 controller disabled: {error}")
    video = None if args.sim else VideoReceiver(port=args.video_port)
    app = build_app(fleet, link, controller, video)
    if args.sim:
        print("[base] simulation: one blimp (animated IMU and telemetry)")
    else:
        print(f"[base] commands -> udp://{args.blimp_host}:{args.command_port}")
    print(f"[base] dashboard -> http://{args.host}:{args.web_port}")
    uvicorn.run(app, host=args.host, port=args.web_port, log_level="warning")


if __name__ == "__main__":
    main()
