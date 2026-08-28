/*
 * ad9833_regs.h - Register encoding for the AD9833 DDS waveform generator.
 *
 * Pure integer maths, no ESP-IDF dependency, so the frequency-word calculation
 * can be unit tested on the host. The SPI transport lives in ad9833.c.
 *
 * Reference: AD9833 data sheet (Analog Devices), "Programming the AD9833".
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Register addresses live in the top two bits of every 16-bit word. */
#define AD9833_REG_CONTROL 0x0000u
#define AD9833_REG_FREQ0   0x4000u
#define AD9833_REG_FREQ1   0x8000u
#define AD9833_REG_PHASE0  0xC000u
#define AD9833_REG_PHASE1  0xE000u

/* Control-register bits used here. */
#define AD9833_CTRL_B28     (1u << 13)  /* 28-bit word in two consecutive writes */
#define AD9833_CTRL_RESET   (1u << 8)   /* Hold the phase accumulator in reset */
#define AD9833_CTRL_OPBITEN (1u << 5)   /* Route the MSB to VOUT (square wave) */
#define AD9833_CTRL_DIV2    (1u << 3)   /* Square wave at MCLK/2 instead of /4 */
#define AD9833_CTRL_MODE    (1u << 1)   /* Triangle instead of sine */

/* The frequency word is 28 bits, split into two 14-bit halves. */
#define AD9833_FREQ_BITS 28
#define AD9833_FREQ_MASK 0x3FFFu

/* Crystal fitted to the common AD9833 breakout boards. */
#define AD9833_DEFAULT_MCLK_HZ 25000000u

typedef enum {
    AD9833_WAVE_SINE = 0,
    AD9833_WAVE_TRIANGLE,
    AD9833_WAVE_SQUARE,
} ad9833_waveform_t;

/**
 * @brief Frequency tuning word: FREQREG = (f_out * 2^28) / f_MCLK.
 *
 * Rounded to nearest. Frequencies at or above Nyquist (MCLK/2) are clamped:
 * the DDS cannot synthesise them and letting the word wrap would produce an
 * alias at some unrelated frequency instead of an obvious error.
 *
 * @return The 28-bit tuning word.
 */
uint32_t ad9833_freq_word(uint32_t freq_hz, uint32_t mclk_hz);

/** @brief Low 14 bits of a tuning word, addressed to FREQ0. */
static inline uint16_t ad9833_freq_lsb(uint32_t word)
{
    return (uint16_t)(AD9833_REG_FREQ0 | (word & AD9833_FREQ_MASK));
}

/** @brief High 14 bits of a tuning word, addressed to FREQ0. */
static inline uint16_t ad9833_freq_msb(uint32_t word)
{
    return (uint16_t)(AD9833_REG_FREQ0 | ((word >> 14) & AD9833_FREQ_MASK));
}

/** @brief Control word for a waveform, optionally asserting RESET. */
uint16_t ad9833_control_word(ad9833_waveform_t wave, bool reset);

/**
 * @brief Frequency actually produced by a tuning word.
 *
 * The inverse of ad9833_freq_word(). Useful to report the real output, which
 * differs slightly from the request because the word is an integer.
 */
uint32_t ad9833_word_to_freq(uint32_t word, uint32_t mclk_hz);

#ifdef __cplusplus
}
#endif
