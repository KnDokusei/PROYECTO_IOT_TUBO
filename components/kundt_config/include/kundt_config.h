/*
 * kundt_config.h - Runtime configuration held in NVS.
 *
 * The Arduino build carried SSID, password and kit number as #defines in
 * wifiConfig.h, which meant (a) credentials sat in the source tree and (b) every
 * kit needed its own binary -- 30 of them across the five kits and four modules.
 * Here the same values live in NVS and are set at runtime, so one binary serves
 * every kit and no secret is committed.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KUNDT_SSID_MAX_LEN     32
#define KUNDT_PASSWORD_MAX_LEN 64
#define KUNDT_IP_MAX_LEN       15

#define KUNDT_KIT_MIN 1
#define KUNDT_KIT_MAX 5

/* The original endpoint was "808" concatenated with the kit digit, so kit 1
 * listens on 8081 and kit 5 on 8085. */
#define KUNDT_WS_PORT_BASE 8080

typedef struct {
    char     wifi_ssid[KUNDT_SSID_MAX_LEN + 1];
    char     wifi_password[KUNDT_PASSWORD_MAX_LEN + 1];
    char     server_ip[KUNDT_IP_MAX_LEN + 1];
    uint8_t  kit;  /* 1..5 */
} kundt_config_t;

/**
 * @brief Initialise NVS and load the stored configuration.
 *
 * Falls back to the Kconfig defaults on first boot (or after an erase) and
 * persists them, so a fresh board comes up in a known state.
 */
esp_err_t kundt_config_init(void);

/** @brief Copy the active configuration. */
esp_err_t kundt_config_get(kundt_config_t *out);

/** @brief Store WiFi credentials. Takes effect on the next connect. */
esp_err_t kundt_config_set_wifi(const char *ssid, const char *password);

/** @brief Store the kit number; rejects anything outside 1..5. */
esp_err_t kundt_config_set_kit(uint8_t kit);

/** @brief Store the server address (dotted-quad string). */
esp_err_t kundt_config_set_server_ip(const char *ip);

/** @brief WebSocket port for the active kit (KUNDT_WS_PORT_BASE + kit). */
uint16_t kundt_config_ws_port(void);

/**
 * @brief Build the WebSocket URI for the active kit, e.g. "ws://192.168.0.100:8081/".
 * @return ESP_OK, or ESP_ERR_INVALID_SIZE if @p buf is too small.
 */
esp_err_t kundt_config_ws_uri(char *buf, size_t buf_len);

/** @brief True when SSID and server address are both non-empty. */
bool kundt_config_is_provisioned(void);

/** @brief Log the active configuration. The password is never printed. */
void kundt_config_log(void);

#ifdef __cplusplus
}
#endif
