/*
 * ad9833_regs.c - see ad9833_regs.h.
 *
 * Compiled both into the firmware and into the host tests.
 */

#include "ad9833_regs.h"

uint32_t ad9833_freq_word(uint32_t freq_hz, uint32_t mclk_hz)
{
    if (mclk_hz == 0) {
        return 0;
    }

    /* A DDS cannot synthesise at or above half its clock. Clamp rather than
     * let the maths wrap, which would emit a plausible-looking alias instead
     * of an obviously wrong value. */
    const uint32_t nyquist = mclk_hz / 2;
    if (freq_hz >= nyquist) {
        freq_hz = nyquist - 1;
    }

    /* FREQREG = f_out * 2^28 / f_MCLK, rounded to nearest.
     * 64-bit intermediate: f_out * 2^28 overflows 32 bits above ~16 Hz. */
    const uint64_t num = ((uint64_t)freq_hz << AD9833_FREQ_BITS) + (mclk_hz / 2);
    return (uint32_t)(num / mclk_hz);
}

uint32_t ad9833_word_to_freq(uint32_t word, uint32_t mclk_hz)
{
    const uint64_t num = (uint64_t)word * mclk_hz + (1ULL << (AD9833_FREQ_BITS - 1));
    return (uint32_t)(num >> AD9833_FREQ_BITS);
}

uint16_t ad9833_control_word(ad9833_waveform_t wave, bool reset)
{
    /* B28 stays set: the frequency word is always written as two 14-bit halves. */
    uint16_t ctrl = AD9833_CTRL_B28;

    switch (wave) {
    case AD9833_WAVE_TRIANGLE:
        ctrl |= AD9833_CTRL_MODE;
        break;
    case AD9833_WAVE_SQUARE:
        /* MSB of the DAC data routed to VOUT, at MCLK/2. */
        ctrl |= AD9833_CTRL_OPBITEN | AD9833_CTRL_DIV2;
        break;
    case AD9833_WAVE_SINE:
    default:
        break;  /* OPBITEN = 0 and MODE = 0 select the sine ROM. */
    }

    if (reset) {
        ctrl |= AD9833_CTRL_RESET;
    }
    return ctrl;
}
