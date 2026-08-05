#pragma once

#include <stdbool.h>

typedef struct {
    float left;
    float right;
    float left_tilt;
    float right_tilt;
    bool estop;
} blimp_output_t;

void blimp_control_init(void);
void blimp_control_set(float left, float right, float left_tilt, float right_tilt);
void blimp_control_estop(void);
void blimp_control_clear_estop(void);
void blimp_control_get(blimp_output_t *out);
