/*
 * Tests de host de la lógica pura del módulo E3 (motor paso a paso).
 *
 * Cubren lo que en el banco es invisible: la conversión pasos<->cm, el acotado
 * al recorrido del riel y —sobre todo— la asimetría de unidades del hallazgo
 * A3, que se demuestra aquí en vez de describirse.
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

static void test_geometria(void)
{
    printf("  la geometría del riel da 250 pasos por centímetro\n");

    /* 200 pasos por vuelta / 0,8 cm por vuelta = 250 pasos/cm exactos. */
    CHECK(CLOSE(STEPPER_STEPS_PER_CM, 250.0f, 0.001f),
          "pasos por cm = %f", (double)STEPPER_STEPS_PER_CM);

    CHECK(stepper_cm_to_steps(1.0f) == 250, "1 cm -> %d pasos", stepper_cm_to_steps(1.0f));
    CHECK(stepper_cm_to_steps(0.0f) == 0, "0 cm no es 0 pasos");

    /* Los fines de carrera del stepperConfig.h original. */
    CHECK(stepper_neg_limit_steps() == 6500, "límite - = %d", stepper_neg_limit_steps());
    CHECK(stepper_pos_limit_steps() == 21000, "límite + = %d", stepper_pos_limit_steps());

    /* Recorrido útil: 84 - 26 = 58 cm. */
    CHECK(stepper_pos_limit_steps() - stepper_neg_limit_steps() == 14500,
          "recorrido = %d pasos", stepper_pos_limit_steps() - stepper_neg_limit_steps());
}

static void test_resolucion(void)
{
    printf("  un paso son 0,004 cm, muy por debajo de los 0,1 cm del README\n");

    const float un_paso = stepper_steps_to_cm(1);
    CHECK(CLOSE(un_paso, 0.004f, 1e-6f), "un paso = %f cm", (double)un_paso);

    /* El hallazgo A2: el sketch sólo refrescaba la telemetría tras media vuelta
     * del motor, o sea 100 pasos, y eso son 0,4 cm. Cuatro veces peor que lo
     * prometido. Informar la posición real del contador lo deja en 0,004 cm. */
    const float media_vuelta = stepper_steps_to_cm(STEPPER_STEPS_PER_REV / 2);
    CHECK(CLOSE(media_vuelta, 0.4f, 1e-5f),
          "media vuelta = %f cm", (double)media_vuelta);
    CHECK(media_vuelta > 0.1f, "la resolución del sketch cumplía los 0,1 cm prometidos");
    CHECK(un_paso < 0.1f, "la resolución por paso no alcanza los 0,1 cm");
}

static void test_ida_y_vuelta(void)
{
    printf("  la conversión cm -> pasos -> cm no acumula deriva\n");

    for (float cm = STEPPER_NEG_LIMIT_CM; cm <= STEPPER_POS_LIMIT_CM; cm += 0.5f) {
        const int32_t pasos = stepper_cm_to_steps(cm);
        const float   otra  = stepper_steps_to_cm(pasos);
        CHECK(CLOSE(otra, cm, 0.002f), "%f cm -> %d pasos -> %f cm",
              (double)cm, pasos, (double)otra);
    }

    /* Redondeo al más cercano, no truncamiento: truncar sesgaría cada
     * conversión medio paso hacia el origen. */
    CHECK(stepper_cm_to_steps(0.006f) == 2, "0,006 cm -> %d pasos (1,5 redondea a 2)",
          stepper_cm_to_steps(0.006f));
    CHECK(stepper_cm_to_steps(0.0039f) == 1, "0,0039 cm -> %d pasos",
          stepper_cm_to_steps(0.0039f));
}

static void test_acotado(void)
{
    printf("  los destinos fuera del riel se acotan, no se ejecutan\n");

    const int32_t lo = stepper_neg_limit_steps();
    const int32_t hi = stepper_pos_limit_steps();

    CHECK(stepper_clamp_steps(0, lo, hi) == lo, "0 pasos no se acotó al límite -");
    CHECK(stepper_clamp_steps(999999, lo, hi) == hi, "un destino enorme no se acotó");
    CHECK(stepper_clamp_steps(-999999, lo, hi) == lo, "un destino negativo no se acotó");
    CHECK(stepper_clamp_steps(10000, lo, hi) == 10000, "un destino válido fue alterado");

    /* Un 0 del backend —el valor que producía un JSON ilegible en la versión
     * Arduino— mandaría el émbolo contra el tope del parlante. Acotado queda
     * en el fin de carrera negativo, que es donde el riel realmente empieza. */
    CHECK(stepper_clamp_steps(stepper_cm_to_steps(0.0f), lo, hi) == lo,
          "una posición 0 no quedó acotada al límite -");

    CHECK(!stepper_cm_in_range(0.0f), "0 cm no debería estar en rango");
    CHECK(!stepper_cm_in_range(100.0f), "100 cm no debería estar en rango");
    CHECK(stepper_cm_in_range(50.0f), "50 cm debería estar en rango");
    CHECK(stepper_cm_in_range(STEPPER_NEG_LIMIT_CM), "el límite - debería estar en rango");
    CHECK(stepper_cm_in_range(STEPPER_POS_LIMIT_CM), "el límite + debería estar en rango");
}

static void test_a3_unidades(void)
{
    printf("  A3: la unidad supuesta para 'embolo' cambia el destino en 10x\n");

    /* Mismo número del servidor, dos interpretaciones. */
    CHECK(CLOSE(stepper_input_to_cm(500.0f, STEPPER_INPUT_MM), 50.0f, 1e-4f),
          "500 en mm deberían ser 50 cm");
    CHECK(CLOSE(stepper_input_to_cm(500.0f, STEPPER_INPUT_CM), 500.0f, 1e-4f),
          "500 en cm deberían quedar en 500");

    /*
     * El riesgo concreto del hallazgo A3, demostrado en vez de descrito.
     *
     * Si el backend hace round-trip —guarda lo que llega por PUT en 'posicion'
     * y lo sirve por GET como 'embolo'— e interpretamos milímetros, entonces
     * cada ciclo divide la posición por 10 y el émbolo colapsa hacia el origen.
     */
    float pos_cm = 50.0f; /* el émbolo arranca a media carrera */
    for (int ciclo = 0; ciclo < 3; ciclo++) {
        const float servido = pos_cm; /* el servidor devuelve lo último informado */
        pos_cm = stepper_input_to_cm(servido, STEPPER_INPUT_MM);
    }
    CHECK(CLOSE(pos_cm, 0.05f, 1e-4f),
          "tras 3 ciclos con round-trip la posición es %f cm", (double)pos_cm);
    CHECK(!stepper_cm_in_range(pos_cm),
          "el colapso deja el destino fuera del riel, donde el acotado lo frena");

    /* Con la interpretación simétrica el round-trip es estable. */
    pos_cm = 50.0f;
    for (int ciclo = 0; ciclo < 3; ciclo++) {
        pos_cm = stepper_input_to_cm(pos_cm, STEPPER_INPUT_CM);
    }
    CHECK(CLOSE(pos_cm, 50.0f, 1e-4f), "en cm el round-trip debería ser estable");
}

static void test_a3_acotado_protege(void)
{
    printf("  A3: aunque la unidad esté mal, el acotado impide dañar el riel\n");

    const int32_t lo = stepper_neg_limit_steps();
    const int32_t hi = stepper_pos_limit_steps();

    /* Interpretar como cm un valor que venía en mm pide 500 cm: imposible. */
    const float   mal  = stepper_input_to_cm(500.0f, STEPPER_INPUT_CM);
    const int32_t dest = stepper_clamp_steps(stepper_cm_to_steps(mal), lo, hi);
    CHECK(dest == hi, "un destino de 500 cm debería acotarse al límite +");

    /* Y al revés: interpretar como mm un valor que venía en cm pide 5 cm. */
    const float   mal2  = stepper_input_to_cm(50.0f, STEPPER_INPUT_MM);
    const int32_t dest2 = stepper_clamp_steps(stepper_cm_to_steps(mal2), lo, hi);
    CHECK(dest2 == lo, "un destino de 5 cm debería acotarse al límite -");

    /* En ambos casos el émbolo va a un extremo, que es visible y recuperable;
     * nunca a una coordenada fuera del riel. */
    CHECK(dest >= lo && dest <= hi, "el destino quedó fuera del riel");
    CHECK(dest2 >= lo && dest2 <= hi, "el destino quedó fuera del riel");
}

int main(void)
{
    printf("Tests de host de E3-StepMotor\n");
    printf("=============================\n");

    test_geometria();
    test_resolucion();
    test_ida_y_vuelta();
    test_acotado();
    test_a3_unidades();
    test_a3_acotado_protege();

    printf("\n%d comprobaciones, %d fallos\n", g_checks, g_failures);
    if (g_failures == 0) { printf("PASS\n"); return 0; }
    printf("FALLO\n");
    return 1;
}
