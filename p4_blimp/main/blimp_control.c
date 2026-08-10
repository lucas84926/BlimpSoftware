#include "blimp_control.h"

#include <math.h>
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define PWM_HZ 50
#define PWM_BITS LEDC_TIMER_14_BIT
#define PWM_MAX ((1U << 14) - 1)
#define COMMAND_TIMEOUT_US 400000

#ifdef CONFIG_BLIMP_LEFT_MOTOR_INVERTED
#define LEFT_MOTOR_INVERTED true
#else
#define LEFT_MOTOR_INVERTED false
#endif
#ifdef CONFIG_BLIMP_RIGHT_MOTOR_INVERTED
#define RIGHT_MOTOR_INVERTED true
#else
#define RIGHT_MOTOR_INVERTED false
#endif
#ifdef CONFIG_BLIMP_LEFT_SERVO_INVERTED
#define LEFT_SERVO_INVERTED true
#else
#define LEFT_SERVO_INVERTED false
#endif
#ifdef CONFIG_BLIMP_RIGHT_SERVO_INVERTED
#define RIGHT_SERVO_INVERTED true
#else
#define RIGHT_SERVO_INVERTED false
#endif

static const char *TAG = "blimp_control";
static SemaphoreHandle_t s_lock;
static blimp_output_t s_output = {.left_tilt = 90, .right_tilt = 90};
static int64_t s_last_control_us;

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static uint32_t pulse_to_duty(float pulse_us)
{
    return (uint32_t)(pulse_us * PWM_HZ * (PWM_MAX + 1) / 1000000.0f);
}

static void set_pulse(ledc_channel_t channel, float pulse_us)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, pulse_to_duty(pulse_us));
    ledc_update_duty(LEDC_LOW_SPEED_MODE, channel);
}

static float motor_pulse(float command, bool inverted, float neutral_us,
                         float forward_scale, float reverse_scale)
{
    command = clampf(command, -1.0f, 1.0f);
    // Direction scales are defined in logical blimp coordinates, before a
    // motor's electrical/propeller reversal is applied.
    command *= command >= 0.0f ? forward_scale : reverse_scale;
    if (inverted) {
        command = -command;
    }

    // Interpolate from the calibrated stop pulse toward the appropriate ESC
    // endpoint. This retains the full 1000..2000 us bounds when neutral is
    // trimmed away from 1500 us.
    if (command >= 0.0f) {
        return neutral_us + command * (2000.0f - neutral_us);
    }
    return neutral_us + command * (neutral_us - 1000.0f);
}

static float servo_pulse(float angle, bool inverted, float neutral_angle)
{
    angle = clampf(angle, 0.0f, 180.0f);
    if (inverted) {
        angle = 180.0f - angle;
    }
    neutral_angle = clampf(neutral_angle, 1.0f, 179.0f);

    // Calibrate the physical center without losing either endpoint. Logical
    // 90 degrees maps to the configured neutral even for an inverted servo.
    float calibrated_angle;
    if (angle <= 90.0f) {
        calibrated_angle = angle * neutral_angle / 90.0f;
    } else {
        calibrated_angle = neutral_angle +
            (angle - 90.0f) * (180.0f - neutral_angle) / 90.0f;
    }
    return 500.0f + calibrated_angle * (2000.0f / 180.0f);
}

static void apply_locked(void)
{
    float left = s_output.estop ? 0 : s_output.left;
    float right = s_output.estop ? 0 : s_output.right;

    set_pulse(LEDC_CHANNEL_1, motor_pulse(
        left, LEFT_MOTOR_INVERTED, CONFIG_BLIMP_LEFT_ESC_NEUTRAL_US,
        CONFIG_BLIMP_LEFT_MOTOR_FORWARD_SCALE_PERCENT / 100.0f,
        CONFIG_BLIMP_LEFT_MOTOR_REVERSE_SCALE_PERCENT / 100.0f));
    set_pulse(LEDC_CHANNEL_2, motor_pulse(
        right, RIGHT_MOTOR_INVERTED, CONFIG_BLIMP_RIGHT_ESC_NEUTRAL_US,
        CONFIG_BLIMP_RIGHT_MOTOR_FORWARD_SCALE_PERCENT / 100.0f,
        CONFIG_BLIMP_RIGHT_MOTOR_REVERSE_SCALE_PERCENT / 100.0f));
    set_pulse(LEDC_CHANNEL_3, servo_pulse(
        s_output.left_tilt, LEFT_SERVO_INVERTED,
        CONFIG_BLIMP_LEFT_SERVO_NEUTRAL_ANGLE));
    set_pulse(LEDC_CHANNEL_4, servo_pulse(
        s_output.right_tilt, RIGHT_SERVO_INVERTED,
        CONFIG_BLIMP_RIGHT_SERVO_NEUTRAL_ANGLE));
}

static void watchdog_task(void *arg)
{
    while (true) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (!s_output.estop && s_last_control_us &&
            esp_timer_get_time() - s_last_control_us > COMMAND_TIMEOUT_US) {
            s_output.left = s_output.right = 0;
            s_last_control_us = 0;
            apply_locked();
        }
        xSemaphoreGive(s_lock);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void blimp_control_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = PWM_BITS,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = PWM_HZ,
        // ESP32-P4 LEDC timers share one global source. XCLK also uses LEDC and
        // needs the 80 MHz PLL, so explicitly use that source for both timers.
        .clk_cfg = LEDC_USE_PLL_DIV_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    const int pins[] = {CONFIG_BLIMP_LEFT_ESC_GPIO, CONFIG_BLIMP_RIGHT_ESC_GPIO,
                        CONFIG_BLIMP_LEFT_TILT_GPIO, CONFIG_BLIMP_RIGHT_TILT_GPIO};
    const float initial_pulses[] = {CONFIG_BLIMP_LEFT_ESC_NEUTRAL_US,
                                    CONFIG_BLIMP_RIGHT_ESC_NEUTRAL_US,
                                    servo_pulse(90.0f, LEFT_SERVO_INVERTED,
                                        CONFIG_BLIMP_LEFT_SERVO_NEUTRAL_ANGLE),
                                    servo_pulse(90.0f, RIGHT_SERVO_INVERTED,
                                        CONFIG_BLIMP_RIGHT_SERVO_NEUTRAL_ANGLE)};
    for (int i = 0; i < 4; ++i) {
        ledc_channel_config_t channel = {
            .gpio_num = pins[i], .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = (ledc_channel_t)(LEDC_CHANNEL_1 + i),
            .intr_type = LEDC_INTR_DISABLE, .timer_sel = LEDC_TIMER_1,
            .duty = pulse_to_duty(initial_pulses[i]), .hpoint = 0,
        };
        ESP_ERROR_CHECK(ledc_channel_config(&channel));
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    apply_locked();
    xSemaphoreGive(s_lock);
    xTaskCreate(watchdog_task, "control_watchdog", 3072, NULL, 12, NULL);
    ESP_LOGI(TAG, "ESC GPIOs %d/%d, tilt GPIOs %d/%d",
             pins[0], pins[1], pins[2], pins[3]);
    ESP_LOGI(TAG, "Reversed: motors L=%d R=%d, servos L=%d R=%d",
             LEFT_MOTOR_INVERTED, RIGHT_MOTOR_INVERTED,
             LEFT_SERVO_INVERTED, RIGHT_SERVO_INVERTED);
    ESP_LOGI(TAG, "Servo neutral angles: L=%d R=%d degrees",
             CONFIG_BLIMP_LEFT_SERVO_NEUTRAL_ANGLE,
             CONFIG_BLIMP_RIGHT_SERVO_NEUTRAL_ANGLE);
    ESP_LOGI(TAG, "ESC neutral us: L=%d R=%d; logical scales: "
             "L fwd/rev=%d/%d%% R fwd/rev=%d/%d%%",
             CONFIG_BLIMP_LEFT_ESC_NEUTRAL_US,
             CONFIG_BLIMP_RIGHT_ESC_NEUTRAL_US,
             CONFIG_BLIMP_LEFT_MOTOR_FORWARD_SCALE_PERCENT,
             CONFIG_BLIMP_LEFT_MOTOR_REVERSE_SCALE_PERCENT,
             CONFIG_BLIMP_RIGHT_MOTOR_FORWARD_SCALE_PERCENT,
             CONFIG_BLIMP_RIGHT_MOTOR_REVERSE_SCALE_PERCENT);
    ESP_LOGI(TAG, "Steering sensitivity: %d%%",
             CONFIG_BLIMP_STEERING_SENSITIVITY_PERCENT);
}

void blimp_control_set(float left, float right, float left_tilt, float right_tilt)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_output.estop) {
        left = clampf(left, -1, 1);
        right = clampf(right, -1, 1);

        // Recover the arcade-drive components from the basestation's mixed
        // motor commands, scale only steering, then mix them again. This keeps
        // straight-line thrust unchanged and avoids weakening ascent/descent.
        const float throttle = (left + right) * 0.5f;
        const float steering = (left - right) * 0.5f;
        const float steering_scale =
            CONFIG_BLIMP_STEERING_SENSITIVITY_PERCENT / 100.0f;
        s_output.left = clampf(throttle + steering * steering_scale, -1, 1);
        s_output.right = clampf(throttle - steering * steering_scale, -1, 1);
        s_output.left_tilt = clampf(left_tilt, 0, 180);
        s_output.right_tilt = clampf(right_tilt, 0, 180);
        s_last_control_us = esp_timer_get_time();
        apply_locked();
    }
    xSemaphoreGive(s_lock);
}

void blimp_control_estop(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_output.estop = true;
    s_output.left = s_output.right = 0;
    s_last_control_us = 0;
    apply_locked();
    xSemaphoreGive(s_lock);
}

void blimp_control_clear_estop(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_output.estop = false;
    s_output.left = s_output.right = 0;
    s_last_control_us = 0;
    apply_locked();
    xSemaphoreGive(s_lock);
}

void blimp_control_get(blimp_output_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_output;
    xSemaphoreGive(s_lock);
}
