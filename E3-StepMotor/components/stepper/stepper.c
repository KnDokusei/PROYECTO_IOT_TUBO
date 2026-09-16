/*
 * stepper.c - ver stepper.h.
 */

#include "stepper.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "stepper";

/* A4988: STEP admite pulsos desde 1 µs; se usan 2 por margen. */
#define STEP_PULSE_US 2
/* DIR estable antes del flanco de subida de STEP. */
#define DIR_SETUP_US 1
/* A4988: 1 ms tras SLEEP en alto para el charge pump. Con esp_rom_delay_us porque
 * el tick de FreeRTOS es de 10 ms y pdMS_TO_TICKS(1) daría cero. */
#define A4988_WAKE_US 1200

static struct {
    gptimer_handle_t timer;
    stepper_config_t cfg;
    int32_t          position;
    int32_t          target;
    bool             moving;
    int              hit; /* switch que detuvo el último movimiento */
    int              last_dir;
} s;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static inline bool IRAM_ATTR pressed(int gpio)
{
    return gpio_get_level(gpio) == 0;
}

static void IRAM_ATTR set_dir(int dir)
{
    int level = (dir == STEPPER_IZQ) ? 1 : 0;
    if (s.cfg.invert_dir) {
        level = !level;
    }
    gpio_set_level(s.cfg.dir_gpio, level);
}

static void IRAM_ATTR pulse_step(void)
{
    gpio_set_level(s.cfg.step_gpio, 1);
    esp_rom_delay_us(STEP_PULSE_US);
    gpio_set_level(s.cfg.step_gpio, 0);
}

/* Duerme el driver al parar para que el motor no se caliente. Sin corriente no hay
 * par de retención: el émbolo se puede mover a mano sin que el firmware se entere. */
static void IRAM_ATTR finish_move(void)
{
    s.moving = false;
    gpio_set_level(s.cfg.sleep_gpio, 0);
}

/* Un paso por alarma. El GPTimer corre siempre: pararlo desde la ISR no es seguro
 * y una alarma vacía cuesta unos microsegundos. */
static bool IRAM_ATTR on_alarm(gptimer_handle_t timer,
                               const gptimer_alarm_event_data_t *edata,
                               void *arg)
{
    (void)timer;
    (void)edata;
    (void)arg;

    portENTER_CRITICAL_ISR(&s_mux);

    if (!s.moving) {
        portEXIT_CRITICAL_ISR(&s_mux);
        return false;
    }

    if (s.position == s.target) {
        finish_move();
        portEXIT_CRITICAL_ISR(&s_mux);
        return false;
    }

    const int dir = (s.target > s.position) ? STEPPER_IZQ : STEPPER_DER;
    const int sw  = (dir == STEPPER_IZQ) ? s.cfg.sw_izq_gpio : s.cfg.sw_der_gpio;

    /* En cada paso, no cada tantos ms: el switch es el único tope físico. */
    if (pressed(sw)) {
        s.hit = dir;
        finish_move();
        portEXIT_CRITICAL_ISR(&s_mux);
        return false;
    }

    if (dir != s.last_dir) {
        set_dir(dir);
        s.last_dir = dir;
        esp_rom_delay_us(DIR_SETUP_US);
    }

    pulse_step();
    s.position += dir;

    portEXIT_CRITICAL_ISR(&s_mux);
    return false;
}

esp_err_t stepper_set_speed(uint32_t steps_per_sec)
{
    /* El temporizador cuenta a 1 MHz: la cuenta de alarma es el período en µs. */
    static gptimer_alarm_config_t alarm;
    alarm.alarm_count                = 1000000ULL / steps_per_sec;
    alarm.reload_count               = 0;
    alarm.flags.auto_reload_on_alarm = true;
    return gptimer_set_alarm_action(s.timer, &alarm);
}

esp_err_t stepper_init(const stepper_config_t *cfg, uint32_t steps_per_sec)
{
    memset(&s, 0, sizeof(s));
    s.cfg = *cfg;

    const gpio_config_t out = {
        .pin_bit_mask = (1ULL << cfg->step_gpio) | (1ULL << cfg->dir_gpio) |
                        (1ULL << cfg->sleep_gpio),
        .mode         = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&out), TAG, "gpio_config de salidas");

    /* GPIO34/35 no admiten pull interno: se configuran sin pull a propósito. */
    const gpio_config_t in = {
        .pin_bit_mask = (1ULL << cfg->sw_izq_gpio) | (1ULL << cfg->sw_der_gpio),
        .mode         = GPIO_MODE_INPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&in), TAG, "gpio_config de switches");

    gpio_set_level(cfg->sleep_gpio, 0); /* arrancar con el motor dormido */
    gpio_set_level(cfg->step_gpio, 0);

    const gptimer_config_t tcfg = {
        .clk_src       = GPTIMER_CLK_SRC_DEFAULT,
        .direction     = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000, /* 1 tick = 1 µs */
    };
    ESP_RETURN_ON_ERROR(gptimer_new_timer(&tcfg, &s.timer), TAG, "gptimer_new_timer");

    const gptimer_event_callbacks_t cbs = { .on_alarm = on_alarm };
    ESP_RETURN_ON_ERROR(gptimer_register_event_callbacks(s.timer, &cbs, NULL), TAG, "callbacks");
    ESP_RETURN_ON_ERROR(stepper_set_speed(steps_per_sec), TAG, "velocidad");
    ESP_RETURN_ON_ERROR(gptimer_enable(s.timer), TAG, "gptimer_enable");
    ESP_RETURN_ON_ERROR(gptimer_start(s.timer), TAG, "gptimer_start");

    ESP_LOGI(TAG, "listo: STEP=%d DIR=%d SLEEP=%d, SW izq=GPIO%d, SW der=GPIO%d",
             cfg->step_gpio, cfg->dir_gpio, cfg->sleep_gpio, cfg->sw_izq_gpio, cfg->sw_der_gpio);
    return ESP_OK;
}

void stepper_move_to(int32_t target)
{
    portENTER_CRITICAL(&s_mux);
    s.target = target;
    s.hit    = 0;
    portEXIT_CRITICAL(&s_mux);

    /* Despertar el driver y esperar el charge pump ANTES de habilitar los pasos:
     * si no, se pierden los primeros de cada arranque (hallazgo M2). */
    gpio_set_level(s.cfg.sleep_gpio, 1);
    esp_rom_delay_us(A4988_WAKE_US);

    portENTER_CRITICAL(&s_mux);
    s.moving = true;
    portEXIT_CRITICAL(&s_mux);
}

void stepper_stop(void)
{
    portENTER_CRITICAL(&s_mux);
    s.target = s.position;
    s.moving = false;
    portEXIT_CRITICAL(&s_mux);
    gpio_set_level(s.cfg.sleep_gpio, 0);
}

int32_t stepper_position(void)
{
    portENTER_CRITICAL(&s_mux);
    const int32_t p = s.position;
    portEXIT_CRITICAL(&s_mux);
    return p;
}

void stepper_set_position(int32_t steps)
{
    portENTER_CRITICAL(&s_mux);
    s.position = steps;
    s.target   = steps;
    s.moving   = false;
    portEXIT_CRITICAL(&s_mux);
    gpio_set_level(s.cfg.sleep_gpio, 0);
}

bool stepper_is_moving(void)
{
    portENTER_CRITICAL(&s_mux);
    const bool m = s.moving;
    portEXIT_CRITICAL(&s_mux);
    return m;
}

int stepper_switch_hit(void)
{
    portENTER_CRITICAL(&s_mux);
    const int h = s.hit;
    portEXIT_CRITICAL(&s_mux);
    return h;
}

void stepper_read_switches(bool *izq, bool *der)
{
    *izq = pressed(s.cfg.sw_izq_gpio);
    *der = pressed(s.cfg.sw_der_gpio);
}
