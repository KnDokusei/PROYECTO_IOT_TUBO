/*
 * kundt_api.h - Cliente del backend del laboratorio del tubo de Kundt.
 *
 * Compartido por E2 y E3: en la versión Arduino, apiGET() estaba copiado textual
 * en ambos sketches, porque el IDE no permite compartir funciones entre sketches.
 *
 * Contrato (deducido de los sketches originales; el backend no tiene especificación
 * escrita):
 *
 *   GET /api/kundt/equipo/{kit}
 *     -> {"valores": {"frecuencia": int, "volumen": int, "embolo": float}}
 *
 * Sobre "volumen": pese al nombre NO es un porcentaje. El firmware original lo
 * escribía directo al servo como ángulo en grados, así que la calibración vive
 * en el backend. Ver hallazgo M1.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Valores leídos del backend. Las banderas `has_*` distinguen "ausente en la
 * respuesta" de "presente y valiendo cero"; la versión Arduino no podía, y un
 * campo faltante se volvía 0 en silencio, llevando el generador a 0 Hz. */
typedef struct {
    int   frecuencia;   /* Hz */
    int   volumen;      /* grados del servo, ver la nota de arriba */
    float embolo;       /* posición del émbolo; unidades sin confirmar, hallazgo A3 */
    bool  has_frecuencia;
    bool  has_volumen;
    bool  has_embolo;
} kundt_valores_t;

/**
 * @brief Configura el cliente. No realiza ninguna petición.
 * @param server_ip Dirección IPv4 del backend en notación decimal punteada.
 * @param port      Puerto del backend (5000 en esta instalación).
 * @param kit       Número de equipo, 1..5.
 */
esp_err_t kundt_api_init(const char *server_ip, uint16_t port, uint8_t kit);

/**
 * @brief Pide por GET las consignas actuales.
 *
 * A diferencia de la versión Arduino, aquí un estado HTTP de error sí es un
 * error: esp_http_client mantiene separados el resultado del transporte
 * (esp_err_t) y el código de estado HTTP, así que un 404 ya no puede confundirse
 * con un éxito (hallazgo A4).
 *
 * @return ESP_OK; ESP_ERR_INVALID_RESPONSE si el estado no es 200 o el cuerpo no
 *         se puede parsear; o el error de transporte de esp_http_client.
 */
esp_err_t kundt_api_get_valores(kundt_valores_t *out);

/**
 * @brief Informa por PUT la posición actual del émbolo, en centímetros.
 *
 * Sólo lo usa E3. La clave del cuerpo es "posicion", distinta de la que se lee
 * en el GET ("valores.embolo"): esa asimetría es el hallazgo A3 y está
 * documentada en stepper_math.h.
 *
 * Como en el GET, un estado HTTP distinto de 200 es un error y no un éxito
 * (hallazgo A4).
 *
 * @return ESP_OK; ESP_ERR_INVALID_RESPONSE si el estado no es 2xx; o el error
 *         de transporte de esp_http_client.
 */
esp_err_t kundt_api_put_posicion(float posicion_cm);

/** @brief Endpoint al que apunta el cliente, para el log. */
const char *kundt_api_url(void);

/**
 * @brief Parsea el cuerpo de una respuesta. Expuesta para los tests.
 *
 * Separada de la capa HTTP para poder ejercitar el manejo de JSON en el host
 * contra entradas malformadas y hostiles.
 *
 * @return ESP_OK, o ESP_ERR_INVALID_RESPONSE si el cuerpo no es JSON válido o no
 *         trae el objeto "valores".
 */
esp_err_t kundt_api_parse_valores(const char *body, kundt_valores_t *out);

#ifdef __cplusplus
}
#endif
