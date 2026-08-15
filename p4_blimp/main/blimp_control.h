#pragma once

#include <stdbool.h>

typedef struct {
    float left;
    float right;
    float left_tilt;
    float right_tilt;
    float gate;          // current gate servo angle, degrees
    bool gate_open;      // commanded gate state, not the in-progress angle
    bool estop;
} blimp_output_t;

void blimp_control_init(void);
void blimp_control_set(float left, float right, float left_tilt, float right_tilt);
// Command the ball-catcher gate. The servo sweeps to the configured angle at
// the configured speed; the position survives E-stop and the failsafe so a
// captured ball is not dropped on a lost link.
void blimp_control_set_gate(bool open);
void blimp_control_estop(void);
void blimp_control_clear_estop(void);
void blimp_control_get(blimp_output_t *out);
