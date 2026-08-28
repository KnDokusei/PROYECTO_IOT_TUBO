/*
 * kundt_api.c - HTTP transport for the backend client. Parsing lives in
 * kundt_api_parse.c.
 */

#include "kundt_api.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "kundt_api";

/* The backend answers with a small JSON object; anything much larger than this
 * is not a response we know how to handle. */
#define RESPONSE_MAX 512

static char s_url[96];
static bool s_ready;

esp_err_t kundt_api_init(const char *server_ip, uint16_t port, uint8_t kit)
{
    if (server_ip == NULL || server_ip[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    const int n = snprintf(s_url, sizeof(s_url),
                           "http://%s:%u/api/kundt/equipo/%u",
                           server_ip, (unsigned)port, (unsigned)kit);
    if (n < 0 || (size_t)n >= sizeof(s_url)) {
        return ESP_ERR_INVALID_SIZE;
    }

    s_ready = true;
    ESP_LOGI(TAG, "endpoint: %s", s_url);
    return ESP_OK;
}

const char *kundt_api_url(void)
{
    return s_url;
}

esp_err_t kundt_api_get_valores(kundt_valores_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    char body[RESPONSE_MAX];

    const esp_http_client_config_t cfg = {
        .url                = s_url,
        .method             = HTTP_METHOD_GET,
        .timeout_ms         = 5000,
        .disable_auto_redirect = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "connect failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    esp_http_client_fetch_headers(client);

    /* Transport success and HTTP status are separate here, by design. The
     * Arduino build collapsed both into one int and treated 404 as success. */
    const int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGW(TAG, "HTTP %d from %s", status, s_url);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const int len = esp_http_client_read_response(client, body, sizeof(body) - 1);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (len <= 0) {
        ESP_LOGW(TAG, "empty response body");
        return ESP_ERR_INVALID_RESPONSE;
    }
    body[len] = '\0';

    err = kundt_api_parse_valores(body, out);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cannot parse response: %.64s", body);
    }
    return err;
}
