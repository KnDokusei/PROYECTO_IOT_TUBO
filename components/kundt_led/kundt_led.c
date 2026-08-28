/*
 * kundt_led.c - ver kundt_led.h.
 */

#include "kundt_led.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "kundt_led";

static int               s_gpio  = KUNDT_LED_DEFAULT_GPIO;
static kundt_led_state_t s_state = KUNDT_LED_BOOT;
static bool              s_selftest;

static void blink(int on_ms, int off_ms)
{
    gpio_set_level(s_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(on_ms));
    gpio_set_level(s_gpio, 0);
    vTaskDelay(pdMS_TO_TICKS(off_ms));
}

static void led_task(void *arg)
{
    (void)arg;

    for (;;) {
        if (s_selftest) {
            /* Tres parpadeos rápidos antes del patrón normal: esta compilación
             * no está cableada como espera el esquemático. */
            blink(60, 60);
            blink(60, 60);
            blink(60, 300);
        }

        switch (s_state) {
        case KUNDT_LED_BOOT:
            blink(50, 50);
            break;
        case KUNDT_LED_RUNNING:
            blink(100, 100);
            break;
        case KUNDT_LED_NO_SERVER:
            blink(80, 120);
            blink(80, 1000);
            break;
        case KUNDT_LED_NO_WIFI:
        default:
            blink(100, 1900);
            break;
        }
    }
}

esp_err_t kundt_led_init(int gpio)
{
    s_gpio = gpio;

    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config: %s", esp_err_to_name(err));
        return err;
    }

    if (xTaskCreate(led_task, "kundt_led", 2560, NULL, 2, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "LED de estado en GPIO%d", gpio);
    return ESP_OK;
}

void kundt_led_mark_selftest(void)
{
    s_selftest = true;
    ESP_LOGW(TAG, "COMPILACIÓN DE AUTOPRUEBA - los pines difieren del "
                  "esquemático, no instalar esto en un equipo");
}

void kundt_led_set_state(kundt_led_state_t state)
{
    if (state != s_state) {
        ESP_LOGI(TAG, "estado -> %s", kundt_led_state_name(state));
        s_state = state;
    }
}

kundt_led_state_t kundt_led_get_state(void)
{
    return s_state;
}

const char *kundt_led_state_name(kundt_led_state_t state)
{
    switch (state) {
    case KUNDT_LED_BOOT:       return "ARRANQUE (titileo rápido)";
    case KUNDT_LED_NO_WIFI:    return "SIN WIFI (parpadeo lento)";
    case KUNDT_LED_NO_SERVER:  return "SIN SERVIDOR (parpadeo doble)";
    case KUNDT_LED_RUNNING:    return "EN MARCHA (parpadeo parejo)";
    case KUNDT_LED_SELFTEST:   return "AUTOPRUEBA (parpadeo triple)";
    default:                   return "desconocido";
    }
}
