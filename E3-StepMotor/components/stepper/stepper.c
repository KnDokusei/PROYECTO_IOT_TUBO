/*
 * stepper.c - ver stepper.h.
 */

#include "stepper.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "stepper";

/* Datasheet del A4988: STEP admite pulsos desde 1 us; se usan 2 por margen. */
#define STEP_PULSE_US 2
/* Margen para que DIR quede estable antes del flanco de subida de STEP. */
#define DIR_SETUP_US 1
/* Datasheet del A4988: 1 ms tras SLEEP alto para que estabilice el charge pump.
 * Se aplica con esp_rom_delay_us y no con vTaskDelay porque el tick por defecto
 * de FreeRTOS es de 10 ms: pdMS_TO_TICKS(1) daría cero y no esperaría nada. */
#define A4988_WAKE_US 1200

/* Margen sobre el tiempo teórico de recorrido antes de declarar fallida la
 * calibración, en porcentaje. Se expresa así y no como "3 / 2" porque una
 * macro con un operador suelto cambia de significado según el lado en que se
 * use en la multiplicación. */
#define CALIB_TIMEOUT_PCT 150

static struct {
    gptimer_handle_t timer;
    stepper_config_t cfg;
    int32_t          min_steps;
    int32_t          max_steps;
    int32_t          position;
    int32_t          target;
    bool             moving;
    bool             calibrated;
    uint32_t         endstop_stops;
    uint64_t         steps_emitted;
    int              last_dir;
} s;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* Fines de carrera activos en bajo: switch a GND con pull-up externo. */
static inline bool IRAM_ATTR endstop_pressed(int gpio)
{
    return gpio_get_level(gpio) == 0;
}

/* dir > 0 mueve hacia el extremo lejano al parlante. */
static void IRAM_ATTR set_dir(int dir)
{
    int level = (dir > 0) ? 1 : 0;
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

/* Deja el movimiento terminado y duerme el driver, como hacía el sketch
 * original para que el motor no se caliente parado. Efecto secundario que el
 * README ya advierte: sin corriente no hay par de retención y el riel se puede
 * mover a mano sin que el firmware se entere. */
static void IRAM_ATTR finish_move(void)
{
    s.moving = false;
    gpio_set_level(s.cfg.sleep_gpio, 0);
}

/*
 * Un paso por alarma. El temporizador queda corriendo siempre: parar y arrancar
 * el GPTimer desde la ISR no es seguro, y una alarma vacía cuesta unos pocos
 * microsegundos. A 500 pasos/s eso es holgadamente menos del 1 % de CPU, a
 * cambio de que no haya carrera alguna entre arranque y parada.
 */
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

    const int dir = (s.target > s.position) ? 1 : -1;

    /* Los fines de carrera se comprueban en cada paso, no cada varios
     * milisegundos: son el único límite físico y llegar tarde significa empujar
     * el riel contra el tope. Al tocar uno, esa posición pasa a ser la verdad
     * y se recalibra sobre ella. */
    if (dir > 0 && endstop_pressed(s.cfg.endstop_pos_gpio)) {
        s.position = s.max_steps;
        s.target   = s.position;
        s.endstop_stops++;
        finish_move();
        portEXIT_CRITICAL_ISR(&s_mux);
        return false;
    }
    if (dir < 0 && endstop_pressed(s.cfg.endstop_neg_gpio)) {
        s.position = s.min_steps;
        s.target   = s.position;
        s.endstop_stops++;
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
    s.steps_emitted++;

    portEXIT_CRITICAL_ISR(&s_mux);
    return false;
}

static esp_err_t set_speed(uint32_t sps)
{
    if (sps == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    /* El temporizador cuenta a 1 MHz, así que la cuenta de alarma es el período
     * en microsegundos. */
    static gptimer_alarm_config_t alarm;
    alarm.alarm_count                = 1000000ULL / sps;
    alarm.reload_count               = 0;
    alarm.flags.auto_reload_on_alarm = true;
    return gptimer_set_alarm_action(s.timer, &alarm);
}

esp_err_t stepper_init(const stepper_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s, 0, sizeof(s));
    s.cfg       = *cfg;
    s.min_steps = stepper_neg_limit_steps();
    s.max_steps = stepper_pos_limit_steps();
    s.last_dir  = 0;

    const gpio_config_t out = {
        .pin_bit_mask = (1ULL << cfg->step_gpio) | (1ULL << cfg->dir_gpio) |
                        (1ULL << cfg->sleep_gpio),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&out);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config de salidas: %s", esp_err_to_name(err));
        return err;
    }

    /* GPIO34/35 son sólo de entrada y no admiten pull interno: el pull-up es
     * obligatoriamente externo. Se configuran sin pull para dejarlo explícito. */
    const gpio_config_t in = {
        .pin_bit_mask = (1ULL << cfg->endstop_pos_gpio) | (1ULL << cfg->endstop_neg_gpio),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&in);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config de fines de carrera: %s", esp_err_to_name(err));
        return err;
    }

    gpio_set_level(cfg->sleep_gpio, 0); /* arrancar con el motor dormido */
    gpio_set_level(cfg->step_gpio, 0);

    const gptimer_config_t tcfg = {
        .clk_src       = GPTIMER_CLK_SRC_DEFAULT,
        .direction     = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000, /* 1 tick = 1 us */
    };
    err = gptimer_new_timer(&tcfg, &s.timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gptimer_new_timer: %s", esp_err_to_name(err));
        return err;
    }

    const gptimer_event_callbacks_t cbs = { .on_alarm = on_alarm };
    err = gptimer_register_event_callbacks(s.timer, &cbs, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gptimer_register_event_callbacks: %s", esp_err_to_name(err));
        return err;
    }

    err = set_speed(cfg->speed_sps);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_speed: %s", esp_err_to_name(err));
        return err;
    }

    err = gptimer_enable(s.timer);
    if (err == ESP_OK) {
        err = gptimer_start(s.timer);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "arranque del temporizador: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG,
             "listo: STEP=%d DIR=%d SLEEP=%d, fines %d/%d, riel %.1f-%.1f cm "
             "(%ld pasos), %lu pasos/s",
             cfg->step_gpio, cfg->dir_gpio, cfg->sleep_gpio,
             cfg->endstop_neg_gpio, cfg->endstop_pos_gpio,
             (double)STEPPER_NEG_LIMIT_CM, (double)STEPPER_POS_LIMIT_CM,
             (long)(s.max_steps - s.min_steps), (unsigned long)cfg->speed_sps);
    return ESP_OK;
}

stepper_endstops_t stepper_read_endstops(void)
{
    stepper_endstops_t es = {
        .pos_pressed = endstop_pressed(s.cfg.endstop_pos_gpio),
        .neg_pressed = endstop_pressed(s.cfg.endstop_neg_gpio),
    };
    return es;
}

esp_err_t stepper_move_to(int32_t target_steps)
{
    /*
     * Los dos fines de carrera activos a la vez es físicamente imposible en un
     * riel real: significa cableado roto o pull-ups ausentes. Sin esta guardia,
     * la ISR trataría el primer paso como llegada a un extremo y "recalibraría"
     * la posición saltando a ese límite, con lo que el émbolo quedaría dando
     * tumbos entre 26 y 84 cm sin moverse. Mejor negarse a mover.
     */
    const stepper_endstops_t es = stepper_read_endstops();
    if (es.pos_pressed && es.neg_pressed) {
        static bool avisado;
        if (!avisado) {
            avisado = true;
            ESP_LOGE(TAG, "ambos fines de carrera activos: no se mueve el motor.");
            ESP_LOGE(TAG, "revisa los pull-ups externos de GPIO%d y GPIO%d.",
                     s.cfg.endstop_neg_gpio, s.cfg.endstop_pos_gpio);
        }
        return ESP_ERR_INVALID_STATE;
    }

    target_steps = stepper_clamp_steps(target_steps, s.min_steps, s.max_steps);

    portENTER_CRITICAL(&s_mux);
    s.target          = target_steps;
    const bool arrived = (s.position == target_steps);
    portEXIT_CRITICAL(&s_mux);

    if (arrived) {
        stepper_stop();
        return ESP_OK;
    }

    /* Despertar el driver y esperar el charge pump ANTES de habilitar los
     * pasos. El sketch original ponía SLEEP en alto y daba pasos en la misma
     * iteración, así que los primeros de cada arranque se perdían: deriva
     * acumulativa que descalibraba el riel en silencio (hallazgo M2). */
    gpio_set_level(s.cfg.sleep_gpio, 1);
    esp_rom_delay_us(A4988_WAKE_US);

    portENTER_CRITICAL(&s_mux);
    s.moving = true;
    portEXIT_CRITICAL(&s_mux);
    return ESP_OK;
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

void stepper_get_stats(stepper_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_mux);
    out->position_steps = s.position;
    out->target_steps   = s.target;
    out->moving         = s.moving;
    out->calibrated     = s.calibrated;
    out->endstop_stops  = s.endstop_stops;
    out->steps_emitted  = s.steps_emitted;
    portEXIT_CRITICAL(&s_mux);
}

esp_err_t stepper_calibrate(void)
{
    const stepper_endstops_t es = stepper_read_endstops();
    if (es.pos_pressed && es.neg_pressed) {
        ESP_LOGE(TAG, "ambos fines de carrera dan pulsado a la vez.");
        ESP_LOGE(TAG, "GPIO%d y GPIO%d no tienen pull interno: revisa los "
                      "pull-ups externos a 3V3 antes de mover el motor.",
                 s.cfg.endstop_neg_gpio, s.cfg.endstop_pos_gpio);
        return ESP_ERR_INVALID_STATE;
    }

    const int32_t travel = s.max_steps - s.min_steps;

    /* Se parte suponiendo el émbolo en el extremo lejano, que es el caso peor:
     * así el recorrido comandado cubre el riel completo venga de donde venga. */
    portENTER_CRITICAL(&s_mux);
    s.position              = s.max_steps;
    const uint32_t hits_before = s.endstop_stops;
    portEXIT_CRITICAL(&s_mux);

    ESP_LOGI(TAG, "calibrando: buscando el fin de carrera de %.1f cm...",
             (double)STEPPER_NEG_LIMIT_CM);

    esp_err_t err = set_speed(s.cfg.calib_speed_sps);
    if (err != ESP_OK) {
        return err;
    }

    err = stepper_move_to(s.min_steps);
    if (err != ESP_OK) {
        return err;
    }

    /* Presupuesto de tiempo en vez del `while (true)` sin salida del sketch
     * original: si el switch no responde se aborta en vez de empujar el riel
     * contra el tope indefinidamente (hallazgo B2). */
    const int64_t budget_us = (int64_t)travel * 1000000 / s.cfg.calib_speed_sps
                              * CALIB_TIMEOUT_PCT / 100;
    const int64_t deadline = esp_timer_get_time() + budget_us;

    while (stepper_is_moving()) {
        if (esp_timer_get_time() > deadline) {
            stepper_stop();
            ESP_LOGE(TAG, "la calibración excedió %lld s sin alcanzar el fin de carrera",
                     (long long)(budget_us / 1000000));
            set_speed(s.cfg.speed_sps);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    set_speed(s.cfg.speed_sps);

    portENTER_CRITICAL(&s_mux);
    const bool by_endstop = (s.endstop_stops != hits_before);
    if (by_endstop) {
        s.calibrated = true;
    }
    portEXIT_CRITICAL(&s_mux);

    if (!by_endstop) {
        /* Recorrió el riel entero sin que el switch se activara nunca. */
        ESP_LOGE(TAG, "recorrido completo sin detectar el fin de carrera: "
                      "revisa el cableado de GPIO%d", s.cfg.endstop_neg_gpio);
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "calibrado: origen en %.2f cm", (double)STEPPER_NEG_LIMIT_CM);
    return ESP_OK;
}
