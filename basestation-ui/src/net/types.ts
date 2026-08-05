export interface Blimp {
  robot_id: string;
  mode: "teleop";
  online: boolean;
  age: number | null;
  estop: boolean;
  left: number;
  right: number;
  left_tilt: number;
  right_tilt: number;
  roll: number | null;
  pitch: number | null;
  yaw: number | null;
  battery: number | null;
  latency_ms: number | null;
}

export interface FleetMessage {
  type: "fleet";
  selected: string;
  robots: Blimp[];
  controller: { connected: boolean; name: string | null };
  video: string[];
}

export type Action =
  | { action: "estop"; robot_id: string }
  | { action: "clear_estop"; robot_id: string };
