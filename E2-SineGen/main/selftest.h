/*
 * selftest.h - Bench validation for module E2 without its analog hardware.
 *
 * Measures the servo PWM by sampling it with the ADC, which only needs a wire
 * between the servo pin and the ADC input. Compiled out unless
 * CONFIG_E2_SELFTEST is set.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_E2_SELFTEST

/**
 * @brief Verify the jumper conducts before trusting any measurement.
 * @return ESP_OK if driving the source pin changes the destination pin.
 */
esp_err_t selftest_check_jumper(void);

/** @brief Bring up the pin used to observe the PWM. */
esp_err_t selftest_init(void);

/**
 * @brief Command an angle, then measure the pulse it produces.
 *
 * Samples GPIO34 in a tight loop for a few PWM frames and reports the measured
 * high time. Compares it against what servo_angle_to_pulse_us() predicts.
 *
 * @return ESP_OK if the measurement is within tolerance.
 */
esp_err_t selftest_measure_angle(int angle_deg);

/** @brief Sweep a set of angles and report each one. */
void selftest_run_servo_sweep(void);

#endif /* CONFIG_E2_SELFTEST */

#ifdef __cplusplus
}
#endif
