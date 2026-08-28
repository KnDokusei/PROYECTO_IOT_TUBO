/*
 * stepper_math.h - Geometría del riel y conversiones del módulo E3.
 *
 * Aritmética pura, sin dependencias de ESP-IDF, para poder probarla en el host.
 * El manejo del A4988, el temporizador y los fines de carrera viven en stepper.c.
 *
 * La posición se lleva SIEMPRE en pasos (entero) dentro del firmware; los
 * centímetros aparecen sólo en la frontera con la API. El sketch de Arduino
 * hacía lo contrario —convertía a cm en cada uso— y de ahí salían tanto el
 * error de resolución (A2) como la ambigüedad de unidades (A3).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Geometría del riel, del stepperConfig.h original. */
#define STEPPER_STEPS_PER_REV 200
#define STEPPER_CM_PER_REV    0.8f

/* 200 pasos / 0,8 cm = 250 pasos por cm exactos, o sea 0,004 cm por paso.
 * El README promete informar la posición con 0,1 cm: sobra un factor 25. */
#define STEPPER_STEPS_PER_CM ((float)STEPPER_STEPS_PER_REV / STEPPER_CM_PER_REV)

/* Posición de los fines de carrera, en cm desde el parlante. */
#define STEPPER_NEG_LIMIT_CM 26.0f /* el más cercano al parlante */
#define STEPPER_POS_LIMIT_CM 84.0f /* el más lejano */

/**
 * @brief Unidad en que el backend entrega "valores.embolo" (hallazgo A3).
 *
 * El sketch de Arduino dividía por 10 al leer (interpretando milímetros) pero
 * enviaba centímetros en el PUT. Como las claves JSON son distintas
 * (`valores.embolo` al leer, `posicion` al escribir) la asimetría PUEDE ser
 * intencional; sólo el código del backend lo resuelve. Si el servidor hace
 * round-trip —guarda `posicion` y la sirve como `embolo`— el émbolo colapsa
 * hacia el origen dividiéndose por 10 en cada ciclo.
 *
 * Aquí la interpretación es explícita y de un solo lugar, en vez de estar
 * escondida en un `/ 10.0f`. El valor por defecto conserva el comportamiento
 * de Arduino para no cambiar la conducta del equipo sin evidencia.
 */
typedef enum {
    STEPPER_INPUT_MM = 0, /* como Arduino: divide por 10 */
    STEPPER_INPUT_CM,     /* simétrico con el PUT: sin conversión */
} stepper_input_unit_t;

/** @brief Convierte el valor crudo de "embolo" a cm según la unidad supuesta. */
float stepper_input_to_cm(float raw, stepper_input_unit_t unit);

/** @brief Centímetros a pasos, redondeando al paso más cercano. */
int32_t stepper_cm_to_steps(float cm);

/** @brief Pasos a centímetros. */
float stepper_steps_to_cm(int32_t steps);

/** @brief Acota un valor en pasos al intervalo [lo, hi]. */
int32_t stepper_clamp_steps(int32_t steps, int32_t lo, int32_t hi);

/** @brief Límites del riel expresados en pasos. */
static inline int32_t stepper_neg_limit_steps(void)
{
    return stepper_cm_to_steps(STEPPER_NEG_LIMIT_CM);
}

static inline int32_t stepper_pos_limit_steps(void)
{
    return stepper_cm_to_steps(STEPPER_POS_LIMIT_CM);
}

/** @brief True si la posición cae dentro del recorrido útil del riel. */
bool stepper_cm_in_range(float cm);

#ifdef __cplusplus
}
#endif
