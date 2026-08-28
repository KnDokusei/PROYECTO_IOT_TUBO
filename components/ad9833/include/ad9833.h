/*
 * ad9833.h - AD9833 DDS driver over the ESP-IDF SPI master.
 *
 * Replaces the Arduino MD_AD9833 library. The part is write-only over a 3-wire
 * interface (SCLK, SDATA, FSYNC): there is nothing to read back, so every call
 * here is fire-and-forget and errors can only come from the SPI layer itself.
 *
 * Wiring in the Kundt tube (module E2), per the project schematic:
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
    uint32_t mclk_hz;      /* Crystal on the board; 25 MHz on usual breakouts. */
    uint32_t clock_speed;  /* SPI clock. The part accepts up to 40 MHz. */
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

/** @brief Initialise the SPI bus and reset the device into a known state. */
esp_err_t ad9833_init(const ad9833_config_t *cfg, ad9833_handle_t *out);

/** @brief Release the device and free the SPI bus. */
esp_err_t ad9833_deinit(ad9833_handle_t dev);

/**
 * @brief Set the output frequency.
 *
 * @param actual_hz Optional; receives the frequency really produced, which
 *                  differs from the request because the tuning word is integer.
 */
esp_err_t ad9833_set_frequency(ad9833_handle_t dev, uint32_t freq_hz, uint32_t *actual_hz);

/** @brief Select the output waveform. */
esp_err_t ad9833_set_waveform(ad9833_handle_t dev, ad9833_waveform_t wave);

/** @brief Frequency currently programmed, in Hz. */
uint32_t ad9833_get_frequency(ad9833_handle_t dev);

#ifdef __cplusplus
}
#endif
