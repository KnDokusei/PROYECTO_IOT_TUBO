/*
 * E2-SineGen - Kundt tube audio generation module, ESP-IDF port.
 *
 * Role (per the project README): generate the signal that drives the speaker
 * at one end of the tube. The frequency comes from the backend and is
 * synthesised by an AD9833 DDS; the volume also comes from the backend and is
 * set by a servo that turns the potentiometer at the output of the audio
 * amplifier.
 *
 * Signal path: AD9833 (SPI) -> LM386 amplifier -> potentiometer (servo) -> speaker
 *
 * Ported from the Arduino sketch. Behaviour is deliberately unchanged except
 * where the original was wrong; those cases are marked with their audit ID.
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

/* The original sketch polled every 2 s. Kept: the backend is shared with the
 * other modules and there is no reason to poll it harder. */
#define POLL_INTERVAL_MS 2000

/* Backend port for the values API. */
#define API_PORT 5000

/* Frequency the AD9833 starts at, matching the Arduino default. */
#define DEFAULT_FREQ_HZ 1200

/*
 * Audio band limits. The tube is a physics experiment, not an arbitrary signal
 * source: a value outside this range is a backend error, not a request. The
 * Arduino build passed anything straight through, and a failed JSON parse in
 * particular produced 0 Hz (finding A4).
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

    ESP_LOGI(TAG, "control loop started (poll every %d ms)", POLL_INTERVAL_MS);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));

        if (!kundt_wifi_is_connected()) {
            kundt_led_set_state(KUNDT_LED_NO_WIFI);
            continue;  /* The WiFi component retries on its own. */
        }

        kundt_valores_t v;
        const esp_err_t err = kundt_api_get_valores(&v);
        if (err != ESP_OK) {
            failed++;
            kundt_led_set_state(KUNDT_LED_NO_SERVER);
            /*
             * Hold the last good setpoint rather than falling back to zero.
             * In the Arduino build a bad response silently became 0 Hz and 0
             * degrees, so a backend hiccup killed the tone and slammed the
             * servo to one end (finding A4).
             */
            if (failed % 10 == 1) {
                ESP_LOGW(TAG, "GET failed (%s); holding last setpoint (%d Hz, %d deg)",
                         esp_err_to_name(err), last_freq, servo_get_angle());
            }
            continue;
        }
        ok++;
        kundt_led_set_state(KUNDT_LED_RUNNING);

        if (v.has_frecuencia) {
            const int want = clamp_freq(v.frecuencia);
            if (want != v.frecuencia) {
                ESP_LOGW(TAG, "frequency %d Hz out of range, clamped to %d Hz",
                         v.frecuencia, want);
            }
            /* Only reprogram on change: the original rewrote the DDS every
             * cycle even when the value was identical. */
            if (want != last_freq) {
                uint32_t actual = 0;
                if (ad9833_set_frequency(s_dds, (uint32_t)want, &actual) == ESP_OK) {
                    last_freq = want;
                    ESP_LOGI(TAG, "frequency -> %d Hz (DDS resolves to %lu Hz)",
                             want, (unsigned long)actual);
                }
            }
        }

        if (v.has_volumen) {
            /* "volumen" is a servo angle in degrees, not a percentage (M1). */
            if (v.volumen != servo_get_angle()) {
                if (servo_set_angle(v.volumen) == ESP_OK) {
                    ESP_LOGI(TAG, "volume -> %d deg", servo_get_angle());
                }
            }
        }

        if ((ok % 30) == 0) {
            ESP_LOGI(TAG, "polls ok=%lu failed=%lu | %d Hz, %d deg | wifi=%s",
                     (unsigned long)ok, (unsigned long)failed,
                     last_freq, servo_get_angle(), kundt_wifi_ip());
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Kundt tube - E2 audio generation module (ESP-IDF)");

    /* Started first so the LED reports progress even if provisioning fails. */
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

    /* Bring the hardware up before the network: the tube should be producing a
     * tone even if the backend is unreachable. */
    const ad9833_config_t dds_cfg = AD9833_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(ad9833_init(&dds_cfg, &s_dds));
    ESP_ERROR_CHECK(ad9833_set_waveform(s_dds, AD9833_WAVE_SINE));

    uint32_t actual = 0;
    ESP_ERROR_CHECK(ad9833_set_frequency(s_dds, DEFAULT_FREQ_HZ, &actual));
    ESP_LOGI(TAG, "default tone: %d Hz requested, %lu Hz synthesised",
             DEFAULT_FREQ_HZ, (unsigned long)actual);

#if CONFIG_E2_SELFTEST
    kundt_led_mark_selftest();

    /* Runs before servo_init(): LEDC claims the pin and the check needs to
     * drive it as a plain GPIO. */
    selftest_check_jumper();
#endif

    servo_config_t servo_cfg = SERVO_DEFAULT_CONFIG();
#if CONFIG_E2_SELFTEST
    /* Route the PWM to the pin that is jumpered to the ADC, so the mapping can
     * be measured without a servo attached. */
    servo_cfg.gpio = CONFIG_E2_SELFTEST_SERVO_GPIO;
#endif
    ESP_ERROR_CHECK(servo_init(&servo_cfg));

#if CONFIG_E2_SELFTEST
    if (selftest_init() == ESP_OK) {
        /* A failed jumper check does not stop the sweep: seeing the readings
         * next to the verdict is more informative than skipping them. */
        selftest_run_servo_sweep();
    }
#endif

    ESP_ERROR_CHECK(kundt_api_init(cfg.server_ip, API_PORT, cfg.kit));

    ESP_ERROR_CHECK(kundt_wifi_init());
    ESP_ERROR_CHECK(kundt_wifi_connect(cfg.wifi_ssid, cfg.wifi_password));

    kundt_led_set_state(KUNDT_LED_NO_WIFI);
    xTaskCreate(control_task, "e2_control", 4096, NULL, 5, NULL);
}
