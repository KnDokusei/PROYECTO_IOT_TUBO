/*
 * stepper_math.c - ver stepper_math.h.
 *
 * Se compila tanto en el firmware como en los tests de host.
 */

#include "stepper_math.h"

#include <math.h>

float stepper_input_to_cm(float raw, stepper_input_unit_t unit)
{
    return (unit == STEPPER_INPUT_MM) ? (raw / 10.0f) : raw;
}

int32_t stepper_cm_to_steps(float cm)
{
    /* Redondeo al más cercano: truncar acumularía medio paso de sesgo en cada
     * conversión, siempre hacia el origen. */
    return (int32_t)lroundf(cm * STEPPER_STEPS_PER_CM);
}

float stepper_steps_to_cm(int32_t steps)
{
    return (float)steps / STEPPER_STEPS_PER_CM;
}

int32_t stepper_clamp_steps(int32_t steps, int32_t lo, int32_t hi)
{
    if (steps < lo) {
        return lo;
    }
    if (steps > hi) {
        return hi;
    }
    return steps;
}

bool stepper_cm_in_range(float cm)
{
    return cm >= STEPPER_NEG_LIMIT_CM && cm <= STEPPER_POS_LIMIT_CM;
}
