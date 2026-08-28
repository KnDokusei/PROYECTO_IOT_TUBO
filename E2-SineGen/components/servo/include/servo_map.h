/*
 * servo_map.h - Angle-to-PWM conversion for the volume servo of module E2.
 *
 * Pure integer maths, no ESP-IDF dependency, so it can be unit tested on the
 * host. The LEDC plumbing lives in servo.c.
 *
 * Context: the servo turns the potentiometer at the output of the audio
 * amplifier, which is how the remote user sets the volume. The backend sends
 * the position already expressed in degrees (finding M1), so this module only
 * has to bound it and turn it into a duty cycle.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Standard hobby servo: 20 ms frame, 0.5-2.5 ms pulse over 0-180 degrees. */
#define SERVO_FREQ_HZ       50
#define SERVO_MIN_PULSE_US  500
#define SERVO_MAX_PULSE_US  2500
#define SERVO_MAX_ANGLE     180
#define SERVO_DUTY_RES_BITS 16

/**
 * @brief Bound an angle to the servo's mechanical range.
 *
 * The Arduino build passed the backend value straight to Servo::write(), where
 * anything at or above 544 is reinterpreted as a pulse width in microseconds
 * rather than an angle -- a bad value would drive the servo into its end stop
 * (finding M1). Clamping here makes that impossible.
 */
int servo_clamp_angle(int angle_deg);

/**
 * @brief Duty cycle for an angle, at SERVO_DUTY_RES_BITS resolution.
 *
 * duty = pulse_us * 2^bits * freq / 1e6. The angle is clamped first.
 */
uint32_t servo_angle_to_duty(int angle_deg);

/** @brief Pulse width in microseconds for an angle. Clamped. */
uint32_t servo_angle_to_pulse_us(int angle_deg);

#ifdef __cplusplus
}
#endif
