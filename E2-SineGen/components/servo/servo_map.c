/*
 * servo_map.c - ver servo_map.h.
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

    /* Redondeo al más cercano para que 90 grados caiga en el centro mecánico
     * (1500 us). */
    return SERVO_MIN_PULSE_US +
           (uint32_t)(((uint32_t)a * span + (SERVO_MAX_ANGLE / 2)) / SERVO_MAX_ANGLE);
}

uint32_t servo_angle_to_duty(int angle_deg)
{
    const uint64_t pulse_us = servo_angle_to_pulse_us(angle_deg);
    const uint64_t full     = 1ULL << SERVO_DUTY_RES_BITS;

    /* duty = pulse_us / periodo_us * full, con periodo_us = 1e6 / freq. */
    return (uint32_t)((pulse_us * full * SERVO_FREQ_HZ + 500000ULL) / 1000000ULL);
}
