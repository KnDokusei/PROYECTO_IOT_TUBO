/*
 * servo_map.h - Conversión de ángulo a PWM del servo de volumen del módulo E2.
 *
 * Aritmética entera pura, sin dependencias de ESP-IDF, para poder probarla en el
 * host. El manejo de LEDC vive en servo.c.
 *
 * Contexto: el servo gira el potenciómetro a la salida del amplificador de
 * audio, que es como el usuario remoto ajusta el volumen. El backend envía la
 * posición ya expresada en grados (hallazgo M1), así que este módulo sólo tiene
 * que acotarla y convertirla en ciclo de trabajo.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Servo estándar de aeromodelismo: trama de 20 ms y pulso de 0,5 a 2,5 ms para
 * recorrer de 0 a 180 grados. */
#define SERVO_FREQ_HZ       50
#define SERVO_MIN_PULSE_US  500
#define SERVO_MAX_PULSE_US  2500
#define SERVO_MAX_ANGLE     180
#define SERVO_DUTY_RES_BITS 16

/**
 * @brief Acota un ángulo al recorrido mecánico del servo.
 *
 * La versión Arduino pasaba el valor del backend directo a Servo::write(), donde
 * cualquier cifra de 544 en adelante se reinterpreta como ancho de pulso en
 * microsegundos y no como ángulo: un valor malo empujaba el servo contra su tope
 * (hallazgo M1). Acotar aquí lo vuelve imposible.
 */
int servo_clamp_angle(int angle_deg);

/**
 * @brief Ciclo de trabajo de un ángulo, con resolución SERVO_DUTY_RES_BITS.
 *
 * duty = pulse_us * 2^bits * freq / 1e6. El ángulo se acota antes.
 */
uint32_t servo_angle_to_duty(int angle_deg);

/** @brief Ancho de pulso en microsegundos para un ángulo. Se acota. */
uint32_t servo_angle_to_pulse_us(int angle_deg);

#ifdef __cplusplus
}
#endif
