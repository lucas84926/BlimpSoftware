import { selected, videoRobots } from "../net/ws.ts";

export function CameraBackground() {
  const live = videoRobots.value.includes(selected.value);
  return (
    <div class="camera-background">
      {live
        ? <img src={`/video/${selected.value}.mjpg`} alt="Blimp forward camera" />
        : <div class="camera-wait"><span>CAMERA</span>waiting for blimp video</div>}
    </div>
  );
}
