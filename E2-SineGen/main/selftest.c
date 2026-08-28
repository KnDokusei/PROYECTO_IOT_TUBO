/*
 * selftest.c - see selftest.h.
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

/* A servo frame is 20 ms. Sampling three of them gives a stable reading even
 * if the loop starts mid-pulse. */
#define FRAMES_TO_SAMPLE 3
#define SAMPLE_WINDOW_US (FRAMES_TO_SAMPLE * 20000)

/* GPIO34 is input-only, but the servo signal is plain 3.3 V logic, so it can be
 * read digitally. That matters: adc_oneshot_read() carries enough driver
 * overhead (lock, configure, convert) that it cannot resolve a 500 us pulse,
 * whereas gpio_get_level() is a register read. */
#define PWM_INPUT_GPIO 34

static uint32_t s_samples_taken;


/*
 * Before measuring anything, prove the jumper actually conducts.
 *
 * GPIO34 is input-only and has no internal pull, so an unconnected pin floats
 * and can read a stable-looking duty from nothing but capacitive coupling --
 * which is indistinguishable from a real measurement unless tested directly.
 * Driving the source pin to each rail and reading the destination settles it.
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

    ESP_LOGI(TAG, "jumper check: drove GPIO%d high -> GPIO%d read %d; "
                  "drove low -> read %d  %s",
             CONFIG_E2_SELFTEST_SERVO_GPIO, PWM_INPUT_GPIO,
             read_high, read_low,
             ok ? "CONNECTED" : "NOT CONNECTED");

    if (!ok) {
        ESP_LOGE(TAG, "the jumper is missing or not making contact.");
        ESP_LOGE(TAG, "GPIO%d floats without it, and a floating pin reads a "
                      "stable-looking duty that is pure coupling noise.",
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

    ESP_LOGW(TAG, "SELF-TEST active: servo PWM on GPIO%d, read on GPIO%d",
             CONFIG_E2_SELFTEST_SERVO_GPIO, PWM_INPUT_GPIO);
    ESP_LOGW(TAG, "SELF-TEST: jumper GPIO%d -> GPIO34 required",
             CONFIG_E2_SELFTEST_SERVO_GPIO);
    return ESP_OK;
}

/*
 * Measures the high time of the PWM by sampling the pin as fast as possible and
 * counting the fraction of the window that reads high.
 *
 * Duty is what is measured; the pulse width follows from the known 20 ms frame.
 * Counting the duty rather than timing edges keeps this immune to the sampling
 * jitter of a polled loop.
 */
static uint32_t measure_pulse_us(void)
{
    const int64_t t0 = esp_timer_get_time();
    uint32_t high = 0, total = 0;

    /* Interrupts stay enabled: WiFi is not up yet at this point, and the duty
     * ratio is unaffected by the occasional preemption anyway. */
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

    /* Let a couple of frames go out before measuring. */
    vTaskDelay(pdMS_TO_TICKS(100));

    const uint32_t expected = servo_angle_to_pulse_us(angle_deg);
    const uint32_t measured = measure_pulse_us();

    /* With a register-read sampling loop the resolution is a few microseconds;
     * 100 us of tolerance covers loop jitter and the LEDC duty quantisation. */
    const int32_t error = (int32_t)measured - (int32_t)expected;
    const bool    ok    = (error > -100 && error < 100);

    ESP_LOGI(TAG, "%3d deg -> expected %4lu us, measured %4lu us, error %+5ld us  "
                  "(%lu samples)  %s",
             angle_deg,
             (unsigned long)expected,
             (unsigned long)measured,
             (long)error,
             (unsigned long)s_samples_taken,
             ok ? "OK" : "OUT OF TOLERANCE");

    return ok ? ESP_OK : ESP_FAIL;
}

void selftest_run_servo_sweep(void)
{
    static const int angles[] = { 0, 45, 90, 135, 180, 90, 0 };

    ESP_LOGI(TAG, "--- servo PWM sweep (measured through the jumper) ---");

    int passed = 0;
    const int n = (int)(sizeof(angles) / sizeof(angles[0]));
    for (int i = 0; i < n; i++) {
        if (selftest_measure_angle(angles[i]) == ESP_OK) {
            passed++;
        }
    }

    ESP_LOGI(TAG, "--- servo sweep: %d/%d within tolerance ---", passed, n);

    /* Out-of-range values must be clamped, not passed through (finding M1). */
    ESP_LOGI(TAG, "--- clamping check (finding M1) ---");
    servo_set_angle(544);   /* The value that Servo::write() misreads as us. */
    ESP_LOGI(TAG, "commanded 544 deg -> servo reports %d deg", servo_get_angle());
    servo_set_angle(-90);
    ESP_LOGI(TAG, "commanded -90 deg -> servo reports %d deg", servo_get_angle());
    servo_set_angle(90);
}

#endif /* CONFIG_E2_SELFTEST */
