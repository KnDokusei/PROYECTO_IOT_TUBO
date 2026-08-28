/*
 * EC-Cameras - Kundt tube module. NOT MIGRATED YET.
 *
 * Role:
 * Servidor HTTP que transmite MJPEG desde el sensor OV2640 y se registra
 * en el servidor indicando su IP. Tres placas por equipo.
 *
 * Transport: Sirve :80/Equipo{kit}/Cam{n} ; PUT de registro a :6001
 *
 * Hardware (AI-Thinker ESP32-CAM):
 *   Sensor OV2640          pinout AI-Thinker (PWDN 32, XCLK 0, SIOD 26, SIOC 27)
 *   Datos Y9..Y2           35,34,39,36,21,19,18,5
 *   VSYNC / HREF / PCLK    25 / 23 / 22
 *
 * Migration status: pending -- Fase 1 del plan (el mas facil) of VIABILIDAD-MIGRACION.md.
 * The working firmware for this module is still the Arduino sketch in
 * ../../src/EC-Cameras/ of the original project.
 *
 * Outstanding audit findings: A4, B5, B6, B10
 *
 * What this skeleton already does: brings up NVS through the shared
 * kundt_config component, so the kit number and credentials are readable here
 * exactly as in E1. Everything else is still to be written:
 *   1. Anadir espressif/esp32-camera a idf_component.yml
 *   2. El sketch Arduino YA usa esp_camera.h y esp_http_server.h: se copian tal cual
 *   3. Sustituir WiFi.h/HTTPClient por los componentes compartidos
 *   4. Verificar PSRAM antes de usar XGA con fb_count=2 (B10)
 */

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "kundt_config.h"

static const char *TAG = "EC-Cameras";

void app_main(void)
{
    ESP_LOGI(TAG, "Kundt tube - EC-Cameras (AI-Thinker ESP32-CAM)");

    /* Shared configuration works already; the module logic does not exist yet. */
    ESP_ERROR_CHECK(kundt_config_init());
    kundt_config_log();

    ESP_LOGW(TAG, "This module has NOT been migrated to ESP-IDF yet.");
    ESP_LOGW(TAG, "Pending work (Fase 1 del plan (el mas facil)):");
    ESP_LOGW(TAG, "  1. Anadir espressif/esp32-camera a idf_component.yml");
    ESP_LOGW(TAG, "  2. El sketch Arduino YA usa esp_camera.h y esp_http_server.h: se copian tal cual");
    ESP_LOGW(TAG, "  3. Sustituir WiFi.h/HTTPClient por los componentes compartidos");
    ESP_LOGW(TAG, "  4. Verificar PSRAM antes de usar XGA con fb_count=2 (B10)");
    ESP_LOGW(TAG, "Use the Arduino sketch in src/EC-Cameras/ until this is done.");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        ESP_LOGI(TAG, "skeleton alive - nothing to do");
    }
}
