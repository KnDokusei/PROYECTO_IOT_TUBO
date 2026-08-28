/*
 * stepper.h - Motor paso a paso del émbolo (módulo E3), sobre driver A4988.
 *
 * Reemplaza a AccelStepper. La diferencia de fondo no es la librería sino el
 * dueño del estado: aquí los pasos los genera una alarma de GPTimer y toda la
 * posición vive dentro de este componente, protegida por una sección crítica.
 * En la versión Arduino el objeto AccelStepper se compartía entre dos tareas
 * ancladas a núcleos distintos sin ningún mutex (hallazgo M4).
 *
 * Tiempos tomados del datasheet del Allegro A4988:
 *   - tras llevar SLEEP a alto hay que esperar 1 ms a que estabilice el charge
 *     pump antes de aceptar pulsos de STEP (hallazgo M2);
 *   - el ancho mínimo de pulso en STEP es 1 us.
 *
 * Los fines de carrera son GPIO34/35: sólo entrada y sin pull interno, así que
 * el cableado obliga a pull-ups externos a 3V3 con los switches a GND. El
 * firmware asume lógica activa-baja.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "stepper_math.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int step_gpio;
    int dir_gpio;
    int sleep_gpio;
    int endstop_pos_gpio; /* más lejano al parlante */
    int endstop_neg_gpio; /* más cercano al parlante */
    /* Heredado de setPinsInverted(true, false, false) del sketch original: en
     * este cableado DIR en bajo mueve hacia el extremo positivo. */
    bool     invert_dir;
    uint32_t speed_sps;       /* pasos por segundo en marcha normal */
    uint32_t calib_speed_sps; /* pasos por segundo durante la calibración */
} stepper_config_t;

#define STEPPER_DEFAULT_CONFIG()          \
    (stepper_config_t)                    \
    {                                     \
        .step_gpio        = 12,           \
        .dir_gpio         = 14,           \
        .sleep_gpio       = 33,           \
        .endstop_pos_gpio = 34,           \
        .endstop_neg_gpio = 35,           \
        .invert_dir       = true,         \
        .speed_sps        = 500,          \
        .calib_speed_sps  = 300,          \
    }

/** @brief Estado de los fines de carrera (activos en bajo). */
typedef struct {
    bool pos_pressed;
    bool neg_pressed;
} stepper_endstops_t;

/** @brief Contadores útiles para diagnosticar sin desmontar el equipo. */
typedef struct {
    int32_t  position_steps;
    int32_t  target_steps;
    bool     moving;
    bool     calibrated;
    uint32_t endstop_stops; /* movimientos abortados por un fin de carrera */
    uint64_t steps_emitted;
} stepper_stats_t;

/** @brief Configura GPIO y temporizador. No mueve el motor. */
esp_err_t stepper_init(const stepper_config_t *cfg);

/**
 * @brief Lee los fines de carrera.
 *
 * Que ambos den "pulsado" con el émbolo a media carrera indica que faltan los
 * pull-ups externos, no que el riel esté en un extremo.
 */
stepper_endstops_t stepper_read_endstops(void);

/**
 * @brief Busca el fin de carrera negativo y fija ahí el origen.
 *
 * Bloquea hasta terminar. A diferencia del sketch original, que giraba en un
 * `while (true)` sin salida, aquí hay un presupuesto máximo de pasos: si el
 * switch no responde se aborta con error en vez de empujar el riel contra el
 * tope indefinidamente (hallazgo B2).
 *
 * @return ESP_OK; ESP_ERR_TIMEOUT si se agotó el presupuesto de pasos;
 *         ESP_ERR_INVALID_STATE si ambos fines de carrera están activos.
 */
esp_err_t stepper_calibrate(void);

/** @brief Ordena mover a una posición absoluta en pasos. Retorna de inmediato. */
esp_err_t stepper_move_to(int32_t target_steps);

/** @brief Detiene el movimiento y duerme el driver. */
void stepper_stop(void);

/** @brief Posición actual en pasos. */
int32_t stepper_position(void);

/**
 * @brief Declara la posición actual sin mover el motor.
 *
 * Es lo que hace la calibración al tocar el fin de carrera. Se expone para las
 * pruebas de banco, donde no hay riel contra el que referenciarse. En operación
 * normal no debería llamarse: fija el origen a ciegas.
 */
void stepper_set_position(int32_t steps);

/** @brief True mientras quede camino por recorrer. */
bool stepper_is_moving(void);

/** @brief Instantánea coherente de todo el estado. */
void stepper_get_stats(stepper_stats_t *out);

#ifdef __cplusplus
}
#endif
