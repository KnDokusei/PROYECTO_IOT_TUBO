/*
 * kundt_led.h - Status LED shared by the Kundt tube modules.
 *
 * The on-board LED is the only diagnostic available once a module is mounted
 * inside the rig with no serial console attached, so the blink pattern encodes
 * what the firmware is doing rather than just proving it is alive.
 *
 * GPIO2 is the blue LED on the DOIT DEVKIT V1.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KUNDT_LED_DEFAULT_GPIO 2

typedef enum {
    KUNDT_LED_BOOT = 0,   /* Fast flicker: starting up / self-test running */
    KUNDT_LED_NO_WIFI,    /* One slow blink every 2 s: no network */
    KUNDT_LED_NO_SERVER,  /* Double blink: WiFi up, backend unreachable */
    KUNDT_LED_RUNNING,    /* Steady fast blink: everything working */
    KUNDT_LED_SELFTEST,   /* Triple blink: bench self-test build, NOT production */
} kundt_led_state_t;

/**
 * @brief Start the LED task. Safe to call before anything else is up.
 * @param gpio Pin driving the LED; KUNDT_LED_DEFAULT_GPIO for the on-board one.
 */
esp_err_t kundt_led_init(int gpio);

/** @brief Change the pattern. Takes effect at the end of the current cycle. */
void kundt_led_set_state(kundt_led_state_t state);

/** @brief Current pattern. */
kundt_led_state_t kundt_led_get_state(void);

/**
 * @brief Mark the build as a bench self-test.
 *
 * Once latched, the LED adds a distinctive triple blink to every pattern. A
 * self-test build re-routes pins away from where the schematic puts them (the
 * E2 servo moves from GPIO21 to GPIO25), so flashing one into a real rig fails
 * silently -- the wire is simply not driven. This makes that visible from
 * across the bench without a console.
 */
void kundt_led_mark_selftest(void);

/** @brief Human-readable name of a state, for logging. */
const char *kundt_led_state_name(kundt_led_state_t state);

#ifdef __cplusplus
}
#endif
