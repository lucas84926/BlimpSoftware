#pragma once

#include <stdbool.h>

#include "driver/i2c_master.h"

typedef struct {
    float roll;
    float pitch;
    float yaw;
    bool ok;
} blimp_attitude_t;

// Pass an existing bus when the IMU shares SDA/SCL with another device (the
// camera's SCCB lines). Pass NULL to open a private bus on the Kconfig pins.
void mpu6050_start(i2c_master_bus_handle_t shared_bus);
void mpu6050_get(blimp_attitude_t *attitude);
