/*
 * selftest.h - Validación de banco del módulo E2 sin su hardware analógico.
 *
 * Mide el PWM del servo muestreándolo desde la entrada del ADC, para lo cual
 * basta un cable entre el pin del servo y esa entrada. No se compila salvo que
 * CONFIG_E2_SELFTEST esté activo.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_E2_SELFTEST

/**
 * @brief Comprueba que el puente conduce antes de confiar en ninguna medición.
 * @return ESP_OK si al excitar el pin de origen cambia el de destino.
 */
esp_err_t selftest_check_jumper(void);

/** @brief Deja listo el pin con el que se observa el PWM. */
esp_err_t selftest_init(void);

/**
 * @brief Comanda un ángulo y mide el pulso que produce.
 *
 * Muestrea GPIO34 en un lazo cerrado durante unas pocas tramas de PWM e informa
 * el tiempo en alto medido, comparándolo con lo que predice
 * servo_angle_to_pulse_us().
 *
 * @return ESP_OK si la medición cae dentro de la tolerancia.
 */
esp_err_t selftest_measure_angle(int angle_deg);

/** @brief Recorre un conjunto de ángulos e informa cada uno. */
void selftest_run_servo_sweep(void);

#endif /* CONFIG_E2_SELFTEST */

#ifdef __cplusplus
}
#endif
