import { CameraBackground } from "./components/CameraBackground.tsx";
import { AttitudeIndicator } from "./components/AttitudeIndicator.tsx";
import { ConnectionPill } from "./components/ConnectionPill.tsx";
import { ControllerStatus } from "./components/ControllerStatus.tsx";
import { EstopBar } from "./components/EstopBar.tsx";
import { PitBoard } from "./components/PitBoard.tsx";
import { Telemetry } from "./components/Telemetry.tsx";
import { selectedRobot } from "./net/ws.ts";

function TopBar() {
  return <header class="topbar panel">
    <div class="brand"><img src="/icon.svg" alt="" /><span class="name">BlimpSoftware<small>teleop</small></span></div>
    <span class="pill quiet"><span class={`led ${selectedRobot.value?.online ? "ok" : "bad"}`} />
      {selectedRobot.value?.online ? "blimp live" : "blimp offline"}</span>
    <ConnectionPill />
  </header>;
}

export function App() {
  return <>
    <CameraBackground />
    <div class="hud">
      <TopBar />
      <aside class="rail panel"><div class="rail-body">
        <div class="rail-section pitboard-section">
          <div class="section-title"><span class="eyebrow">Calling</span><span class="tape">BLIMP</span></div>
          <PitBoard />
        </div>
        <AttitudeIndicator />
        <section class="rail-section output-section"><MotorOutput /></section>
        <section class="rail-section"><Telemetry /></section>
        <section class="rail-section"><ControllerStatus /></section>
      </div></aside>
      <div class="estop-corner"><EstopBar /></div>
    </div>
  </>;
}

function MotorOutput() {
  const b = selectedRobot.value;
  const pct = (v?: number) => `${Math.round((v ?? 0) * 100)}%`;
  return <><div class="section-title"><span class="eyebrow">Output</span><span class="eyebrow">live</span></div>
    <div class="output-grid"><span>L MOTOR <b>{pct(b?.left)}</b></span><span>R MOTOR <b>{pct(b?.right)}</b></span>
      <span>L TILT <b>{Math.round(b?.left_tilt ?? 90)}°</b></span><span>R TILT <b>{Math.round(b?.right_tilt ?? 90)}°</b></span></div></>;
}
