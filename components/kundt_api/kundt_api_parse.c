/*
 * kundt_api_parse.c - JSON parsing for the backend response.
 *
 * Only depends on cJSON, never on esp_http_client, so the host tests can build
 * it directly against the cJSON shipped with ESP-IDF.
 */

#include <string.h>

#include "cJSON.h"
#include "esp_err.h"
#include "kundt_api.h"

esp_err_t kundt_api_parse_valores(const char *body, kundt_valores_t *out)
{
    if (body == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));

    /* cJSON_Parse returns NULL on malformed input. The Arduino build ignored
     * the equivalent error from deserializeJson(), so an HTML error page was
     * happily "parsed" into all-zero setpoints. */
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *valores = cJSON_GetObjectItemCaseSensitive(root, "valores");
    if (!cJSON_IsObject(valores)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *f = cJSON_GetObjectItemCaseSensitive(valores, "frecuencia");
    if (cJSON_IsNumber(f)) {
        out->frecuencia     = f->valueint;
        out->has_frecuencia = true;
    }

    const cJSON *v = cJSON_GetObjectItemCaseSensitive(valores, "volumen");
    if (cJSON_IsNumber(v)) {
        out->volumen     = v->valueint;
        out->has_volumen = true;
    }

    const cJSON *e = cJSON_GetObjectItemCaseSensitive(valores, "embolo");
    if (cJSON_IsNumber(e)) {
        out->embolo     = (float)e->valuedouble;
        out->has_embolo = true;
    }

    cJSON_Delete(root);
    return ESP_OK;
}
