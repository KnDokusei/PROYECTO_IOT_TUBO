/*
 * E1-Mic - Kundt tube microphone module, ESP-IDF port.
 *
 * Role (per the project README): monitor the audio the speaker module (E2)
 * injects into the tube, using the microphone fixed at the tube inlet, and
 * stream it to the server over a binary WebSocket so the remote user can listen
 * to the standing wave as the piston sweeps.
 *
 * Signal path: electret mic -> preamp -> LM324 -> GPIO34 (ADC1_CH6)
 *              -> SAR ADC in DMA continuous mode -> PCM16 -> WebSocket.
 *
 * Ported from the Arduino sketch that used the legacy I2S built-in ADC mode,
 * which no longer exists in current ESP-IDF. See MIGRACION-E1.md.
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

/* One PCM block per WebSocket frame. Matches the DMA frame size so a read maps
 * to exactly one send: 512 samples = 1024 bytes, ~86 frames/s at 44.1 kHz. */
#define PCM_BLOCK_SAMPLES 512

/* Long enough that a quiet tube does not spam timeouts, short enough that a
 * stalled ADC is noticed promptly. */
#define ADC_READ_TIMEOUT_MS 200

/* Progress logging cadence. */
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
        ESP_LOGI(TAG, "websocket connected");
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "websocket disconnected, client will retry");
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "websocket error (esp_tls err=0x%x)",
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
        ESP_LOGE(TAG, "cannot build websocket URI: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "websocket target: %s", uri);

    const esp_websocket_client_config_t cfg = {
        .uri                 = uri,
        /* The client reconnects on its own; the Arduino build relied on a
         * callback that was never dispatched because poll() was never called. */
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms   = 10000,
        .buffer_size          = PCM_BLOCK_SAMPLES * sizeof(int16_t) * 2,
        .task_stack           = 6144,
    };

    s_ws = esp_websocket_client_init(&cfg);
    if (s_ws == NULL) {
        ESP_LOGE(TAG, "esp_websocket_client_init failed");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_websocket_register_events(
        s_ws, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL));

    return esp_websocket_client_start(s_ws);
}

#if CONFIG_E1_SELFTEST_DAC
/*
 * Feeds the ADC from the chip own cosine-wave generator so the whole capture
 * path can be exercised with no microphone and no signal generator. Needs a
 * jumper from the DAC pin to GPIO34.
 *
 * The generator is driven by the RTC clock and is independent of the I2S0
 * block the continuous ADC driver uses, so the two can run at once.
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
        /* The offset is written straight into the DAC's 8-bit DC register. With
         * offset 0 the cosine swings around 0 V and the DAC clips every negative
         * half-cycle, so the ADC only ever sees a rectified stub. Biasing to
         * ~100/255 puts the whole waveform inside the converter's range. */
        .offset  = 100,
        .flags   = { .force_set_freq = true },
    };

    ESP_ERROR_CHECK(dac_cosine_new_channel(&cfg, &handle));
    ESP_ERROR_CHECK(dac_cosine_start(handle));

    ESP_LOGW(TAG, "SELF-TEST: %d Hz cosine on GPIO%d",
             CONFIG_E1_SELFTEST_DAC_FREQ,
             CONFIG_E1_SELFTEST_DAC_GPIO25 ? 25 : 26);
    ESP_LOGW(TAG, "SELF-TEST: jumper GPIO%d -> GPIO34 required",
             CONFIG_E1_SELFTEST_DAC_GPIO25 ? 25 : 26);
}
#endif /* CONFIG_E1_SELFTEST_DAC */


#if CONFIG_E1_SELFTEST_DAC
/*
 * Dumps one block of PCM as hex over the console, once, a few seconds after
 * capture starts (i.e. after the DC tracker has settled). Lets the whole
 * ADC -> DSP path be checked against a known tone on a host, with no network
 * and no server: frequency, symmetry and waveform shape all come out of it.
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

    ESP_LOGI(TAG, "streaming task started on core %d", xPortGetCoreID());

    for (;;) {
        size_t    samples = 0;
        esp_err_t err = mic_capture_read(s_pcm, PCM_BLOCK_SAMPLES, &samples,
                                         ADC_READ_TIMEOUT_MS);

        if (err == ESP_ERR_TIMEOUT) {
            continue;  /* No conversions ready yet; not an error. */
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
        /* Wait ~3 s of audio so the DC estimate has converged before dumping. */
        if (mic_capture_is_running()) {
            static uint32_t blocks;
            if (++blocks == 260) {
                selftest_dump_block(s_pcm, samples);
            }
        }
#endif

        /* Drop audio while the link is down rather than blocking the ADC path:
         * the tube keeps running and stale audio is worthless to the listener. */
        if (esp_websocket_client_is_connected(s_ws)) {
            const int bytes = (int)(samples * sizeof(int16_t));
            const int wrote  = esp_websocket_client_send_bin(
                s_ws, (const char *)s_pcm, bytes, pdMS_TO_TICKS(1000));

            if (wrote < 0) {
                send_fails++;  /* Counted, not logged: this is the hot path. */
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
                     "ADC  captured=%llu dc=%ld min=%d max=%d rms=%lu drop=%llu ovf=%lu",
                     (unsigned long long)st.samples_captured,
                     (long)st.dc_offset,
                     (int)st.pcm_min,
                     (int)st.pcm_max,
                     (unsigned long)st.pcm_rms,
                     (unsigned long long)st.words_dropped,
                     (unsigned long)st.pool_overflows);
            ESP_LOGI(TAG,
                     "NET  wifi=%s(%lu drops) ws=%s sent=%lluKiB failed=%lu",
                     kundt_wifi_ip(),
                     (unsigned long)kundt_wifi_disconnect_count(),
                     esp_websocket_client_is_connected(s_ws) ? "up" : "down",
                     (unsigned long long)(sent_bytes / 1024),
                     (unsigned long)send_fails);
            /* Heap is the figure that betrays a leak in the streaming path:
             * a slow drift down over hours is what takes a lab rig offline. */
            ESP_LOGI(TAG, "SYS  uptime=%llus heap=%u min_heap=%u",
                     (unsigned long long)(esp_timer_get_time() / 1000000),
                     (unsigned)esp_get_free_heap_size(),
                     (unsigned)esp_get_minimum_free_heap_size());
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Kundt tube - E1 microphone module (ESP-IDF)");

    /* Started first so the LED shows life even if provisioning fails. */
    ESP_ERROR_CHECK(kundt_led_init(KUNDT_LED_DEFAULT_GPIO));

    ESP_ERROR_CHECK(kundt_config_init());
    kundt_config_log();

    if (!kundt_config_is_provisioned()) {
        ESP_LOGE(TAG, "WiFi SSID or server IP not set.");
        ESP_LOGE(TAG, "Set them with 'idf.py menuconfig' under 'Kundt tube configuration',");
        ESP_LOGE(TAG, "then erase NVS once with 'idf.py erase-flash' so the new defaults load.");
        return;
    }

    kundt_config_t cfg;
    ESP_ERROR_CHECK(kundt_config_get(&cfg));

    ESP_ERROR_CHECK(kundt_wifi_init());
    ESP_ERROR_CHECK(kundt_wifi_connect(cfg.wifi_ssid, cfg.wifi_password));

    /* Wait for the first association so the WebSocket does not start against a
     * down interface. If it times out we continue anyway: the WiFi component
     * keeps retrying and the WebSocket client tolerates an unreachable host. */
    if (kundt_wifi_wait_connected(30000) != ESP_OK) {
        ESP_LOGW(TAG, "no WiFi after 30 s, continuing (retries run in background)");
    }

    ESP_ERROR_CHECK(websocket_start());

#if CONFIG_E1_SELFTEST_DAC
    kundt_led_mark_selftest();
    selftest_dac_start();
#endif

    const mic_capture_config_t mic_cfg = MIC_CAPTURE_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(mic_capture_start(&mic_cfg));

    /* Core 1 keeps the streaming path off core 0, where the WiFi stack runs. */
    xTaskCreatePinnedToCore(mic_stream_task, "mic_stream", 4096, NULL, 5, NULL, 1);
}
