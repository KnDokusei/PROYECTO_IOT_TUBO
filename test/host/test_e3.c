/*
 * Tests de host de la recta pasos <-> cm del módulo E3 (motor paso a paso).
 *
 * Compilar y ejecutar:  make -C test/host e3
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "stepper_math.h"

static int g_checks, g_failures;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        g_checks++;                                         \
        if (!(cond)) {                                      \
            g_failures++;                                   \
            printf("    FALLA %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                            \
            printf("\n");                                   \
        }                                                   \
    } while (0)

#define CLOSE(a, b, tol) (fabsf((a) - (b)) <= (tol))

/* Pasos entre switches medidos en el riel real (barrido del 2026-09-14). */
#define SPAN_MEDIDO 14405

/* La misma fórmula que pendiente() calibrada en main.c. */
#define M_CALIBRADA ((float)SPAN_MEDIDO / (STEPPER_SW_IZQ_CM - STEPPER_SW_DER_CM))

static void test_teorica(void)
{
    printf("  pendiente teórica: 250 pasos/cm, switches en 6500 y 21000 pasos\n");

    const float m = STEPPER_STEPS_PER_CM_TEORICO;
    CHECK(CLOSE(m, 250.0f, 0.001f), "m = %f", (double)m);
    CHECK(stepper_cm_to_steps(0.0f, m) == 0, "el parlante no es el paso 0");
    CHECK(stepper_cm_to_steps(STEPPER_SW_DER_CM, m) == 6500,
          "SW der = %d pasos", stepper_cm_to_steps(STEPPER_SW_DER_CM, m));
    CHECK(stepper_cm_to_steps(STEPPER_SW_IZQ_CM, m) == 21000,
          "SW izq = %d pasos", stepper_cm_to_steps(STEPPER_SW_IZQ_CM, m));
}

static void test_calibrada(void)
{
    printf("  pendiente calibrada: la recta pasa por los dos switches\n");

    const int32_t der = stepper_cm_to_steps(STEPPER_SW_DER_CM, M_CALIBRADA);
    const int32_t izq = stepper_cm_to_steps(STEPPER_SW_IZQ_CM, M_CALIBRADA);
    CHECK(abs(izq - der - SPAN_MEDIDO) <= 1,
          "izq - der = %d pasos, span = %d", izq - der, SPAN_MEDIDO);
}

static void test_ida_y_vuelta(void)
{
    printf("  cm -> pasos -> cm no deriva más de medio paso, con ambas pendientes\n");

    const float pendientes[] = { STEPPER_STEPS_PER_CM_TEORICO, M_CALIBRADA };
    for (int i = 0; i < 2; i++) {
        const float m = pendientes[i];
        for (float cm = STEPPER_SW_DER_CM; cm <= STEPPER_SW_IZQ_CM; cm += 0.5f) {
            const float otra = stepper_steps_to_cm(stepper_cm_to_steps(cm, m), m);
            CHECK(CLOSE(otra, cm, 0.5f / m + 1e-4f),
                  "m = %f: %f cm -> %f cm", (double)m, (double)cm, (double)otra);
        }
    }

    const float m = STEPPER_STEPS_PER_CM_TEORICO;
    CHECK(stepper_cm_to_steps(0.006f, m) == 2, "1,5 pasos no redondeó a 2");
    CHECK(stepper_cm_to_steps(0.0039f, m) == 1, "0,975 pasos no redondeó a 1");
}

static void test_acotado(void)
{
    printf("  una consigna fuera del riel se acota a los switches\n");

    const float   m  = STEPPER_STEPS_PER_CM_TEORICO;
    const int32_t lo = stepper_cm_to_steps(STEPPER_SW_DER_CM, m);
    const int32_t hi = stepper_cm_to_steps(STEPPER_SW_IZQ_CM, m);

    CHECK(stepper_clamp_steps(stepper_cm_to_steps(0.0f, m), lo, hi) == lo,
          "0 cm (el parlante) no quedó en el SW derecho");
    CHECK(stepper_clamp_steps(stepper_cm_to_steps(500.0f, m), lo, hi) == hi,
          "500 cm no quedó en el SW izquierdo");
    CHECK(stepper_clamp_steps(stepper_cm_to_steps(55.0f, m), lo, hi) == 13750,
          "55 cm dentro del riel fue alterado");
}

int main(void)
{
    printf("Tests de host de E3-StepMotor\n");
    printf("=============================\n");

    test_teorica();
    test_calibrada();
    test_ida_y_vuelta();
    test_acotado();

    printf("\n%d comprobaciones, %d fallos\n", g_checks, g_failures);
    if (g_failures == 0) { printf("PASS\n"); return 0; }
    printf("FALLO\n");
    return 1;
}
