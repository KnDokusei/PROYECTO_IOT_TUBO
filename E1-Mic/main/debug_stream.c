/*
 * debug_stream.c - ver debug_stream.h.
 *
 * sdkconfig.h va primero y explícito: lo demás llega por includes transitivos y
 * un reordenamiento dejaría el archivo compilando vacío sin decir nada.
 */
#include "sdkconfig.h"

#include "debug_stream.h"

#if CONFIG_E1_DEBUG_STREAM

#include "esp_event.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"

#include "kundt_config.h"

static const char *TAG = "E1-debug";

static esp_websocket_client_handle_t s_ws;
static uint64_t                      s_sent_bytes;
static uint32_t                      s_send_fails;

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;

    switch ((esp_websocket_event_id_t)id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket conectado");
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket desconectado");
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "error de WebSocket");
        break;
    default:
        break;
    }
}

void debug_stream_start(void)
{
    char uri[64];
    if (kundt_config_ws_uri(uri, sizeof(uri)) != ESP_OK) {
        ESP_LOGE(TAG, "no se pudo armar la URI del WebSocket");
        return;
    }

    const esp_websocket_client_config_t cfg = {
        .uri                 = uri,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms   = 5000,
    };

    s_ws = esp_websocket_client_init(&cfg);
    if (s_ws == NULL) {
        ESP_LOGE(TAG, "no se pudo crear el cliente de WebSocket");
        return;
    }

    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
    if (esp_websocket_client_start(s_ws) != ESP_OK) {
        ESP_LOGE(TAG, "no se pudo arrancar el cliente de WebSocket");
        return;
    }

    ESP_LOGW(TAG, "COMPILACIÓN DE DEPURACIÓN: audio crudo saliendo por %s", uri);
}

void debug_stream_send(const int16_t *pcm, size_t samples)
{
    if (s_ws == NULL || !esp_websocket_client_is_connected(s_ws)) {
        return;  /* Sin enlace se descarta: el audio viejo no le sirve a nadie. */
    }

    const int bytes = (int)(samples * sizeof(int16_t));
    const int wrote = esp_websocket_client_send_bin(s_ws, (const char *)pcm, bytes,
                                                    pdMS_TO_TICKS(1000));
    if (wrote < 0) {
        s_send_fails++;  /* Se cuenta, no se registra: es el camino crítico. */
    } else {
        s_sent_bytes += (uint64_t)wrote;
    }
}

void debug_stream_log(void)
{
    ESP_LOGI(TAG, "WS   enviado=%lluKiB fallos=%lu",
             (unsigned long long)(s_sent_bytes / 1024),
             (unsigned long)s_send_fails);
}

#else  /* compilación de producción: nada de esto existe */

void debug_stream_start(void) { }
void debug_stream_send(const int16_t *pcm, size_t samples) { (void)pcm; (void)samples; }
void debug_stream_log(void) { }

#endif
