/*
 * kundt_api_parse.c - Parseo JSON de la respuesta del backend.
 *
 * Sólo depende de cJSON, nunca de esp_http_client, así los tests de host lo
 * compilan directo contra el cJSON que trae ESP-IDF.
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

    /* cJSON_Parse devuelve NULL ante una entrada malformada. La versión Arduino
     * ignoraba el error equivalente de deserializeJson(), así que una página HTML
     * de error se "parseaba" tan campante como consignas en cero. */
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
