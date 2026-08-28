/*
 * kundt_wifi.h - Station-mode WiFi with automatic reconnection.
 *
 * The original README states that the board "detectará el cambio, e intentará
 * reconectar" when WiFi drops. In the Arduino build that was true of E2, E3 and
 * EC, but not of E1: it connected once in setup() and never checked again
 * (finding A1). Here reconnection is driven by esp_event callbacks, so it is a
 * property of the component rather than something a polling loop must remember
 * to do.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Bring up netif, the default event loop and the WiFi driver. */
esp_err_t kundt_wifi_init(void);

/**
 * @brief Start connecting. Returns immediately; the link comes up asynchronously.
 *
 * On disconnect the component retries on its own, backing off from
 * KUNDT_WIFI_RETRY_MIN_MS up to KUNDT_WIFI_RETRY_MAX_MS. It never gives up:
 * the rig sits in a lab and is expected to recover unattended after an AP
 * reboot or a power blip.
 */
esp_err_t kundt_wifi_connect(const char *ssid, const char *password);

/** @brief True while the station holds an IP address. */
bool kundt_wifi_is_connected(void);

/**
 * @brief Block until connected.
 * @return ESP_OK, or ESP_ERR_TIMEOUT if the link did not come up in time.
 */
esp_err_t kundt_wifi_wait_connected(uint32_t timeout_ms);

/** @brief Dotted-quad address of the station, or "0.0.0.0" when down. */
const char *kundt_wifi_ip(void);

/** @brief Number of disconnect events seen since boot. */
uint32_t kundt_wifi_disconnect_count(void);

#define KUNDT_WIFI_RETRY_MIN_MS 1000
#define KUNDT_WIFI_RETRY_MAX_MS 30000

#ifdef __cplusplus
}
#endif
