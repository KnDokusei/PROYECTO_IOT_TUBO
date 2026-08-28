/*
 * servo_map.c - see servo_map.h.
 */

#include "servo_map.h"

int servo_clamp_angle(int angle_deg)
{
    if (angle_deg < 0) {
        return 0;
    }
    if (angle_deg > SERVO_MAX_ANGLE) {
        return SERVO_MAX_ANGLE;
    }
    return angle_deg;
}

uint32_t servo_angle_to_pulse_us(int angle_deg)
{
    const int a = servo_clamp_angle(angle_deg);
    const uint32_t span = SERVO_MAX_PULSE_US - SERVO_MIN_PULSE_US;

    /* Rounded to nearest so 90 degrees lands on the mechanical centre. */
    return SERVO_MIN_PULSE_US +
           (uint32_t)(((uint32_t)a * span + (SERVO_MAX_ANGLE / 2)) / SERVO_MAX_ANGLE);
}

uint32_t servo_angle_to_duty(int angle_deg)
{
    const uint64_t pulse_us = servo_angle_to_pulse_us(angle_deg);
    const uint64_t full     = 1ULL << SERVO_DUTY_RES_BITS;

    /* duty = pulse_us / period_us * full, with period_us = 1e6 / freq. */
    return (uint32_t)((pulse_us * full * SERVO_FREQ_HZ + 500000ULL) / 1000000ULL);
}
