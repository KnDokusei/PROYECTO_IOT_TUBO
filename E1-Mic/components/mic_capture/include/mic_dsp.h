/*
 * mic_dsp.h - Pure sample-conversion logic for the Kundt tube microphone module (E1).
 *
 * Deliberately free of any ESP-IDF dependency so it can be compiled and unit
 * tested on the host (see test/host/). The firmware and the tests build the
 * exact same translation unit.
 *
 * Background: the ESP32 SAR ADC in DMA (continuous) mode emits 16-bit words
 * laid out as TYPE1 on this target: data in bits 0..11, channel in bits 12..15.
 * A single DMA frame may interleave words belonging to other channels, so the
 * channel field must be checked rather than assumed.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 12-bit SAR ADC: raw samples span 0..4095, nominal silence sits at mid-scale. */
#define MIC_ADC_RAW_MAX      4095u
#define MIC_ADC_RAW_MIDPOINT 2048u

/* 12-bit -> 16-bit headroom. 4096 * 16 = 65536 covers the full int16 span. */
#define MIC_DSP_GAIN 16

/*
 * DC offset tracker.
 *
 * The analog front end (electret -> preamp -> LM324) is AC-coupled around a DC
 * operating point, so raw samples are unipolar and centred on that offset, not
 * on zero. Emitting them as-is produces PCM that is offset by roughly half of
 * full scale -- audible as clipping/distortion on the listener side. This
 * tracker estimates the offset and removes it, yielding conventional signed PCM.
 *
 * `track == false` pins the estimate to MIC_ADC_RAW_MIDPOINT, which is correct
 * only if the hardware bias is exactly mid-rail.
 */
typedef struct {
    int32_t dc_q16;   /* Offset estimate in Q16.16 (raw ADC counts). */
    uint8_t shift;    /* Smoothing factor: larger = slower, steadier tracking. */
    bool    track;    /* false pins the estimate at mid-scale. */
} mic_dsp_t;

/* Smoothing default: ~2^10 samples of memory (~23 ms at 44.1 kHz).
 * Fast enough to settle at start-up, far below the audio band so it does not
 * attenuate low-frequency content of interest. */
#define MIC_DSP_DC_SHIFT_DEFAULT 10

/**
 * @brief Initialise the converter.
 * @param dsp    Instance to initialise (must not be NULL).
 * @param track  Follow the measured DC offset instead of assuming mid-scale.
 * @param shift  Smoothing factor; clamped to 1..30. Ignored when track==false.
 */
void mic_dsp_init(mic_dsp_t *dsp, bool track, uint8_t shift);

/** @brief Extract the 12-bit sample value from a raw ADC DMA word. */
static inline uint16_t mic_word_value(uint16_t word)
{
    return (uint16_t)(word & 0x0FFFu);
}

/** @brief Extract the channel index from a raw ADC DMA word. */
static inline uint8_t mic_word_channel(uint16_t word)
{
    return (uint8_t)((word >> 12) & 0x0Fu);
}

/**
 * @brief Convert one raw sample to signed 16-bit PCM, updating the DC estimate.
 *
 * Values above MIC_ADC_RAW_MAX are clamped; the result saturates at the int16
 * limits rather than wrapping.
 */
int16_t mic_dsp_convert(mic_dsp_t *dsp, uint16_t raw12);

/**
 * @brief Convert a DMA frame: filter by channel, convert, and count rejects.
 *
 * This is the hot path. Words whose channel field does not match @p channel are
 * skipped -- they belong to another conversion pattern and must not reach the
 * audio stream.
 *
 * @param dsp      Converter state.
 * @param words    Raw 16-bit ADC words straight from the DMA buffer.
 * @param n_words  Number of words available in @p words.
 * @param channel  ADC channel to keep.
 * @param out      Destination PCM buffer.
 * @param out_cap  Capacity of @p out, in samples.
 * @param dropped  Optional; receives the count of words rejected by the channel
 *                 filter. Pass NULL if not needed.
 * @return Number of PCM samples written to @p out.
 */
size_t mic_dsp_process(mic_dsp_t *dsp,
                       const uint16_t *words,
                       size_t          n_words,
                       uint8_t         channel,
                       int16_t        *out,
                       size_t          out_cap,
                       uint32_t       *dropped);

/** @brief Current DC offset estimate, in raw ADC counts. */
int32_t mic_dsp_dc_offset(const mic_dsp_t *dsp);

/** @brief Summary of one PCM block. Enough to tell signal from a floating pin. */
typedef struct {
    int16_t  min;
    int16_t  max;
    uint64_t sum_sq;  /* Sum of squares; RMS = sqrt(sum_sq / n). */
    size_t   n;
} mic_pcm_stats_t;

/**
 * @brief Measure a PCM block: min, max and sum of squares.
 *
 * Kept here (rather than in mic_capture) so it stays free of ESP-IDF and can be
 * unit tested on the host like the rest of the conversion path.
 */
void mic_dsp_analyze(const int16_t *pcm, size_t n, mic_pcm_stats_t *out);

#ifdef __cplusplus
}
#endif
