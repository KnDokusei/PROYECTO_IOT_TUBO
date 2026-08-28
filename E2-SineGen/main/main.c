/*
 * E2-SineGen - Módulo de generación de audio del tubo de Kundt, port a ESP-IDF.
 *
 * Función (según el README del proyecto): generar la señal que excita el parlante
 * en un extremo del tubo. La frecuencia la entrega el backend y la sintetiza un
 * DDS AD9833; el volumen también viene del backend y lo fija un servo que gira
 * el potenciómetro a la salida del amplificador de audio.
 *
 * Cadena de señal: AD9833 (SPI) -> amplificador LM386 -> potenciómetro (servo)
 *                  -> parlante.
 *
 * Portado del sketch de Arduino. El comportamiento se conserva a propósito,
 * salvo donde el original estaba mal; esos casos se marcan con su ID de auditoría.
 */

#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ad9833.h"
#include "kundt_api.h"
#include "kundt_config.h"
#include "kundt_led.h"
#include "kundt_wifi.h"
#include "selftest.h"
#include "servo.h"

static const char *TAG = "E2-SineGen";

/* El sketch original consultaba cada 2 s. Se mantiene: el backend es compartido
 * con los otros módulos y no hay motivo para exigirlo más. */
#define POLL_INTERVAL_MS 2000

/* Puerto del backend para la API de valores. */
#define API_PORT 5000

/* Frecuencia inicial del AD9833; la misma que traía la versión Arduino. */
#define DEFAULT_FREQ_HZ 1200

/*
 * Límites de la banda de audio. El tubo es un experimento de física, no una
 * fuente de señal arbitraria: un valor fuera de este rango es un error del
 * backend, no una petición. La versión Arduino dejaba pasar cualquier cosa, y en
 * particular un parseo JSON fallido daba 0 Hz (hallazgo A4).
 */
#define FREQ_MIN_HZ 20
#define FREQ_MAX_HZ 20000

static ad9833_handle_t s_dds;

static int clamp_freq(int hz)
{
    if (hz < FREQ_MIN_HZ) {
        return FREQ_MIN_HZ;
    }
    if (hz > FREQ_MAX_HZ) {
        return FREQ_MAX_HZ;
    }
    return hz;
}

static void control_task(void *arg)
{
    (void)arg;

    int last_freq = -1;
    uint32_t ok = 0, failed = 0;

    ESP_LOGI(TAG, "lazo de control iniciado (consulta cada %d ms)", POLL_INTERVAL_MS);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));

        if (!kundt_wifi_is_connected()) {
            kundt_led_set_state(KUNDT_LED_NO_WIFI);
            continue;  /* El componente de WiFi reintenta por su cuenta. */
        }

        kundt_valores_t v;
        const esp_err_t err = kundt_api_get_valores(&v);
        if (err != ESP_OK) {
            failed++;
            kundt_led_set_state(KUNDT_LED_NO_SERVER);
            /*
             * Mantener la última consigna válida en vez de caer a cero. En la
             * versión Arduino una respuesta mala se convertía en silencio en
             * 0 Hz y 0 grados, así que un tropiezo del backend apagaba el tono y
             * mandaba el servo contra un extremo (hallazgo A4).
             */
            if (failed % 10 == 1) {
                ESP_LOGW(TAG, "el GET falló (%s); se mantiene la última consigna (%d Hz, %d grados)",
                         esp_err_to_name(err), last_freq, servo_get_angle());
            }
            continue;
        }
        ok++;
        kundt_led_set_state(KUNDT_LED_RUNNING);

        if (v.has_frecuencia) {
            const int want = clamp_freq(v.frecuencia);
            if (want != v.frecuencia) {
                ESP_LOGW(TAG, "frecuencia %d Hz fuera de rango, acotada a %d Hz",
                         v.frecuencia, want);
            }
            /* Reprogramar sólo si cambió: el original reescribía el DDS en cada
             * ciclo aunque el valor fuera idéntico. */
            if (want != last_freq) {
                uint32_t actual = 0;
                if (ad9833_set_frequency(s_dds, (uint32_t)want, &actual) == ESP_OK) {
                    last_freq = want;
                    ESP_LOGI(TAG, "frecuencia -> %d Hz (el DDS sintetiza %lu Hz)",
                             want, (unsigned long)actual);
                }
            }
        }

        if (v.has_volumen) {
            /* "volumen" es un ángulo de servo en grados, no un porcentaje (M1). */
            if (v.volumen != servo_get_angle()) {
                if (servo_set_angle(v.volumen) == ESP_OK) {
                    ESP_LOGI(TAG, "volumen -> %d grados", servo_get_angle());
                }
            }
        }

        if ((ok % 30) == 0) {
            ESP_LOGI(TAG, "consultas ok=%lu fallidas=%lu | %d Hz, %d grados | wifi=%s",
                     (unsigned long)ok, (unsigned long)failed,
                     last_freq, servo_get_angle(), kundt_wifi_ip());
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Tubo de Kundt - módulo E2 de generación de audio (ESP-IDF)");

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

    /* Levantar el hardware antes que la red: el tubo debe estar emitiendo tono
     * aunque el backend sea inalcanzable. */
    const ad9833_config_t dds_cfg = AD9833_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(ad9833_init(&dds_cfg, &s_dds));
    ESP_ERROR_CHECK(ad9833_set_waveform(s_dds, AD9833_WAVE_SINE));

    uint32_t actual = 0;
    ESP_ERROR_CHECK(ad9833_set_frequency(s_dds, DEFAULT_FREQ_HZ, &actual));
    ESP_LOGI(TAG, "tono por defecto: %d Hz pedidos, %lu Hz sintetizados",
             DEFAULT_FREQ_HZ, (unsigned long)actual);

#if CONFIG_E2_SELFTEST
    kundt_led_mark_selftest();

    /* Corre antes de servo_init(): LEDC se apropia del pin y esta comprobación
     * necesita manejarlo como GPIO común. */
    selftest_check_jumper();
#endif

    servo_config_t servo_cfg = SERVO_DEFAULT_CONFIG();
#if CONFIG_E2_SELFTEST
    /* Llevar el PWM al pin puenteado con el ADC, para medir el mapeo sin tener
     * un servo conectado. */
    servo_cfg.gpio = CONFIG_E2_SELFTEST_SERVO_GPIO;
#endif
    ESP_ERROR_CHECK(servo_init(&servo_cfg));

#if CONFIG_E2_SELFTEST
    if (selftest_init() == ESP_OK) {
        /* Que falle la comprobación del puente no detiene el barrido: ver las
         * lecturas junto al veredicto informa más que omitirlas. */
        selftest_run_servo_sweep();
    }
#endif

    ESP_ERROR_CHECK(kundt_api_init(cfg.server_ip, API_PORT, cfg.kit));

    ESP_ERROR_CHECK(kundt_wifi_init());
    ESP_ERROR_CHECK(kundt_wifi_connect(cfg.wifi_ssid, cfg.wifi_password));

    kundt_led_set_state(KUNDT_LED_NO_WIFI);
    xTaskCreate(control_task, "e2_control", 4096, NULL, 5, NULL);
}
