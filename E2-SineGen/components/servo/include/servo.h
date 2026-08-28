/*
 * servo.h - Volume servo of module E2, driven by LEDC.
 *
 * Replaces the Arduino ESP32Servo library. LEDC is what that library used
 * underneath anyway, so this is the same hardware with one less dependency.
 */
#pragma once

#include "esp_err.h"
#include "servo_map.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int gpio;         /* Signal pin. GPIO21 in the Kundt schematic. */
    int initial_angle;
} servo_config_t;

#define SERVO_DEFAULT_CONFIG()   \
    (servo_config_t)             \
    {                            \
        .gpio          = 21,     \
        .initial_angle = 0,      \
    }

/** @brief Configure the LEDC timer and channel and move to the initial angle. */
esp_err_t servo_init(const servo_config_t *cfg);

/**
 * @brief Move to an angle. Out-of-range values are clamped, not rejected.
 *
 * Writes are skipped when the angle has not changed, so a backend that repeats
 * the same setpoint every two seconds does not keep the servo humming.
 */
esp_err_t servo_set_angle(int angle_deg);

/** @brief Angle currently commanded. */
int servo_get_angle(void);

#ifdef __cplusplus
}
#endif
