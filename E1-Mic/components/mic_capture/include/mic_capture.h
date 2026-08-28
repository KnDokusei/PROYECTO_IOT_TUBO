/*
 * mic_capture.h - Continuous microphone acquisition for the Kundt tube (E1).
 *
 * Replaces the Arduino implementation that drove the SAR ADC through the legacy
 * I2S driver (`I2S_MODE_ADC_BUILT_IN`). That mode was deprecated in ESP-IDF 5.0
 * and removed outright in 6.0; `esp_adc/adc_continuous.h` is the supported
 * replacement and drives the same hardware (the ADC DMA path still borrows I2S0
 * internally on this target).
 *
 * Hardware: electret microphone -> preamp -> LM324 -> GPIO34 (ADC1 channel 6).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "hal/adc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Tunables. These map one-to-one onto the "Variables de interés" listed for E1
 * in the original README, renamed because nothing here is I2S any more:
 *
 *   I2S_SAMPLE_RATE    -> sample_rate_hz
 *   I2S_DMA_BUF_LEN    -> frame_samples
 *   I2S_DMA_BUF_COUNT  -> frame_count
 */
typedef struct {
    uint32_t     sample_rate_hz;  /* Must fall inside the target's ADC range. */
    uint16_t     frame_samples;   /* Samples per DMA conversion frame. */
    uint8_t      frame_count;     /* Frames held in the driver's pool. */
    adc_channel_t adc_channel;    /* ADC1 channel; 6 == GPIO34 on ESP32. */
    adc_atten_t  atten;           /* Input attenuation (sets the voltage span). */
    bool         track_dc;        /* Follow the measured DC bias (recommended). */
    uint8_t      dc_shift;        /* DC tracker smoothing factor. */
} mic_capture_config_t;

/*
 * Defaults preserve the acquisition parameters of the Arduino build: 44.1 kHz,
 * 512-sample frames, 10 frames in flight, ADC1_CH6 (GPIO34).
 *
 * ADC_ATTEN_DB_12 selects the widest input span (~0..3.1 V). Note that the
 * schematic's front end is powered from 5 V; the README's own "Mejoras Futuras"
 * section calls for reworking it to 3.3 V with a 1.65 V operating point. Until
 * that rework lands, loud passages can drive the input above the ADC's range
 * and clip. That is an analog-side limitation, not something firmware can fix.
 */
#define MIC_CAPTURE_DEFAULT_CONFIG()             \
    (mic_capture_config_t)                       \
    {                                            \
        .sample_rate_hz = 44100,                 \
        .frame_samples  = 512,                   \
        .frame_count    = 10,                    \
        .adc_channel    = ADC_CHANNEL_6,         \
        .atten          = ADC_ATTEN_DB_12,       \
        .track_dc       = true,                  \
        .dc_shift       = 10,                    \
    }

/** @brief Runtime counters, useful for spotting overruns without a scope. */
typedef struct {
    uint64_t samples_captured;  /* PCM samples handed to the caller. */
    uint64_t words_dropped;     /* DMA words rejected by the channel filter. */
    uint32_t pool_overflows;    /* Driver pool filled: samples were lost. */
    int32_t  dc_offset;         /* Current DC estimate, in raw ADC counts. */
    int16_t  pcm_min;           /* Minimum PCM value in the last block. */
    int16_t  pcm_max;           /* Maximum PCM value in the last block. */
    uint32_t pcm_rms;           /* RMS of the last block. */
} mic_capture_stats_t;

/**
 * @brief Validate a configuration without starting the driver.
 *
 * Split out from mic_capture_start() so the checks are reachable from tests.
 * Verifies the sample rate against the target's supported range and that the
 * resulting frame size is a multiple of SOC_ADC_DIGI_DATA_BYTES_PER_CONV --
 * the alignment rule the continuous driver enforces.
 *
 * @return ESP_OK, or ESP_ERR_INVALID_ARG with a message logged.
 */
esp_err_t mic_capture_validate(const mic_capture_config_t *cfg);

/** @brief Configure and start continuous acquisition. */
esp_err_t mic_capture_start(const mic_capture_config_t *cfg);

/** @brief Stop acquisition and release the driver. Safe to call when stopped. */
esp_err_t mic_capture_stop(void);

/** @brief Whether acquisition is currently running. */
bool mic_capture_is_running(void);

/**
 * @brief Read one block of signed 16-bit PCM.
 *
 * Blocks until data is available or @p timeout_ms elapses.
 *
 * @param out          Destination buffer.
 * @param out_cap      Capacity of @p out, in samples.
 * @param out_samples  Receives the number of samples written.
 * @param timeout_ms   Block limit in milliseconds.
 * @return ESP_OK, ESP_ERR_TIMEOUT if no data arrived, ESP_ERR_INVALID_STATE if
 *         not started, or ESP_ERR_INVALID_ARG.
 */
esp_err_t mic_capture_read(int16_t *out,
                           size_t   out_cap,
                           size_t  *out_samples,
                           uint32_t timeout_ms);

/** @brief Snapshot the runtime counters. */
void mic_capture_get_stats(mic_capture_stats_t *stats);

#ifdef __cplusplus
}
#endif
