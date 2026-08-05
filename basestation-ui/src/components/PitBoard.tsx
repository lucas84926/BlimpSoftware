import { selectedRobot } from "../net/ws.ts";

function Cell({ label, value, unit }: { label: string; value: string; unit?: string }) {
  return <div class="pb-cell"><span class="pb-key">{label}<span class="pb-unit">{unit}</span></span><span class="pb-val">{value}</span></div>;
}

export function PitBoard() {
  const b = selectedRobot.value;
  return <div class="pitboard">
    <Cell label="thr" value={String(Math.round((((b?.left ?? 0) + (b?.right ?? 0)) / 2) * 100))} unit="%" />
    <Cell label="yaw" value={b?.yaw == null ? "—" : String(Math.round(b.yaw))} unit="°" />
    <Cell label="ping" value={b?.latency_ms == null ? "—" : String(b.latency_ms)} unit="ms" />
  </div>;
}
