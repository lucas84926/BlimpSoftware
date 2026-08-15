"""Minimal browser-to-ESP32 bridge for one blimp."""

from __future__ import annotations

import asyncio
import re
import time
from pathlib import Path
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse, JSONResponse, StreamingResponse
from fastapi.staticfiles import StaticFiles


def build_app(fleet, link, controller=None, video_rx=None, ui_hz: float = 20):
    app = FastAPI(title="BlimpSoftware Base Station")
    clients: set[WebSocket] = set()

    def send(message: dict) -> None:
        link.send(message)

    if controller is not None:
        controller.on_control = lambda l, r, lt, rt: send({
            "type": "control", "left": round(l, 3), "right": round(r, 3),
            "left_tilt": lt, "right_tilt": rt,
        })
        controller.on_action = lambda action: send({"type": action})

    async def broadcast() -> None:
        nonce = 0
        last_ping = 0.0
        while True:
            now = time.monotonic()
            if now - last_ping >= 1:
                nonce += 1
                fleet.note_ping(nonce, now)
                send({"type": "ping", "n": nonce})
                last_ping = now
            frame = fleet.snapshot(now)
            frame["controller"] = {
                "connected": bool(getattr(controller, "connected", False)),
                "name": getattr(controller, "name", None),
            }
            frame["video"] = video_rx.robots() if video_rx else []
            for client in list(clients):
                try:
                    await client.send_json(frame)
                except Exception:
                    clients.discard(client)
            await asyncio.sleep(1 / max(ui_hz, 1))

    @app.on_event("startup")
    async def startup():
        link.start()
        if controller: controller.start()
        if video_rx: video_rx.start()
        app.state.broadcast = asyncio.create_task(broadcast())

    @app.on_event("shutdown")
    async def shutdown():
        app.state.broadcast.cancel()
        if controller: controller.stop()
        if video_rx: video_rx.stop()
        link.stop()

    @app.websocket("/ws")
    async def websocket(ws: WebSocket):
        await ws.accept()
        clients.add(ws)
        try:
            while True:
                data = await ws.receive_json()
                action = data.get("action")
                if action in ("estop", "clear_estop",
                              "gate_open", "gate_close"):
                    send({"type": action})
        except (WebSocketDisconnect, RuntimeError):
            pass
        finally:
            clients.discard(ws)

    @app.get("/video/{blimp_id}.mjpg")
    async def video(blimp_id: str):
        async def frames():
            previous = None
            while True:
                jpeg = video_rx.latest(blimp_id) if video_rx else None
                if jpeg and jpeg is not previous:
                    previous = jpeg
                    yield b"--frame\r\nContent-Type: image/jpeg\r\n\r\n" + jpeg + b"\r\n"
                await asyncio.sleep(.04)
        return StreamingResponse(frames(), media_type="multipart/x-mixed-replace; boundary=frame")

    # Serve the built dashboard directly, making run_basestation.py sufficient
    # for normal use. Deno remains available as an optional kiosk/LAN frontend.
    ui_dist = Path(__file__).resolve().parent.parent / "basestation-ui" / "dist"
    ui_index = ui_dist / "index.html"
    missing_ui_files: list[str] = []
    if ui_index.is_file():
        html = ui_index.read_text(encoding="utf-8")
        references = re.findall(r'(?:src|href)="(/[^"?#]+)', html)
        missing_ui_files = [
            path for path in references
            if not (ui_dist / path.lstrip("/")).is_file()
        ]

    if ui_index.is_file() and not missing_ui_files:
        # Vite filenames include a content hash and old bundles are removed on
        # rebuild. Never let a browser cache index.html, or it may request asset
        # hashes that no longer exist and render a blank page after an update.
        @app.get("/", include_in_schema=False)
        async def dashboard():
            return FileResponse(
                ui_index,
                headers={"Cache-Control": "no-store, max-age=0"},
            )

        app.mount("/", StaticFiles(directory=ui_dist, html=True), name="dashboard")
    else:
        if missing_ui_files:
            print("[base] dashboard build is incomplete; missing: " +
                  ", ".join(missing_ui_files))

        @app.get("/")
        async def missing_ui():
            return JSONResponse({
                "error": "Dashboard has not been built or is incomplete",
                "missing": missing_ui_files,
                "fix": "cd basestation-ui && npm install && npm run build",
            }, status_code=503)

    return app
