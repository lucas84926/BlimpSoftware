import { selectedRobot } from "../net/ws.ts";

function Row({ label, children }: { label: string; children: preact.ComponentChildren }) {
  return <div class="tel-row"><span class="k">{label}</span><span class="v">{children}</span></div>;
}

export function Telemetry() {
  const b = selectedRobot.value;
  if (!b) return <p class="hint">Waiting for telemetry…</p>;
  return <div class="telemetry">
    <Row label="mode">{b.estop ? <b style="color:var(--danger)">E-STOP</b> : "teleop"}</Row>
    <Row label="battery">{b.battery == null ? "—" : `${b.battery.toFixed(1)}%`}</Row>
    <Row label="yaw">{b.yaw == null ? "—" : `${b.yaw.toFixed(1)}°`}</Row>
    <Row label="link"><span style={`color:${b.online ? "var(--ok)" : "var(--danger)"}`}>{b.online ? "online" : "offline"}</span></Row>
  </div>;
}
