import { computed, signal } from "@preact/signals";
import type { Action, Blimp, FleetMessage } from "./types.ts";

export const conn = signal<"connecting" | "live" | "reconnecting">("connecting");
export const robots = signal<Blimp[]>([]);
export const selected = signal("blimp");
export const controller = signal({ connected: false, name: null as string | null });
export const videoRobots = signal<string[]>([]);
export const selectedRobot = computed(() => robots.value[0] ?? null);
let socket: WebSocket | null = null;

export function connect(): void {
  const protocol = location.protocol === "https:" ? "wss" : "ws";
  socket = new WebSocket(`${protocol}://${location.host}/ws`);
  socket.onopen = () => conn.value = "live";
  socket.onclose = () => {
    conn.value = "reconnecting";
    setTimeout(connect, 1000);
  };
  socket.onmessage = event => {
    let message: FleetMessage;
    try { message = JSON.parse(event.data); } catch { return; }
    if (message.type !== "fleet") return;
    robots.value = message.robots;
    controller.value = message.controller;
    videoRobots.value = message.video ?? [];
  };
}

export function send(action: Action): void {
  if (socket?.readyState === WebSocket.OPEN) socket.send(JSON.stringify(action));
}
