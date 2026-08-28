/*
 * servo.h - Servo de volumen del módulo E2, gobernado por LEDC.
 *
 * Reemplaza a la librería ESP32Servo de Arduino. Esa librería usaba LEDC por
 * debajo de todas formas, así que es el mismo hardware con una dependencia menos.
 */
#pragma once

#include "esp_err.h"
#include "servo_map.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int gpio;         /* Pin de señal. GPIO21 en el esquemático del Kundt. */
    int initial_angle;
} servo_config_t;

#define SERVO_DEFAULT_CONFIG()   \
    (servo_config_t)             \
    {                            \
        .gpio          = 21,     \
        .initial_angle = 0,      \
    }

/** @brief Configura el temporizador y el canal LEDC, y va al ángulo inicial. */
esp_err_t servo_init(const servo_config_t *cfg);

/**
 * @brief Mueve a un ángulo. Los valores fuera de rango se acotan, no se rechazan.
 *
 * Si el ángulo no cambió no se escribe nada, así un backend que repite la misma
 * consigna cada dos segundos no deja al servo zumbando.
 */
esp_err_t servo_set_angle(int angle_deg);

/** @brief Ángulo comandado actualmente. */
int servo_get_angle(void);

#ifdef __cplusplus
}
#endif
