#include "mpu6050.h"

#include <math.h>
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define MPU_SCL CONFIG_BLIMP_IMU_SCL_GPIO
#define MPU_SDA CONFIG_BLIMP_IMU_SDA_GPIO
#define MPU_PWR CONFIG_BLIMP_IMU_POWER_GPIO
#define RAD_TO_DEG_F 57.2957795f

static const char *TAG = "mpu6050";
static i2c_master_dev_handle_t s_dev;
static i2c_master_bus_handle_t s_bus;
static SemaphoreHandle_t s_lock;
static blimp_attitude_t s_attitude;

#ifdef CONFIG_BLIMP_IMU_INVERT_ROLL
#define INVERT_ROLL true
#else
#define INVERT_ROLL false
#endif

#ifdef CONFIG_BLIMP_IMU_INVERT_PITCH
#define INVERT_PITCH true
#else
#define INVERT_PITCH false
#endif

#ifdef CONFIG_BLIMP_IMU_ZERO_AT_STARTUP
#define ZERO_AT_STARTUP true
#else
#define ZERO_AT_STARTUP false
#endif

static esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    uint8_t data[] = {reg, value};
    return i2c_master_transmit(s_dev, data, sizeof(data), 100);
}

static esp_err_t read_regs(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, data, len, 100);
}

static int16_t be16(const uint8_t *p)
{
    return (int16_t)((p[0] << 8) | p[1]);
}

static float wrap_180(float angle)
{
    while (angle <= -180.0f) angle += 360.0f;
    while (angle > 180.0f) angle -= 360.0f;
    return angle;
}

static float fuse_angle(float predicted, float measured)
{
    // Blend the shortest angular error. This avoids a false 360-degree jump
    // when an upside-down sensor crosses the -180/+180 accelerometer boundary.
    return wrap_180(predicted + 0.02f * wrap_180(measured - predicted));
}

static void map_tilt_axes(float native_roll, float native_pitch,
                          float *roll, float *pitch)
{
#ifdef CONFIG_BLIMP_IMU_AXES_SWAPPED
    *roll = native_pitch;
    *pitch = native_roll;
#else
    *roll = native_roll;
    *pitch = native_pitch;
#endif
    if (INVERT_ROLL) *roll = -*roll;
    if (INVERT_PITCH) *pitch = -*pitch;
}

static void imu_task(void *arg)
{
    uint8_t who = 0;
    if (read_regs(0x75, &who, 1) != ESP_OK || (who != 0x68 && who != 0x70)) {
        ESP_LOGE(TAG, "MPU6050 stopped responding (WHO_AM_I=0x%02x)", who);
        vTaskDelete(NULL);
    }
    ESP_ERROR_CHECK(write_reg(0x6B, 0x01)); // wake, PLL with X gyro
    ESP_ERROR_CHECK(write_reg(0x1A, 0x03)); // ~44 Hz DLPF
    ESP_ERROR_CHECK(write_reg(0x1B, 0x00)); // gyro +/-250 dps
    ESP_ERROR_CHECK(write_reg(0x1C, 0x00)); // accel +/-2 g

    float gx_bias = 0, gy_bias = 0, gz_bias = 0;
    int samples = 0;
    for (int i = 0; i < 200; ++i) {
        uint8_t raw[14];
        if (read_regs(0x3B, raw, sizeof(raw)) == ESP_OK) {
            gx_bias += be16(raw + 8); gy_bias += be16(raw + 10); gz_bias += be16(raw + 12);
            samples++;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (samples) { gx_bias /= samples; gy_bias /= samples; gz_bias /= samples; }
    int64_t previous = esp_timer_get_time();
    float native_roll = 0.0f, native_pitch = 0.0f, yaw = 0.0f;
    float zero_roll = 0.0f, zero_pitch = 0.0f;
    bool seeded = false;
    bool reference_captured = false;
    while (true) {
        uint8_t raw[14];
        if (read_regs(0x3B, raw, sizeof(raw)) == ESP_OK) {
            int64_t now = esp_timer_get_time();
            float dt = (now - previous) / 1000000.0f;
            previous = now;
            float ax = be16(raw) / 16384.0f, ay = be16(raw + 2) / 16384.0f;
            float az = be16(raw + 4) / 16384.0f;
            float gx = (be16(raw + 8) - gx_bias) / 131.0f;
            float gy = (be16(raw + 10) - gy_bias) / 131.0f;
            float gz = (be16(raw + 12) - gz_bias) / 131.0f;
            float acc_roll = atan2f(ay, az) * RAD_TO_DEG_F;
            float acc_pitch = atan2f(-ax, sqrtf(ay * ay + az * az)) * RAD_TO_DEG_F;
            if (!seeded) {
                native_roll = acc_roll;
                native_pitch = acc_pitch;
                seeded = true;
            } else if (dt > 0 && dt < .1f) {
                native_roll = fuse_angle(native_roll + gx * dt, acc_roll);
                native_pitch = fuse_angle(native_pitch + gy * dt, acc_pitch);
                yaw += gz * dt;
                if (yaw < 0) yaw += 360;
                if (yaw >= 360) yaw -= 360;
            }

            float roll, pitch;
            map_tilt_axes(native_roll, native_pitch, &roll, &pitch);
            if (!reference_captured) {
                if (ZERO_AT_STARTUP) {
                    zero_roll = roll;
                    zero_pitch = pitch;
                    ESP_LOGI(TAG, "Startup attitude zero captured: roll=%.1f pitch=%.1f",
                             zero_roll, zero_pitch);
                }
                reference_captured = true;
            }

            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_attitude.roll = wrap_180(roll - zero_roll);
            s_attitude.pitch = wrap_180(pitch - zero_pitch);
            s_attitude.yaw = yaw;
            s_attitude.ok = true;
            xSemaphoreGive(s_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void mpu6050_start(i2c_master_bus_handle_t shared_bus)
{
    s_lock = xSemaphoreCreateMutex();

    // Power the module before assigning its pins to the I2C peripheral. An
    // unpowered module can clamp SDA/SCL and make the driver's bus clear fail.
#if CONFIG_BLIMP_IMU_POWER_GPIO >= 0
    {
        gpio_config_t power_cfg = {
            .pin_bit_mask = 1ULL << MPU_PWR,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&power_cfg));
        ESP_ERROR_CHECK(gpio_set_level(MPU_PWR, 1));
        vTaskDelay(pdMS_TO_TICKS(200));
    }
#endif

    if (shared_bus) {
        // Multi-drop: the IMU hangs off pins another controller already owns.
        // Opening a second bus on those pads steals them from the first owner.
        s_bus = shared_bus;
    } else {
        i2c_master_bus_config_t bus_cfg = {
            .i2c_port = I2C_NUM_1, .sda_io_num = MPU_SDA, .scl_io_num = MPU_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_bus));
    }

    uint8_t address = 0;
    if (i2c_master_probe(s_bus, 0x68, 100) == ESP_OK) {
        address = 0x68;
    } else if (i2c_master_probe(s_bus, 0x69, 100) == ESP_OK) {
        address = 0x69;
    }
    if (!address) {
        ESP_LOGE(TAG,
                 "No IMU ACK on GPIO SCL=%d SDA=%d; line levels SCL=%d SDA=%d. "
                 "Check wiring, common ground, and external pull-ups.",
                 MPU_SCL, MPU_SDA, gpio_get_level(MPU_SCL), gpio_get_level(MPU_SDA));
        return;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = address,
        .scl_speed_hz = 100000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev));
    ESP_LOGI(TAG, "MPU6050 detected at 0x%02x on SCL=%d SDA=%d", address, MPU_SCL, MPU_SDA);
    ESP_LOGI(TAG, "Attitude mapping: axes=%s invert roll=%d pitch=%d startup zero=%d",
#ifdef CONFIG_BLIMP_IMU_AXES_SWAPPED
             "swapped",
#else
             "native",
#endif
             INVERT_ROLL, INVERT_PITCH, ZERO_AT_STARTUP);
    xTaskCreate(imu_task, "mpu6050", 4096, NULL, 10, NULL);
}

void mpu6050_get(blimp_attitude_t *attitude)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *attitude = s_attitude;
    xSemaphoreGive(s_lock);
}
