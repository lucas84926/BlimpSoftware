import { selected, selectedRobot, send } from "../net/ws.ts";

// Compact safety control stays at the top-right over the camera stream.
export function EstopBar() {
  const rid = selected.value;
  const robot = selectedRobot.value;
  const armed = robot?.estop ?? false;

  return (
    <div class="estop-dock">
      {armed && (
        <button
          class="btn estop-clear"
          disabled={!rid}
          onClick={() => rid && send({ action: "clear_estop", robot_id: rid })}
        >
          Clear
        </button>
      )}
      <button
        class={`estop${armed ? " armed" : ""}`}
        disabled={!rid}
        onClick={() => rid && send({ action: "estop", robot_id: rid })}
      >
        E‑STOP
      </button>
    </div>
  );
}
