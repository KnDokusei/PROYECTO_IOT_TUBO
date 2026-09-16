/*
 * stepper_math.h - Recta pasos <-> cm del riel de E3.
 *
 * Aritmética pura, sin ESP-IDF, para probarla en el host.
 * Coordenadas desde el parlante (0 cm = parlante):
 *
 *     pasos = m * cm          cm = pasos / m
 *
 * La pendiente m [pasos/cm] la elige main.c: calibrada o teórica.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Posición de los switches, medida con huincha desde el parlante. */
#define STEPPER_SW_DER_CM 26.0f
#define STEPPER_SW_IZQ_CM 84.0f

/* Pendiente teórica: 200 pasos/vuelta / 0,8 cm/vuelta = 250 pasos/cm. */
#define STEPPER_STEPS_PER_CM_TEORICO (200.0f / 0.8f)

/** @brief cm -> pasos, redondeando al paso más cercano. */
int32_t stepper_cm_to_steps(float cm, float m);

/** @brief pasos -> cm. */
float stepper_steps_to_cm(int32_t steps, float m);

/** @brief Acota un valor en pasos al intervalo [lo, hi]. */
int32_t stepper_clamp_steps(int32_t steps, int32_t lo, int32_t hi);

#ifdef __cplusplus
}
#endif
