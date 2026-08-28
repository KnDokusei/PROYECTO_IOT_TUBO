/*
 * kundt_api.h - Client for the Kundt tube lab backend.
 *
 * Shared by E2 and E3: in the Arduino build apiGET() was copy-pasted verbatim
 * into both sketches because the IDE cannot share functions between sketches.
 *
 * Contract (reverse-engineered from the original sketches; the backend has no
 * written spec):
 *
 *   GET /api/kundt/equipo/{kit}
 *     -> {"valores": {"frecuencia": int, "volumen": int, "embolo": float}}
 *
 * Note on "volumen": despite the name it is NOT a percentage. The original
 * firmware wrote it straight to the servo as an angle in degrees, so the
 * calibration lives in the backend. See finding M1.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Values read from the backend. The `has_*` flags distinguish "absent from the
 * response" from "present and zero" -- the Arduino build could not, and a
 * missing field silently became 0, driving the generator to 0 Hz. */
typedef struct {
    int   frecuencia;   /* Hz */
    int   volumen;      /* servo degrees, see note above */
    float embolo;       /* piston position; units unconfirmed, see finding A3 */
    bool  has_frecuencia;
    bool  has_volumen;
    bool  has_embolo;
} kundt_valores_t;

/**
 * @brief Configure the client. Does not perform any request.
 * @param server_ip Dotted-quad address of the backend.
 * @param port      Backend port (5000 in this deployment).
 * @param kit       Rig number, 1..5.
 */
esp_err_t kundt_api_init(const char *server_ip, uint16_t port, uint8_t kit);

/**
 * @brief GET the current setpoints.
 *
 * Unlike the Arduino version, an HTTP error status is an error here:
 * esp_http_client keeps the transport result (esp_err_t) and the HTTP status
 * code apart, so a 404 can no longer be mistaken for success (finding A4).
 *
 * @return ESP_OK; ESP_ERR_INVALID_RESPONSE on a non-200 status or unparseable
 *         body; or the transport error from esp_http_client.
 */
esp_err_t kundt_api_get_valores(kundt_valores_t *out);

/** @brief Endpoint the client is pointed at, for logging. */
const char *kundt_api_url(void);

/**
 * @brief Parse a response body. Exposed for testing.
 *
 * Split from the HTTP layer so the JSON handling can be exercised on the host
 * against malformed and hostile inputs.
 *
 * @return ESP_OK, or ESP_ERR_INVALID_RESPONSE if the body is not valid JSON or
 *         has no "valores" object.
 */
esp_err_t kundt_api_parse_valores(const char *body, kundt_valores_t *out);

#ifdef __cplusplus
}
#endif
