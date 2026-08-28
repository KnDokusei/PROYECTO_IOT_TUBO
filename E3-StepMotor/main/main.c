/*
 * E3-StepMotor - Módulo del motor paso a paso del tubo de Kundt, port a ESP-IDF.
 *
 * Función (según el README del proyecto): mover el émbolo por el riel hasta la
 * posición que pide el servidor, respetando los dos fines de carrera, e
 * informar de vuelta la posición real para que el usuario remoto la vea.
 *
 * Cadena de control: GET posición deseada -> pasos -> A4988 -> motor -> riel,
 *                    y PUT de la posición alcanzada.
 *
 * Portado del sketch de Arduino. Se conservan las funcionalidades; se corrigen
 * los hallazgos de la auditoría que el port permite cerrar, cada uno marcado
 * con su ID.
 */

#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "kundt_api.h"
#include "kundt_config.h"
#include "kundt_led.h"
#include "kundt_wifi.h"
#include "selftest.h"
#include "stepper.h"

static const char *TAG = "E3-StepMotor";

/* El sketch original consultaba cada 500 ms (API_CALL_DELAY_MS). Se mantiene. */
#define POLL_INTERVAL_MS 500

/* Puerto del backend para la API de valores. */
#define API_PORT 5000

/* Cadencia del log de avance. */
#define STATS_INTERVAL_MS 15000

#if CONFIG_E3_EMBOLO_UNIT_CM
#define EMBOLO_UNIT STEPPER_INPUT_CM
#else
#define EMBOLO_UNIT STEPPER_INPUT_MM
#endif

static void control_task(void *arg)
{
    (void)arg;

    int32_t  last_target = INT32_MIN;
    int32_t  last_warned = INT32_MIN; /* evita repetir el aviso de acotado */
    bool     was_moving  = false;
    uint32_t ok = 0, failed = 0, put_failed = 0;
    int64_t  last_stats = 0;

    ESP_LOGI(TAG, "lazo de control iniciado (consulta cada %d ms)", POLL_INTERVAL_MS);

    for (;;) {
        /* Espera real, no `continue` en seco. El sketch original giraba a 100 %
         * de CPU en el núcleo 0 durante los 500 ms de espera, dejando hambrienta
         * a la tarea IDLE que alimenta el watchdog (hallazgo M3). */
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));

        if (!kundt_wifi_is_connected()) {
            /* El README promete que el motor se detiene mientras no haya red. */
            if (stepper_is_moving()) {
                ESP_LOGW(TAG, "sin WiFi: se detiene el motor");
                stepper_stop();
            }
            kundt_led_set_state(KUNDT_LED_NO_WIFI);
            continue;
        }

        kundt_valores_t v;
        const esp_err_t err = kundt_api_get_valores(&v);
        if (err != ESP_OK) {
            failed++;
            kundt_led_set_state(KUNDT_LED_NO_SERVER);
            /* Se conserva la última consigna válida en vez de caer a cero, que
             * es lo que hacía la versión Arduino ante un 404 o un JSON ilegible
             * (hallazgo A4). Aquí caer a cero significaría mandar el émbolo
             * contra el tope. */
            if (failed % 20 == 1) {
                /* Antes de la primera consigna válida no hay nada que mantener:
                 * imprimir last_target daría INT32_MIN convertido a cm. */
                if (last_target == INT32_MIN) {
                    ESP_LOGW(TAG, "el GET falló (%s); aún sin consigna del servidor",
                             esp_err_to_name(err));
                } else {
                    ESP_LOGW(TAG, "el GET falló (%s); se mantiene la consigna (%.2f cm)",
                             esp_err_to_name(err),
                             (double)stepper_steps_to_cm(last_target));
                }
            }
            continue;
        }
        ok++;
        kundt_led_set_state(KUNDT_LED_RUNNING);

        if (v.has_embolo) {
            const float   want_cm = stepper_input_to_cm(v.embolo, EMBOLO_UNIT);
            const int32_t want    = stepper_clamp_steps(stepper_cm_to_steps(want_cm),
                                                        stepper_neg_limit_steps(),
                                                        stepper_pos_limit_steps());

            /* El aviso se emite al cambiar la petición, no en cada consulta:
             * un backend que insiste en un valor fuera de rango llenaría el log
             * dos veces por segundo sin aportar nada nuevo. */
            if (!stepper_cm_in_range(want_cm) && want != last_warned) {
                ESP_LOGW(TAG, "posición %.2f cm fuera del riel [%.1f, %.1f], acotada a %.2f cm",
                         (double)want_cm,
                         (double)STEPPER_NEG_LIMIT_CM, (double)STEPPER_POS_LIMIT_CM,
                         (double)stepper_steps_to_cm(want));
                last_warned = want;
            }

            if (want != last_target) {
                if (stepper_move_to(want) == ESP_OK) {
                    ESP_LOGI(TAG, "destino -> %.2f cm (%ld pasos), desde %.2f cm",
                             (double)stepper_steps_to_cm(want), (long)want,
                             (double)stepper_steps_to_cm(stepper_position()));
                    last_target = want;
                }
            }
        }

        /*
         * Telemetría. Se informa mientras el motor se mueve y una vez más al
         * detenerse, que es la intención del sketch original (evitar PUT
         * inútiles con el motor parado) sin su máquina de estados de conexión.
         *
         * La diferencia de fondo está en el valor enviado: aquí es la posición
         * real del contador de pasos, con resolución de 0,004 cm. El sketch
         * enviaba una variable que sólo se refrescaba dentro de printPosition()
         * y sólo tras media vuelta del motor, o sea 0,4 cm: cuatro veces peor
         * que los 0,1 cm que promete el README (hallazgo A2).
         */
        const bool moving = stepper_is_moving();
        if (moving || was_moving) {
            const float pos_cm = stepper_steps_to_cm(stepper_position());
            if (kundt_api_put_posicion(pos_cm) != ESP_OK) {
                put_failed++;
            }
        }
        was_moving = moving;

        const int64_t now = esp_timer_get_time() / 1000;
        if (now - last_stats >= STATS_INTERVAL_MS) {
            last_stats = now;

            stepper_stats_t st;
            stepper_get_stats(&st);
            const stepper_endstops_t es = stepper_read_endstops();

            ESP_LOGI(TAG, "POS  %.2f cm -> %.2f cm  %s  pasos=%llu topes=%lu",
                     (double)stepper_steps_to_cm(st.position_steps),
                     (double)stepper_steps_to_cm(st.target_steps),
                     st.moving ? "en marcha" : "detenido",
                     (unsigned long long)st.steps_emitted,
                     (unsigned long)st.endstop_stops);
            ESP_LOGI(TAG, "RED  consultas ok=%lu fallidas=%lu put_fallidos=%lu wifi=%s",
                     (unsigned long)ok, (unsigned long)failed,
                     (unsigned long)put_failed, kundt_wifi_ip());
            ESP_LOGI(TAG, "FIN  carrera -: %s   carrera +: %s   calibrado: %s",
                     es.neg_pressed ? "PULSADO" : "libre",
                     es.pos_pressed ? "PULSADO" : "libre",
                     st.calibrated ? "sí" : "NO");
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Tubo de Kundt - módulo E3 de motor paso a paso (ESP-IDF)");

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

    ESP_LOGI(TAG, "unidad supuesta para 'embolo': %s (hallazgo A3)",
             EMBOLO_UNIT == STEPPER_INPUT_MM ? "milímetros" : "centímetros");

    stepper_config_t st_cfg  = STEPPER_DEFAULT_CONFIG();
    st_cfg.speed_sps         = CONFIG_E3_SPEED_SPS;
    st_cfg.calib_speed_sps   = CONFIG_E3_CALIB_SPEED_SPS;
    ESP_ERROR_CHECK(stepper_init(&st_cfg));

#if CONFIG_E3_SELFTEST_ENDSTOP_SIM
    /* Antes de leer los fines de carrera por primera vez, para que el estado
     * registrado al arrancar sea el simulado y no el de unos pines al aire. */
    selftest_endstop_sim_init();
#endif

    /* Estado de los fines de carrera antes de mover nada: si ambos aparecen
     * pulsados con el émbolo a media carrera, faltan los pull-ups externos.
     * GPIO34/35 no tienen pull interno y al aire dan lecturas engañosas. */
    const stepper_endstops_t es = stepper_read_endstops();
    ESP_LOGI(TAG, "fines de carrera al arrancar: - %s, + %s",
             es.neg_pressed ? "PULSADO" : "libre",
             es.pos_pressed ? "PULSADO" : "libre");

    ESP_ERROR_CHECK(kundt_api_init(cfg.server_ip, API_PORT, cfg.kit));

    ESP_ERROR_CHECK(kundt_wifi_init());
    ESP_ERROR_CHECK(kundt_wifi_connect(cfg.wifi_ssid, cfg.wifi_password));
    kundt_led_set_state(KUNDT_LED_NO_WIFI);

#if CONFIG_E3_SELFTEST
    kundt_led_mark_selftest();
    /* Corre antes de la calibración: comprueba el generador de pasos contra el
     * contador interno del chip y, de paso, que la calibración falle sin
     * hardware en vez de colgarse. */
    selftest_run();
#endif

#if CONFIG_E3_SKIP_CALIBRATION
    kundt_led_mark_selftest();
    ESP_LOGW(TAG, "CALIBRACIÓN OMITIDA por configuración: la posición informada");
    ESP_LOGW(TAG, "no tiene referencia física. Sólo para pruebas de banco.");
#else
    /* La calibración no necesita red: se hace apenas el hardware está listo,
     * para que el riel quede referenciado cuanto antes. */
    const esp_err_t cal = stepper_calibrate();
    if (cal != ESP_OK) {
        ESP_LOGE(TAG, "la calibración falló (%s).", esp_err_to_name(cal));
        ESP_LOGE(TAG, "El módulo sigue en marcha para poder diagnosticar por red,");
        ESP_LOGE(TAG, "pero NO moverá el motor: sin referencia, cualquier destino");
        ESP_LOGE(TAG, "es una orden a ciegas contra los topes del riel.");
        kundt_led_set_state(KUNDT_LED_NO_SERVER);
        /* Se deja el módulo vivo pero quieto: informar la posición de un riel
         * sin referenciar sería peor que no informar nada. */
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
#endif

    xTaskCreate(control_task, "e3_control", 4096, NULL, 5, NULL);
}
