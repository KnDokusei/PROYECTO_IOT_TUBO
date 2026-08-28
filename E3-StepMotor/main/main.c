/*
 * E3-StepMotor - Kundt tube module. NOT MIGRATED YET.
 *
 * Role:
 * Mueve el embolo con un motor paso a paso via driver A4988, con dos
 * fines de carrera, y reporta la posicion al servidor.
 *
 * Transport: HTTP GET + PUT a :5000/api/kundt/equipo/{kit} cada 500 ms
 *
 * Hardware (DOIT ESP32 DEVKIT V1):
 *   A4988 STEP             GPIO12
 *   A4988 DIR              GPIO14
 *   A4988 SLEEP            GPIO33
 *   Fin de carrera +       GPIO34 (input-only, pull-up externo)
 *   Fin de carrera -       GPIO35 (input-only, pull-up externo)
 *
 * Migration status: pending -- Fase 4 of VIABILIDAD-MIGRACION.md.
 * The working firmware for this module is still the Arduino sketch in
 * ../../src/E3-StepMotor/ of the original project.
 *
 * Outstanding audit findings: A2, A3 (bloqueante), A4, M2, M3, M4, B1-B4
 *
 * What this skeleton already does: brings up NVS through the shared
 * kundt_config component, so the kit number and credentials are readable here
 * exactly as in E1. Everything else is still to be written:
 *   1. Generar pasos con RMT (ejemplo oficial peripherals/rmt/stepper_motor)
 *   2. Respetar el wake-up de 1 ms del A4988 tras SLEEP (M2)
 *   3. Cerrar A2 (telemetria a 0.4 cm en vez de 0.1)
 *   4. BLOQUEANTE: resolver A3 (GET en mm vs PUT en cm) contra el backend
 */

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "kundt_config.h"

static const char *TAG = "E3-StepMotor";

void app_main(void)
{
    ESP_LOGI(TAG, "Kundt tube - E3-StepMotor (DOIT ESP32 DEVKIT V1)");

    /* Shared configuration works already; the module logic does not exist yet. */
    ESP_ERROR_CHECK(kundt_config_init());
    kundt_config_log();

    ESP_LOGW(TAG, "This module has NOT been migrated to ESP-IDF yet.");
    ESP_LOGW(TAG, "Pending work (Fase 4):");
    ESP_LOGW(TAG, "  1. Generar pasos con RMT (ejemplo oficial peripherals/rmt/stepper_motor)");
    ESP_LOGW(TAG, "  2. Respetar el wake-up de 1 ms del A4988 tras SLEEP (M2)");
    ESP_LOGW(TAG, "  3. Cerrar A2 (telemetria a 0.4 cm en vez de 0.1)");
    ESP_LOGW(TAG, "  4. BLOQUEANTE: resolver A3 (GET en mm vs PUT en cm) contra el backend");
    ESP_LOGW(TAG, "Use the Arduino sketch in src/E3-StepMotor/ until this is done.");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        ESP_LOGI(TAG, "skeleton alive - nothing to do");
    }
}
