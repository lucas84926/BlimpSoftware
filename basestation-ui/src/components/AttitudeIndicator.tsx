import { selectedRobot } from "../net/ws.ts";

export function AttitudeIndicator() {
  const blimp = selectedRobot.value;
  const roll = blimp?.roll ?? 0;
  const pitch = blimp?.pitch ?? 0;
  return (
    <section class="rail-section">
      <div class="section-title"><span class="eyebrow">Attitude</span><span class="eyebrow">IMU</span></div>
      <div class="attitude" aria-label={`Roll ${roll} degrees, pitch ${pitch} degrees`}>
        <div class="attitude-world" style={`transform:translateY(${pitch * 1.2}px) rotate(${-roll}deg)`}>
          <div class="sky" /><div class="ground" /><div class="horizon" />
          <div class="pitch-scale">
            {[-40, -30, -20, -10, 10, 20, 30, 40].map((value) => (
              <i class={`pitch-mark${Math.abs(value) % 20 === 0 ? " major" : ""}`} style={`top:calc(50% + ${value * 2}px)`}>
                <span>{value > 0 ? `-${value}` : `+${Math.abs(value)}`}</span>
              </i>
            ))}
          </div>
        </div>
        <svg class="roll-scale" viewBox="0 0 300 46" aria-hidden="true">
          <path d="M42 29v-7 M70 23v-8 M96 17V7 M123 12V4 M150 11V1 M177 12V4 M204 17V7 M230 23v-8 M258 29v-7" />
          <g class="roll-labels"><text x="37" y="42">60</text><text x="91" y="31">30</text><text x="145" y="25">0</text><text x="199" y="31">30</text><text x="253" y="42">60</text></g>
        </svg>
        <div class="aircraft-mark"><i /><b /><i /></div>
        <span class="roll-readout">R {roll.toFixed(0)}°</span>
        <span class="pitch-readout">P {pitch.toFixed(0)}°</span>
      </div>
    </section>
  );
}
