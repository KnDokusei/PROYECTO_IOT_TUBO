/*
 * ad9833_regs.c - ver ad9833_regs.h.
 *
 * Se compila tanto en el firmware como en los tests de host.
 */

#include "ad9833_regs.h"

uint32_t ad9833_freq_word(uint32_t freq_hz, uint32_t mclk_hz)
{
    if (mclk_hz == 0) {
        return 0;
    }

    /* Un DDS no puede sintetizar en la mitad de su reloj ni por encima. Se acota
     * en vez de dejar desbordar la cuenta, que emitiría un alias de aspecto
     * plausible en lugar de un valor evidentemente incorrecto. */
    const uint32_t nyquist = mclk_hz / 2;
    if (freq_hz >= nyquist) {
        freq_hz = nyquist - 1;
    }

    /* FREQREG = f_out * 2^28 / f_MCLK, redondeado al más cercano.
     * Intermedio de 64 bits: f_out * 2^28 desborda 32 bits pasados los ~16 Hz. */
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
    /* B28 queda siempre activo: la frecuencia se escribe como dos mitades de 14 bits. */
    uint16_t ctrl = AD9833_CTRL_B28;

    switch (wave) {
    case AD9833_WAVE_TRIANGLE:
        ctrl |= AD9833_CTRL_MODE;
        break;
    case AD9833_WAVE_SQUARE:
        /* El MSB del DAC sale por VOUT. Con DIV2=1 se entrega sin dividir, así
         * que la cuadrada queda en la frecuencia programada; con DIV2=0 saldría
         * a la mitad. */
        ctrl |= AD9833_CTRL_OPBITEN | AD9833_CTRL_DIV2;
        break;
    case AD9833_WAVE_SINE:
    default:
        break;  /* Con OPBITEN = 0 y MODE = 0 se elige la ROM senoidal. */
    }

    if (reset) {
        ctrl |= AD9833_CTRL_RESET;
    }
    return ctrl;
}
