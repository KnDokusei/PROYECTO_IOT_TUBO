/*
 * selftest.c - ver selftest.h.
 */

#include "selftest.h"

#if CONFIG_E2_SELFTEST

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "servo.h"

static const char *TAG = "E2-selftest";

/* Una trama de servo dura 20 ms. Muestrear tres da una lectura estable aunque
 * el lazo arranque a mitad de un pulso. */
#define FRAMES_TO_SAMPLE 3
#define SAMPLE_WINDOW_US (FRAMES_TO_SAMPLE * 20000)

/* GPIO34 es sólo de entrada, pero la señal del servo es lógica de 3,3 V y se
 * puede leer digitalmente. Eso importa: adc_oneshot_read() arrastra tanto
 * trabajo del driver (bloqueo, configuración, conversión) que no alcanza a
 * resolver un pulso de 500 us, mientras que gpio_get_level() es una lectura de
 * registro. */
#define PWM_INPUT_GPIO 34

static uint32_t s_samples_taken;


/*
 * Antes de medir nada, demostrar que el puente realmente conduce.
 *
 * GPIO34 es sólo de entrada y no tiene pull interno, así que un pin sin conectar
 * queda al aire y puede arrojar un ciclo de trabajo de aspecto estable que no es
 * más que acoplamiento capacitivo, indistinguible de una medición real si no se
 * comprueba directamente. Llevar el pin de origen a cada riel y leer el de
 * destino lo zanja.
 */
esp_err_t selftest_check_jumper(void)
{
    const gpio_config_t out = {
        .pin_bit_mask = 1ULL << CONFIG_E2_SELFTEST_SERVO_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    const gpio_config_t in = {
        .pin_bit_mask = 1ULL << PWM_INPUT_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&out));
    ESP_ERROR_CHECK(gpio_config(&in));

    gpio_set_level(CONFIG_E2_SELFTEST_SERVO_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    const int read_high = gpio_get_level(PWM_INPUT_GPIO);

    gpio_set_level(CONFIG_E2_SELFTEST_SERVO_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    const int read_low = gpio_get_level(PWM_INPUT_GPIO);

    const bool ok = (read_high == 1 && read_low == 0);

    ESP_LOGI(TAG, "prueba del puente: GPIO%d en alto -> GPIO%d leyó %d; "
                  "en bajo -> leyó %d  %s",
             CONFIG_E2_SELFTEST_SERVO_GPIO, PWM_INPUT_GPIO,
             read_high, read_low,
             ok ? "CONECTADO" : "NO CONECTADO");

    if (!ok) {
        ESP_LOGE(TAG, "falta el puente o no hace contacto.");
        ESP_LOGE(TAG, "sin él GPIO%d queda al aire, y un pin al aire entrega un "
                      "ciclo de trabajo de aspecto estable que es puro ruido "
                      "de acoplamiento.",
                 PWM_INPUT_GPIO);
    }
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t selftest_init(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << PWM_INPUT_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGW(TAG, "AUTOPRUEBA activa: PWM del servo en GPIO%d, lectura en GPIO%d",
             CONFIG_E2_SELFTEST_SERVO_GPIO, PWM_INPUT_GPIO);
    ESP_LOGW(TAG, "AUTOPRUEBA: se requiere puente GPIO%d -> GPIO34",
             CONFIG_E2_SELFTEST_SERVO_GPIO);
    return ESP_OK;
}

/*
 * Mide el tiempo en alto del PWM muestreando el pin lo más rápido posible y
 * contando qué fracción de la ventana lee alto.
 *
 * Lo que se mide es el ciclo de trabajo; el ancho de pulso sale de la trama
 * conocida de 20 ms. Contar el ciclo de trabajo en vez de cronometrar flancos
 * deja la medición inmune al jitter de muestreo de un lazo por sondeo.
 */
static uint32_t measure_pulse_us(void)
{
    const int64_t t0 = esp_timer_get_time();
    uint32_t high = 0, total = 0;

    /* Las interrupciones quedan habilitadas: en este punto WiFi todavía no está
     * arriba, y de todos modos una expropiación ocasional no altera la
     * proporción del ciclo de trabajo. */
    while ((esp_timer_get_time() - t0) < SAMPLE_WINDOW_US) {
        total++;
        if (gpio_get_level(PWM_INPUT_GPIO)) {
            high++;
        }
    }

    s_samples_taken = total;
    if (total == 0) {
        return 0;
    }

    return (uint32_t)(((uint64_t)high * 20000u) / total);
}

esp_err_t selftest_measure_angle(int angle_deg)
{
    esp_err_t err = servo_set_angle(angle_deg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "servo_set_angle(%d): %s", angle_deg, esp_err_to_name(err));
        return err;
    }

    /* Dejar salir un par de tramas antes de medir. */
    vTaskDelay(pdMS_TO_TICKS(100));

    const uint32_t expected = servo_angle_to_pulse_us(angle_deg);
    const uint32_t measured = measure_pulse_us();

    /* Con un lazo de muestreo por lectura de registro la resolución es de unos
     * pocos microsegundos; 100 us de tolerancia cubren el jitter del lazo y la
     * cuantización del ciclo de trabajo de LEDC. */
    const int32_t error = (int32_t)measured - (int32_t)expected;
    const bool    ok    = (error > -100 && error < 100);

    ESP_LOGI(TAG, "%3d grados -> esperado %4lu us, medido %4lu us, error %+5ld us  "
                  "(%lu muestras)  %s",
             angle_deg,
             (unsigned long)expected,
             (unsigned long)measured,
             (long)error,
             (unsigned long)s_samples_taken,
             ok ? "OK" : "FUERA DE TOLERANCIA");

    return ok ? ESP_OK : ESP_FAIL;
}

void selftest_run_servo_sweep(void)
{
    static const int angles[] = { 0, 45, 90, 135, 180, 90, 0 };

    ESP_LOGI(TAG, "--- barrido de PWM del servo (medido a través del puente) ---");

    int passed = 0;
    const int n = (int)(sizeof(angles) / sizeof(angles[0]));
    for (int i = 0; i < n; i++) {
        if (selftest_measure_angle(angles[i]) == ESP_OK) {
            passed++;
        }
    }

    ESP_LOGI(TAG, "--- barrido del servo: %d/%d dentro de tolerancia ---", passed, n);

    /* Los valores fuera de rango deben acotarse, no pasar tal cual (hallazgo M1). */
    ESP_LOGI(TAG, "--- prueba de acotado (hallazgo M1) ---");
    servo_set_angle(544);   /* El valor que Servo::write() malinterpreta como us. */
    ESP_LOGI(TAG, "comandados 544 grados -> el servo reporta %d grados", servo_get_angle());
    servo_set_angle(-90);
    ESP_LOGI(TAG, "comandados -90 grados -> el servo reporta %d grados", servo_get_angle());
    servo_set_angle(90);
}

#endif /* CONFIG_E2_SELFTEST */
