/*
 * kundt_config.c - see kundt_config.h.
 */

#include "kundt_config.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "kundt_config";

#define NVS_NAMESPACE "kundt"
#define KEY_SSID      "ssid"
#define KEY_PASSWORD  "pass"
#define KEY_SERVER_IP "srv_ip"
#define KEY_KIT       "kit"

static kundt_config_t s_cfg;
static bool           s_loaded;

/* Copies into a fixed buffer and always terminates. Returns false when the
 * source would not fit, so callers can reject rather than silently truncate. */
static bool copy_bounded(char *dst, size_t dst_size, const char *src)
{
    if (src == NULL) {
        return false;
    }
    const size_t len = strlen(src);
    if (len >= dst_size) {
        return false;
    }
    memcpy(dst, src, len + 1);
    return true;
}

static esp_err_t nvs_get_str_or_default(nvs_handle_t h,
                                        const char  *key,
                                        char        *dst,
                                        size_t       dst_size,
                                        const char  *fallback)
{
    size_t    len = dst_size;
    esp_err_t err = nvs_get_str(h, key, dst, &len);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        if (!copy_bounded(dst, dst_size, fallback)) {
            dst[0] = '\0';
        }
        return ESP_OK;
    }
    return err;
}

static esp_err_t store_str(const char *key, const char *value)
{
    nvs_handle_t h;
    esp_err_t    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(h, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t kundt_config_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erasing, reformatting");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return err;
    }

    nvs_handle_t h;
    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* Namespace absent: first boot. Seed from the build-time defaults. */
        ESP_LOGI(TAG, "no stored config, seeding from Kconfig defaults");
        copy_bounded(s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid), CONFIG_KUNDT_DEFAULT_WIFI_SSID);
        copy_bounded(s_cfg.wifi_password, sizeof(s_cfg.wifi_password), CONFIG_KUNDT_DEFAULT_WIFI_PASSWORD);
        copy_bounded(s_cfg.server_ip, sizeof(s_cfg.server_ip), CONFIG_KUNDT_DEFAULT_SERVER_IP);
        s_cfg.kit = CONFIG_KUNDT_DEFAULT_KIT;
        s_loaded  = true;

        kundt_config_set_wifi(s_cfg.wifi_ssid, s_cfg.wifi_password);
        kundt_config_set_server_ip(s_cfg.server_ip);
        kundt_config_set_kit(s_cfg.kit);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    nvs_get_str_or_default(h, KEY_SSID, s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid),
                           CONFIG_KUNDT_DEFAULT_WIFI_SSID);
    nvs_get_str_or_default(h, KEY_PASSWORD, s_cfg.wifi_password, sizeof(s_cfg.wifi_password),
                           CONFIG_KUNDT_DEFAULT_WIFI_PASSWORD);
    nvs_get_str_or_default(h, KEY_SERVER_IP, s_cfg.server_ip, sizeof(s_cfg.server_ip),
                           CONFIG_KUNDT_DEFAULT_SERVER_IP);

    uint8_t kit = 0;
    if (nvs_get_u8(h, KEY_KIT, &kit) != ESP_OK || kit < KUNDT_KIT_MIN || kit > KUNDT_KIT_MAX) {
        kit = CONFIG_KUNDT_DEFAULT_KIT;
    }
    s_cfg.kit = kit;

    nvs_close(h);
    s_loaded = true;
    return ESP_OK;
}

esp_err_t kundt_config_get(kundt_config_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_loaded) {
        return ESP_ERR_INVALID_STATE;
    }
    *out = s_cfg;
    return ESP_OK;
}

esp_err_t kundt_config_set_wifi(const char *ssid, const char *password)
{
    if (ssid == NULL || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!copy_bounded(s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid), ssid) ||
        !copy_bounded(s_cfg.wifi_password, sizeof(s_cfg.wifi_password), password)) {
        ESP_LOGE(TAG, "SSID or password too long");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = store_str(KEY_SSID, s_cfg.wifi_ssid);
    if (err == ESP_OK) {
        err = store_str(KEY_PASSWORD, s_cfg.wifi_password);
    }
    return err;
}

esp_err_t kundt_config_set_kit(uint8_t kit)
{
    if (kit < KUNDT_KIT_MIN || kit > KUNDT_KIT_MAX) {
        ESP_LOGE(TAG, "kit %u outside %d..%d", kit, KUNDT_KIT_MIN, KUNDT_KIT_MAX);
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(h, KEY_KIT, kit);
    if (err == ESP_OK) {
        err = nvs_commit(h);
        s_cfg.kit = kit;
    }
    nvs_close(h);
    return err;
}

esp_err_t kundt_config_set_server_ip(const char *ip)
{
    if (!copy_bounded(s_cfg.server_ip, sizeof(s_cfg.server_ip), ip)) {
        ESP_LOGE(TAG, "server IP too long");
        return ESP_ERR_INVALID_ARG;
    }
    return store_str(KEY_SERVER_IP, s_cfg.server_ip);
}

uint16_t kundt_config_ws_port(void)
{
    return (uint16_t)(KUNDT_WS_PORT_BASE + s_cfg.kit);
}

esp_err_t kundt_config_ws_uri(char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const int n = snprintf(buf, buf_len, "ws://%s:%u/",
                           s_cfg.server_ip, (unsigned)kundt_config_ws_port());
    if (n < 0 || (size_t)n >= buf_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

bool kundt_config_is_provisioned(void)
{
    return s_loaded && s_cfg.wifi_ssid[0] != '\0' && s_cfg.server_ip[0] != '\0';
}

void kundt_config_log(void)
{
    ESP_LOGI(TAG, "kit=%u  ssid=\"%s\"  server=%s  ws_port=%u",
             (unsigned)s_cfg.kit,
             s_cfg.wifi_ssid,
             s_cfg.server_ip,
             (unsigned)kundt_config_ws_port());
    ESP_LOGI(TAG, "password: %s", s_cfg.wifi_password[0] ? "<set>" : "<empty>");
}
