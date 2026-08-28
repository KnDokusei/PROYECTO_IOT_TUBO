/*
 * ad9833.c - see ad9833.h.
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

/* Every register write is a single 16-bit word, MSB first, framed by FSYNC.
 * FSYNC is driven by the SPI peripheral as the CS line. */
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
        ESP_LOGE(TAG, "mclk_hz must be non-zero");
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
        .miso_io_num     = -1,          /* Write-only device. */
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

    /* SPI mode 2: the AD9833 latches SDATA on the falling edge of SCLK, and
     * SCLK idles high. Getting this wrong is silent -- the part simply
     * misinterprets every word. */
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

    /* Power-up sequence from the data sheet: assert RESET, load the frequency
     * and phase registers, then clear RESET to start the accumulator. */
    err = write_word(dev, ad9833_control_word(AD9833_WAVE_SINE, true));
    if (err == ESP_OK) {
        err = write_word(dev, AD9833_REG_PHASE0);  /* Phase 0. */
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reset sequence failed: %s", esp_err_to_name(err));
        spi_bus_remove_device(dev->spi);
        spi_bus_free(cfg->host);
        free(dev);
        return err;
    }

    *out = dev;
    ESP_LOGI(TAG, "ready on host %d (MOSI %d, SCLK %d, FSYNC %d), MCLK %lu Hz",
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

    /* With B28 set the 28-bit word goes out as two consecutive 14-bit writes,
     * LSBs first. They must not be separated by any other register write. */
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
