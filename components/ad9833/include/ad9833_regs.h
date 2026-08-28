/*
 * ad9833_regs.h - Codificación de registros del generador DDS AD9833.
 *
 * Aritmética entera pura, sin dependencias de ESP-IDF, para poder probar en el
 * host el cálculo de la palabra de frecuencia. El transporte SPI vive en
 * ad9833.c.
 *
 * Referencia: datasheet del AD9833 (Analog Devices), "Programming the AD9833".
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* La dirección de registro va en los bits altos de cada palabra de 16 bits. */
#define AD9833_REG_CONTROL 0x0000u
#define AD9833_REG_FREQ0   0x4000u
#define AD9833_REG_FREQ1   0x8000u
#define AD9833_REG_PHASE0  0xC000u
#define AD9833_REG_PHASE1  0xE000u

/* Bits del registro de control que se usan aquí (datasheet, tabla de control). */
#define AD9833_CTRL_B28     (1u << 13)  /* Palabra de 28 bits en dos escrituras */
#define AD9833_CTRL_RESET   (1u << 8)   /* Mantiene en reset el acumulador de fase */
#define AD9833_CTRL_OPBITEN (1u << 5)   /* Saca el MSB del DAC por VOUT (cuadrada) */
#define AD9833_CTRL_DIV2    (1u << 3)   /* Sólo con OPBITEN=1: 1 saca el MSB, 0 el MSB/2 */
#define AD9833_CTRL_MODE    (1u << 1)   /* Triangular en vez de senoidal */

/* La palabra de frecuencia es de 28 bits, partida en dos mitades de 14. */
#define AD9833_FREQ_BITS 28
#define AD9833_FREQ_MASK 0x3FFFu

/* Cristal que traen las placas de desarrollo habituales del AD9833. */
#define AD9833_DEFAULT_MCLK_HZ 25000000u

typedef enum {
    AD9833_WAVE_SINE = 0,
    AD9833_WAVE_TRIANGLE,
    AD9833_WAVE_SQUARE,
} ad9833_waveform_t;

/**
 * @brief Palabra de sintonía: FREQREG = (f_out * 2^28) / f_MCLK.
 *
 * Redondeada al más cercano. Las frecuencias iguales o superiores a Nyquist
 * (MCLK/2) se acotan: el DDS no puede sintetizarlas, y dejar que la palabra
 * desborde produciría un alias en una frecuencia cualquiera en vez de un error
 * evidente.
 *
 * @return La palabra de sintonía de 28 bits.
 */
uint32_t ad9833_freq_word(uint32_t freq_hz, uint32_t mclk_hz);

/** @brief Los 14 bits bajos de una palabra de sintonía, dirigidos a FREQ0. */
static inline uint16_t ad9833_freq_lsb(uint32_t word)
{
    return (uint16_t)(AD9833_REG_FREQ0 | (word & AD9833_FREQ_MASK));
}

/** @brief Los 14 bits altos de una palabra de sintonía, dirigidos a FREQ0. */
static inline uint16_t ad9833_freq_msb(uint32_t word)
{
    return (uint16_t)(AD9833_REG_FREQ0 | ((word >> 14) & AD9833_FREQ_MASK));
}

/** @brief Palabra de control para una forma de onda, activando RESET si se pide. */
uint16_t ad9833_control_word(ad9833_waveform_t wave, bool reset);

/**
 * @brief Frecuencia que realmente produce una palabra de sintonía.
 *
 * La inversa de ad9833_freq_word(). Sirve para informar la salida real, que
 * difiere levemente de la pedida porque la palabra es entera.
 */
uint32_t ad9833_word_to_freq(uint32_t word, uint32_t mclk_hz);

#ifdef __cplusplus
}
#endif
