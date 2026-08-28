/*
 * servo.c - ver servo.h.
 */

#include "servo.h"

#include "driver/ledc.h"
#include "esp_log.h"

static const char *TAG = "servo";

#define SERVO_LEDC_TIMER   LEDC_TIMER_0
#define SERVO_LEDC_CHANNEL LEDC_CHANNEL_0
#define SERVO_LEDC_MODE    LEDC_LOW_SPEED_MODE

static int  s_angle = -1;   /* -1 == nunca se ha escrito */
static bool s_ready;

esp_err_t servo_init(const servo_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const ledc_timer_config_t timer = {
        .speed_mode      = SERVO_LEDC_MODE,
        .duty_resolution = SERVO_DUTY_RES_BITS,
        .timer_num       = SERVO_LEDC_TIMER,
        .freq_hz         = SERVO_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config: %s", esp_err_to_name(err));
        return err;
    }

    const ledc_channel_config_t ch = {
        .gpio_num   = cfg->gpio,
        .speed_mode = SERVO_LEDC_MODE,
        .channel    = SERVO_LEDC_CHANNEL,
        .timer_sel  = SERVO_LEDC_TIMER,
        .duty       = servo_angle_to_duty(cfg->initial_angle),
        .hpoint     = 0,
    };
    err = ledc_channel_config(&ch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config: %s", esp_err_to_name(err));
        return err;
    }

    s_ready = true;
    s_angle = servo_clamp_angle(cfg->initial_angle);

    ESP_LOGI(TAG, "listo en GPIO%d a %d grados (%lu us)",
             cfg->gpio, s_angle, (unsigned long)servo_angle_to_pulse_us(s_angle));
    return ESP_OK;
}

esp_err_t servo_set_angle(int angle_deg)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    const int angle = servo_clamp_angle(angle_deg);
    if (angle != angle_deg) {
        ESP_LOGW(TAG, "ángulo %d fuera de rango, acotado a %d", angle_deg, angle);
    }
    if (angle == s_angle) {
        return ESP_OK;  /* Nada que hacer; evita mover el servo sin necesidad. */
    }

    const uint32_t duty = servo_angle_to_duty(angle);
    esp_err_t err = ledc_set_duty(SERVO_LEDC_MODE, SERVO_LEDC_CHANNEL, duty);
    if (err == ESP_OK) {
        err = ledc_update_duty(SERVO_LEDC_MODE, SERVO_LEDC_CHANNEL);
    }
    if (err == ESP_OK) {
        s_angle = angle;
    }
    return err;
}

int servo_get_angle(void)
{
    return s_angle;
}
