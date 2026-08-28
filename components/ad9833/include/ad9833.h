/*
 * ad9833.h - Driver del DDS AD9833 sobre el maestro SPI de ESP-IDF.
 *
 * Reemplaza a la librería MD_AD9833 de Arduino. El integrado es de sólo
 * escritura por una interfaz de tres hilos (SCLK, SDATA, FSYNC): no hay nada que
 * leer de vuelta, así que toda llamada es a ciegas y los errores sólo pueden
 * venir de la capa SPI.
 *
 * Cableado en el tubo de Kundt (módulo E2), según el esquemático del proyecto:
 *   FSYNC -> GPIO5,  SDATA -> GPIO23 (VSPI MOSI),  SCLK -> GPIO18 (VSPI SCK)
 */
#pragma once

#include <stdint.h>

#include "ad9833_regs.h"
#include "driver/spi_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    spi_host_device_t host;
    int      gpio_mosi;
    int      gpio_sclk;
    int      gpio_fsync;
    uint32_t mclk_hz;      /* Cristal de la placa; 25 MHz en las habituales. */
    uint32_t clock_speed;  /* Reloj SPI. El integrado admite hasta 40 MHz. */
} ad9833_config_t;

#define AD9833_DEFAULT_CONFIG()                    \
    (ad9833_config_t)                              \
    {                                              \
        .host        = SPI3_HOST, /* VSPI */       \
        .gpio_mosi   = 23,                         \
        .gpio_sclk   = 18,                         \
        .gpio_fsync  = 5,                          \
        .mclk_hz     = AD9833_DEFAULT_MCLK_HZ,     \
        .clock_speed = 1000000,                    \
    }

typedef struct ad9833_dev_t *ad9833_handle_t;

/** @brief Inicializa el bus SPI y deja el dispositivo en un estado conocido. */
esp_err_t ad9833_init(const ad9833_config_t *cfg, ad9833_handle_t *out);

/** @brief Libera el dispositivo y el bus SPI. */
esp_err_t ad9833_deinit(ad9833_handle_t dev);

/**
 * @brief Fija la frecuencia de salida.
 *
 * @param actual_hz Opcional; recibe la frecuencia realmente producida, que
 *                  difiere de la pedida porque la palabra de sintonía es entera.
 */
esp_err_t ad9833_set_frequency(ad9833_handle_t dev, uint32_t freq_hz, uint32_t *actual_hz);

/** @brief Selecciona la forma de onda de salida. */
esp_err_t ad9833_set_waveform(ad9833_handle_t dev, ad9833_waveform_t wave);

/** @brief Frecuencia programada actualmente, en Hz. */
uint32_t ad9833_get_frequency(ad9833_handle_t dev);

#ifdef __cplusplus
}
#endif
