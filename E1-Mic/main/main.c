/*
 * E1-Mic - Módulo de micrófono del tubo de Kundt, port a ESP-IDF.
 *
 * Función (según el README del proyecto): escuchar con el micrófono fijo en la
 * entrada del tubo el audio que el módulo de parlante (E2) inyecta, y enviarlo
 * al servidor por un WebSocket binario, para que el usuario remoto oiga la onda
 * estacionaria mientras el émbolo barre la longitud.
 *
 * Cadena de señal: micrófono electret -> preamplificador -> LM324 -> GPIO34
 *                  (ADC1_CH6) -> SAR ADC en modo continuo por DMA -> PCM16
 *                  -> WebSocket.
 *
 * Portado del sketch de Arduino, que usaba el modo ADC interno del driver I2S
 * antiguo, hoy inexistente en ESP-IDF. Ver MIGRACION-E1.md.
 */

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_E1_SELFTEST_DAC
#include "driver/dac_cosine.h"
#endif

#include "kundt_config.h"
#include "kundt_led.h"
#include "kundt_wifi.h"
#include "mic_capture.h"

static const char *TAG = "E1-Mic";

/* Un bloque PCM por trama de WebSocket. Coincide con el tamaño de trama del DMA
 * para que cada lectura sea exactamente un envío: 512 muestras = 1024 bytes,
 * ~86 tramas/s a 44,1 kHz. */
#define PCM_BLOCK_SAMPLES 512

/* Suficientemente largo para que un tubo en silencio no llene el log de
 * timeouts, y suficientemente corto para notar pronto un ADC detenido. */
#define ADC_READ_TIMEOUT_MS 200

/* Cadencia del log de avance. */
#define STATS_INTERVAL_MS 10000

static esp_websocket_client_handle_t s_ws;
static int16_t                       s_pcm[PCM_BLOCK_SAMPLES];

static void ws_event_handler(void            *arg,
                             esp_event_base_t base,
                             int32_t          id,
                             void            *data)
{
    (void)arg;
    (void)base;
    const esp_websocket_event_data_t *ev = (const esp_websocket_event_data_t *)data;

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "websocket conectado");
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "websocket desconectado; el cliente reintentará");
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "error de websocket (esp_tls err=0x%x)",
                 ev ? ev->error_handle.esp_tls_last_esp_err : 0);
        break;
    default:
        break;
    }
}

static esp_err_t websocket_start(void)
{
    char uri[64];
    esp_err_t err = kundt_config_ws_uri(uri, sizeof(uri));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no se pudo construir la URI del websocket: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "destino del websocket: %s", uri);

    const esp_websocket_client_config_t cfg = {
        .uri                 = uri,
        /* El cliente reconecta solo. La versión Arduino dependía de un callback
         * que nunca se despachaba, porque nunca se llamaba a poll(). */
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms   = 10000,
        .buffer_size          = PCM_BLOCK_SAMPLES * sizeof(int16_t) * 2,
        .task_stack           = 6144,
    };

    s_ws = esp_websocket_client_init(&cfg);
    if (s_ws == NULL) {
        ESP_LOGE(TAG, "esp_websocket_client_init falló");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_websocket_register_events(
        s_ws, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL));

    return esp_websocket_client_start(s_ws);
}

#if CONFIG_E1_SELFTEST_DAC
/*
 * Alimenta el ADC con el generador de coseno del propio chip, para ejercitar
 * toda la cadena de captura sin micrófono ni generador de señales. Requiere un
 * puente del pin del DAC a GPIO34.
 *
 * El generador se reloja con RTC_FAST, independiente del bloque I2S0 que usa el
 * driver continuo del ADC, así que ambos pueden correr a la vez.
 */
static void selftest_dac_start(void)
{
    static dac_cosine_handle_t handle;

    const dac_cosine_config_t cfg = {
#if CONFIG_E1_SELFTEST_DAC_GPIO25
        .chan_id = DAC_CHAN_0,   /* GPIO25 */
#else
        .chan_id = DAC_CHAN_1,   /* GPIO26 */
#endif
        .freq_hz = CONFIG_E1_SELFTEST_DAC_FREQ,
        .clk_src = DAC_COSINE_CLK_SRC_DEFAULT,
        .atten   = DAC_COSINE_ATTEN_DB_6,
        .phase   = DAC_COSINE_PHASE_0,
        /* El offset va directo al registro de continua de 8 bits del DAC. Con
         * offset 0 el coseno oscila en torno a 0 V y el DAC recorta cada
         * semiciclo negativo, dejando al ADC sólo un resto rectificado. Con 100
         * la onda entra completa en el rango del conversor. Medido en placa da
         * dc≈522 cuentas: el registro no escala como fracción directa del fondo
         * de escala, así que este valor es empírico, no calculado. */
        .offset  = 100,
        .flags   = { .force_set_freq = true },
    };

    ESP_ERROR_CHECK(dac_cosine_new_channel(&cfg, &handle));
    ESP_ERROR_CHECK(dac_cosine_start(handle));

    ESP_LOGW(TAG, "AUTOPRUEBA: coseno de %d Hz en GPIO%d",
             CONFIG_E1_SELFTEST_DAC_FREQ,
             CONFIG_E1_SELFTEST_DAC_GPIO25 ? 25 : 26);
    ESP_LOGW(TAG, "AUTOPRUEBA: se requiere puente GPIO%d -> GPIO34",
             CONFIG_E1_SELFTEST_DAC_GPIO25 ? 25 : 26);
}
#endif /* CONFIG_E1_SELFTEST_DAC */


#if CONFIG_E1_SELFTEST_DAC
/*
 * Vuelca una vez un bloque PCM en hexadecimal por consola, unos segundos después
 * de arrancar la captura (o sea, ya asentado el estimador de continua). Permite
 * verificar toda la cadena ADC -> DSP contra un tono conocido desde el PC, sin
 * red ni servidor: de ahí salen frecuencia, simetría y forma de onda.
 */
static void selftest_dump_block(const int16_t *pcm, size_t n)
{
    static bool done;
    if (done) {
        return;
    }
    done = true;

    printf("\n#PCMDUMP n=%u rate=44100\n", (unsigned)n);
    for (size_t i = 0; i < n; i++) {
        printf("%04x", (unsigned)(uint16_t)pcm[i]);
        if ((i % 32) == 31) {
            printf("\n");
        }
    }
    printf("\n#PCMEND\n\n");
}
#endif

static void mic_stream_task(void *arg)
{
    (void)arg;

    int64_t  last_stats = 0;
    uint64_t sent_bytes = 0;
    uint32_t send_fails = 0;

    ESP_LOGI(TAG, "tarea de streaming iniciada en el núcleo %d", xPortGetCoreID());

    for (;;) {
        size_t    samples = 0;
        esp_err_t err = mic_capture_read(s_pcm, PCM_BLOCK_SAMPLES, &samples,
                                         ADC_READ_TIMEOUT_MS);

        if (err == ESP_ERR_TIMEOUT) {
            continue;  /* Aún no hay conversiones listas; no es un error. */
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "mic_capture_read: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (samples == 0) {
            continue;
        }

#if CONFIG_E1_SELFTEST_DAC
        /* Esperar ~3 s de audio para volcar con el offset ya convergido. */
        if (mic_capture_is_running()) {
            static uint32_t blocks;
            if (++blocks == 260) {
                selftest_dump_block(s_pcm, samples);
            }
        }
#endif

        /* Con el enlace caído se descarta el audio en vez de bloquear la cadena
         * del ADC: el tubo sigue funcionando y el audio viejo no le sirve a
         * nadie. */
        if (esp_websocket_client_is_connected(s_ws)) {
            const int bytes = (int)(samples * sizeof(int16_t));
            const int wrote  = esp_websocket_client_send_bin(
                s_ws, (const char *)s_pcm, bytes, pdMS_TO_TICKS(1000));

            if (wrote < 0) {
                send_fails++;  /* Se cuenta, no se registra: es el camino crítico. */
            } else {
                sent_bytes += (uint64_t)wrote;
            }
        }

        kundt_led_set_state(!kundt_wifi_is_connected()   ? KUNDT_LED_NO_WIFI
                            : esp_websocket_client_is_connected(s_ws)
                                                            ? KUNDT_LED_RUNNING
                                                            : KUNDT_LED_NO_SERVER);

        const int64_t now = esp_timer_get_time() / 1000;
        if (now - last_stats >= STATS_INTERVAL_MS) {
            last_stats = now;

            mic_capture_stats_t st;
            mic_capture_get_stats(&st);

            ESP_LOGI(TAG,
                     "ADC  captadas=%llu dc=%ld min=%d max=%d rms=%lu descartes=%llu ovf=%lu",
                     (unsigned long long)st.samples_captured,
                     (long)st.dc_offset,
                     (int)st.pcm_min,
                     (int)st.pcm_max,
                     (unsigned long)st.pcm_rms,
                     (unsigned long long)st.words_dropped,
                     (unsigned long)st.pool_overflows);
            ESP_LOGI(TAG,
                     "RED  wifi=%s(%lu caídas) ws=%s enviado=%lluKiB fallos=%lu",
                     kundt_wifi_ip(),
                     (unsigned long)kundt_wifi_disconnect_count(),
                     esp_websocket_client_is_connected(s_ws) ? "arriba" : "abajo",
                     (unsigned long long)(sent_bytes / 1024),
                     (unsigned long)send_fails);
            /* El heap es la cifra que delata una fuga en la cadena de envío:
             * una caída lenta a lo largo de horas es lo que deja el equipo del
             * laboratorio fuera de servicio. */
            ESP_LOGI(TAG, "SIS  encendido=%llus heap=%u heap_min=%u",
                     (unsigned long long)(esp_timer_get_time() / 1000000),
                     (unsigned)esp_get_free_heap_size(),
                     (unsigned)esp_get_minimum_free_heap_size());
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Tubo de Kundt - módulo E1 de micrófono (ESP-IDF)");

    /* Se arranca primero para que el LED dé señales aunque falle la provisión. */
    ESP_ERROR_CHECK(kundt_led_init(KUNDT_LED_DEFAULT_GPIO));

    ESP_ERROR_CHECK(kundt_config_init());
    kundt_config_log();

    if (!kundt_config_is_provisioned()) {
        ESP_LOGE(TAG, "Falta el SSID de WiFi o la IP del servidor.");
        ESP_LOGE(TAG, "Configúralos con 'idf.py menuconfig', menú 'Kundt tube configuration',");
        ESP_LOGE(TAG, "y luego borra NVS una vez con 'idf.py erase-flash' para que carguen.");
        return;
    }

    kundt_config_t cfg;
    ESP_ERROR_CHECK(kundt_config_get(&cfg));

    ESP_ERROR_CHECK(kundt_wifi_init());
    ESP_ERROR_CHECK(kundt_wifi_connect(cfg.wifi_ssid, cfg.wifi_password));

    /* Esperar la primera asociación para no arrancar el WebSocket contra una
     * interfaz caída. Si vence el plazo se continúa igual: el componente de WiFi
     * sigue reintentando y el cliente WebSocket tolera un host inalcanzable. */
    if (kundt_wifi_wait_connected(30000) != ESP_OK) {
        ESP_LOGW(TAG, "sin WiFi tras 30 s; se continúa (los reintentos siguen en segundo plano)");
    }

    ESP_ERROR_CHECK(websocket_start());

#if CONFIG_E1_SELFTEST_DAC
    kundt_led_mark_selftest();
    selftest_dac_start();
#endif

    const mic_capture_config_t mic_cfg = MIC_CAPTURE_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(mic_capture_start(&mic_cfg));

    /* El núcleo 1 mantiene la cadena de envío fuera del 0, donde corre WiFi. */
    xTaskCreatePinnedToCore(mic_stream_task, "mic_stream", 4096, NULL, 5, NULL, 1);
}
