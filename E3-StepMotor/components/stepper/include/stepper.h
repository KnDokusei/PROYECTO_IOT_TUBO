/*
 * stepper.h - Driver del A4988 para el émbolo de E3.
 *
 * Da pasos hacia un objetivo y para al llegar o al tocar el switch del sentido
 * de avance. No sabe de centímetros, límites ni calibración: eso vive en main.c.
 *
 * Los pasos los genera una alarma de GPTimer; la posición vive aquí, protegida
 * por una sección crítica.
 *
 * Datasheet del Allegro A4988:
 *   - tras llevar SLEEP a alto, esperar 1 ms (charge pump) antes de dar pasos;
 *   - ancho mínimo del pulso de STEP: 1 µs.
 *
 * Los switches van en GPIO34/35: sólo entrada y sin pull interno, así que llevan
 * pull-up externo a 3V3 y cierran a GND (activos en bajo).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STEPPER_IZQ  1 /* hacia el SW izquierdo, alejándose del parlante */
#define STEPPER_DER -1 /* hacia el SW derecho, acercándose al parlante */

typedef struct {
    int  step_gpio;
    int  dir_gpio;
    int  sleep_gpio;
    int  sw_izq_gpio; /* switch a 84 cm del parlante */
    int  sw_der_gpio; /* switch a 26 cm del parlante */
    bool invert_dir;  /* con este cableado, DIR en bajo mueve hacia la izquierda */
} stepper_config_t;

#define STEPPER_DEFAULT_CONFIG()   \
    (stepper_config_t)             \
    {                              \
        .step_gpio   = 12,         \
        .dir_gpio    = 14,         \
        .sleep_gpio  = 33,         \
        .sw_izq_gpio = 34,         \
        .sw_der_gpio = 35,         \
        .invert_dir  = true,       \
    }

/** @brief Configura GPIO y temporizador. No mueve el motor. */
esp_err_t stepper_init(const stepper_config_t *cfg, uint32_t steps_per_sec);

/** @brief Cambia la velocidad; vale también en marcha. */
esp_err_t stepper_set_speed(uint32_t steps_per_sec);

/** @brief Ordena ir a `target` pasos. Retorna de inmediato. No acota. */
void stepper_move_to(int32_t target);

/** @brief Detiene el movimiento y duerme el driver. */
void stepper_stop(void);

/** @brief Posición actual en pasos. */
int32_t stepper_position(void);

/** @brief Declara la posición actual sin mover el motor (re-referencia). */
void stepper_set_position(int32_t steps);

/** @brief True mientras quede camino por recorrer. */
bool stepper_is_moving(void);

/** @brief Switch que detuvo el último movimiento: 0, STEPPER_IZQ o STEPPER_DER. */
int stepper_switch_hit(void);

/** @brief Lee ambos switches (true = pulsado). */
void stepper_read_switches(bool *izq, bool *der);

#ifdef __cplusplus
}
#endif
