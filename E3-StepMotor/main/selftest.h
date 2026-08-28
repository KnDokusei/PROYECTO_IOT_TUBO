/*
 * selftest.h - Validación de banco del módulo E3 sin motor, driver ni riel.
 *
 * Cuenta los pulsos reales de STEP con el periférico PCNT del propio chip,
 * enrutando la salida hacia la entrada por dentro del GPIO matrix
 * (flag io_loop_back). No hace falta ningún cable externo.
 *
 * No se compila salvo que CONFIG_E3_SELFTEST esté activo.
 */
#pragma once

/* Explícito y no por herencia de otra cabecera: sin esto CONFIG_E3_SELFTEST
 * queda indefinido, el `#if` de abajo da falso y el .c entero se compila
 * vacío, con un fallo de enlace como único síntoma. */
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_E3_SELFTEST

/** @brief Ejecuta la batería completa y deja el resultado en el log. */
void selftest_run(void);

#if CONFIG_E3_SELFTEST_ENDSTOP_SIM
/**
 * @brief Deja los pines que simulan los fines de carrera en "libre".
 *
 * Se llama justo después de stepper_init() y antes de leer los fines de
 * carrera por primera vez, para que el estado que se registra al arrancar sea
 * el simulado y no el de unos pines al aire.
 */
void selftest_endstop_sim_init(void);
#endif

#endif /* CONFIG_E3_SELFTEST */

#ifdef __cplusplus
}
#endif
