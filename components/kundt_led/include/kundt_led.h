/*
 * kundt_led.h - LED de estado compartido por los módulos del tubo de Kundt.
 *
 * Una vez montado el módulo en el equipo y sin consola serie conectada, el LED
 * de la placa es el único diagnóstico disponible. Por eso el patrón de parpadeo
 * codifica qué está haciendo el firmware, en vez de sólo demostrar que vive.
 *
 * En la DOIT DEVKIT V1, GPIO2 es el LED azul.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KUNDT_LED_DEFAULT_GPIO 2

typedef enum {
    KUNDT_LED_BOOT = 0,   /* Titileo rápido: arrancando o autoprueba en curso */
    KUNDT_LED_NO_WIFI,    /* Un parpadeo lento cada 2 s: sin red */
    KUNDT_LED_NO_SERVER,  /* Parpadeo doble: WiFi arriba, backend inalcanzable */
    KUNDT_LED_RUNNING,    /* Parpadeo parejo: todo funcionando */
    KUNDT_LED_SELFTEST,   /* Parpadeo triple: compilación de banco, NO de producción */
} kundt_led_state_t;

/**
 * @brief Arranca la tarea del LED. Se puede llamar antes que todo lo demás.
 * @param gpio Pin que maneja el LED; KUNDT_LED_DEFAULT_GPIO para el de la placa.
 */
esp_err_t kundt_led_init(int gpio);

/** @brief Cambia el patrón. Toma efecto al terminar el ciclo en curso. */
void kundt_led_set_state(kundt_led_state_t state);

/** @brief Patrón actual. */
kundt_led_state_t kundt_led_get_state(void);

/**
 * @brief Marca la compilación como autoprueba de banco.
 *
 * Una vez activado, el LED antepone un parpadeo triple distintivo a cualquier
 * patrón. Una compilación de autoprueba mueve pines fuera de donde los pone el
 * esquemático (el servo de E2 pasa de GPIO21 a GPIO25), así que grabarla en un
 * equipo real falla en silencio: el cable simplemente no se excita. Esto lo hace
 * visible desde el otro lado del banco y sin consola.
 */
void kundt_led_mark_selftest(void);

/** @brief Nombre legible de un estado, para el log. */
const char *kundt_led_state_name(kundt_led_state_t state);

#ifdef __cplusplus
}
#endif
