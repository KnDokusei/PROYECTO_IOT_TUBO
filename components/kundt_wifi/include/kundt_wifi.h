/*
 * kundt_wifi.h - WiFi en modo estación con reconexión automática.
 *
 * El README original afirma que ante una caída de WiFi la placa "detectará el
 * cambio, e intentará reconectar". En la versión Arduino eso valía para E2, E3 y
 * EC, pero no para E1: conectaba una vez en setup() y no volvía a comprobar
 * (hallazgo A1). Aquí la reconexión la disparan callbacks de esp_event, así que
 * es una propiedad del componente y no algo que un lazo de sondeo deba recordar
 * hacer.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Levanta netif, el lazo de eventos por defecto y el driver de WiFi. */
esp_err_t kundt_wifi_init(void);

/**
 * @brief Inicia la conexión. Retorna de inmediato; el enlace sube de forma
 *        asíncrona.
 *
 * Ante una desconexión el componente reintenta por su cuenta, espaciando los
 * intentos desde KUNDT_WIFI_RETRY_MIN_MS hasta KUNDT_WIFI_RETRY_MAX_MS. Nunca se
 * da por vencido: el equipo vive en un laboratorio y se espera que se recupere
 * solo tras un reinicio del AP o un corte breve de energía.
 */
esp_err_t kundt_wifi_connect(const char *ssid, const char *password);

/** @brief Verdadero mientras la estación tenga dirección IP. */
bool kundt_wifi_is_connected(void);

/**
 * @brief Bloquea hasta que haya conexión.
 * @return ESP_OK, o ESP_ERR_TIMEOUT si el enlace no subió a tiempo.
 */
esp_err_t kundt_wifi_wait_connected(uint32_t timeout_ms);

/** @brief IP de la estación en decimal punteado, o "0.0.0.0" si está caída. */
const char *kundt_wifi_ip(void);

/** @brief Cantidad de desconexiones vistas desde el arranque. */
uint32_t kundt_wifi_disconnect_count(void);

#define KUNDT_WIFI_RETRY_MIN_MS 1000
#define KUNDT_WIFI_RETRY_MAX_MS 30000

#ifdef __cplusplus
}
#endif
