/*
 * stepper_math.c - ver stepper_math.h.
 *
 * Se compila tanto en el firmware como en los tests de host.
 */

#include "stepper_math.h"

#include <math.h>

int32_t stepper_cm_to_steps(float cm, float m)
{
    /* Al más cercano: truncar sesgaría cada conversión medio paso hacia el parlante. */
    return (int32_t)lroundf(m * cm);
}

float stepper_steps_to_cm(int32_t steps, float m)
{
    return (float)steps / m;
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
