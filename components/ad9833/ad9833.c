/*
 * ad9833.c - ver ad9833.h.
 */

#include "ad9833.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "ad9833";

struct ad9833_dev_t {
    spi_device_handle_t spi;
    spi_host_device_t   host;
    uint32_t            mclk_hz;
    uint32_t            freq_hz;
    ad9833_waveform_t   wave;
};

/* Cada escritura de registro es una sola palabra de 16 bits, MSB primero,
 * enmarcada por FSYNC. El periférico SPI maneja FSYNC como línea de CS. */
static esp_err_t write_word(ad9833_handle_t dev, uint16_t word)
{
    const uint8_t tx[2] = { (uint8_t)(word >> 8), (uint8_t)(word & 0xFF) };

    spi_transaction_t t = {
        .length    = 16,
        .tx_buffer = tx,
    };
    return spi_device_transmit(dev->spi, &t);
}

esp_err_t ad9833_init(const ad9833_config_t *cfg, ad9833_handle_t *out)
{
    if (cfg == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->mclk_hz == 0) {
        ESP_LOGE(TAG, "mclk_hz no puede ser cero");
        return ESP_ERR_INVALID_ARG;
    }

    ad9833_handle_t dev = calloc(1, sizeof(struct ad9833_dev_t));
    if (dev == NULL) {
        return ESP_ERR_NO_MEM;
    }
    dev->host    = cfg->host;
    dev->mclk_hz = cfg->mclk_hz;
    dev->wave    = AD9833_WAVE_SINE;

    const spi_bus_config_t bus = {
        .mosi_io_num     = cfg->gpio_mosi,
        .miso_io_num     = -1,          /* Dispositivo de sólo escritura. */
        .sclk_io_num     = cfg->gpio_sclk,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4,
    };

    esp_err_t err = spi_bus_initialize(cfg->host, &bus, SPI_DMA_DISABLED);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
        free(dev);
        return err;
    }

    /* Modo SPI 2: el AD9833 engancha SDATA en el flanco de bajada de SCLK, y
     * SCLK reposa en alto. Equivocarse aquí no da error: el integrado
     * simplemente malinterpreta todas las palabras. */
    const spi_device_interface_config_t devcfg = {
        .mode           = 2,
        .clock_speed_hz = (int)cfg->clock_speed,
        .spics_io_num   = cfg->gpio_fsync,
        .queue_size     = 1,
        .flags          = SPI_DEVICE_HALFDUPLEX,
    };

    err = spi_bus_add_device(cfg->host, &devcfg, &dev->spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device: %s", esp_err_to_name(err));
        spi_bus_free(cfg->host);
        free(dev);
        return err;
    }

    /* Secuencia de encendido del datasheet: activar RESET y cargar los registros.
     * Aquí sólo se escribe la fase, y RESET queda activo: lo libera la siguiente
     * escritura del registro de control (ad9833_set_waveform o
     * ad9833_set_frequency), que va con reset=false. */
    err = write_word(dev, ad9833_control_word(AD9833_WAVE_SINE, true));
    if (err == ESP_OK) {
        err = write_word(dev, AD9833_REG_PHASE0);  /* Fase 0. */
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "falló la secuencia de reset: %s", esp_err_to_name(err));
        spi_bus_remove_device(dev->spi);
        spi_bus_free(cfg->host);
        free(dev);
        return err;
    }

    *out = dev;
    ESP_LOGI(TAG, "listo en el host %d (MOSI %d, SCLK %d, FSYNC %d), MCLK %lu Hz",
             (int)cfg->host, cfg->gpio_mosi, cfg->gpio_sclk, cfg->gpio_fsync,
             (unsigned long)cfg->mclk_hz);
    return ESP_OK;
}

esp_err_t ad9833_deinit(ad9833_handle_t dev)
{
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    spi_bus_remove_device(dev->spi);
    spi_bus_free(dev->host);
    free(dev);
    return ESP_OK;
}

esp_err_t ad9833_set_frequency(ad9833_handle_t dev, uint32_t freq_hz, uint32_t *actual_hz)
{
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint32_t word = ad9833_freq_word(freq_hz, dev->mclk_hz);

    /* Con B28 activo la palabra de 28 bits sale como dos escrituras consecutivas
     * de 14 bits, primero la baja. Ninguna otra escritura puede intercalarse
     * entre ambas. */
    esp_err_t err = write_word(dev, ad9833_control_word(dev->wave, false));
    if (err == ESP_OK) {
        err = write_word(dev, ad9833_freq_lsb(word));
    }
    if (err == ESP_OK) {
        err = write_word(dev, ad9833_freq_msb(word));
    }
    if (err != ESP_OK) {
        return err;
    }

    dev->freq_hz = ad9833_word_to_freq(word, dev->mclk_hz);
    if (actual_hz != NULL) {
        *actual_hz = dev->freq_hz;
    }
    return ESP_OK;
}

esp_err_t ad9833_set_waveform(ad9833_handle_t dev, ad9833_waveform_t wave)
{
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    dev->wave = wave;
    return write_word(dev, ad9833_control_word(wave, false));
}

uint32_t ad9833_get_frequency(ad9833_handle_t dev)
{
    return (dev != NULL) ? dev->freq_hz : 0;
}
