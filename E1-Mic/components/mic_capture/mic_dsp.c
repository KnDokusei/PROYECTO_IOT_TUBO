/*
 * mic_dsp.c - ver mic_dsp.h.
 *
 * Sin cabeceras de ESP-IDF a propósito: este archivo se compila tanto en el
 * firmware como en los tests de host.
 */

#include <math.h>

#include "mic_dsp.h"

/* El offset se lleva en punto fijo Q16.16 para que un suavizado de hasta 30
 * siga resolviendo cambios menores a una cuenta; en entero puro el término de
 * corrección se cuantizaría a cero y el estimador nunca convergería. */
#define Q16_SHIFT 16
#define Q16_ONE   (1 << Q16_SHIFT)

#define INT16_MIN_V (-32768)
#define INT16_MAX_V (32767)

void mic_dsp_init(mic_dsp_t *dsp, bool track, uint8_t shift)
{
    if (dsp == NULL) {
        return;
    }

    if (shift < 1u) {
        shift = 1u;
    } else if (shift > 30u) {
        shift = 30u;
    }

    dsp->shift = shift;
    dsp->track = track;
    /* Semilla a media escala: con seguimiento activo es sólo un punto de
     * partida, y mantiene sensatas las primeras muestras mientras converge. */
    dsp->dc_q16 = (int32_t)MIC_ADC_RAW_MIDPOINT << Q16_SHIFT;
}

int32_t mic_dsp_dc_offset(const mic_dsp_t *dsp)
{
    if (dsp == NULL) {
        return 0;
    }
    return dsp->dc_q16 >> Q16_SHIFT;
}

int16_t mic_dsp_convert(mic_dsp_t *dsp, uint16_t raw12)
{
    if (dsp == NULL) {
        return 0;
    }

    if (raw12 > MIC_ADC_RAW_MAX) {
        raw12 = (uint16_t)MIC_ADC_RAW_MAX;
    }

    const int32_t raw_q16 = (int32_t)raw12 << Q16_SHIFT;

    if (dsp->track) {
        /* IIR de primer orden: dc += (raw - dc) >> shift. */
        dsp->dc_q16 += (raw_q16 - dsp->dc_q16) >> dsp->shift;
    } else {
        dsp->dc_q16 = (int32_t)MIC_ADC_RAW_MIDPOINT << Q16_SHIFT;
    }

    /* Centrar y luego escalar de 12 bits al rango de int16. En int32 para que
     * una excursión a fondo de escala no desborde antes del recorte. */
    const int32_t centred = (raw_q16 - dsp->dc_q16) >> Q16_SHIFT;
    int32_t       scaled  = centred * MIC_DSP_GAIN;

    if (scaled > INT16_MAX_V) {
        scaled = INT16_MAX_V;
    } else if (scaled < INT16_MIN_V) {
        scaled = INT16_MIN_V;
    }

    return (int16_t)scaled;
}

size_t mic_dsp_process(mic_dsp_t *dsp,
                       const uint16_t *words,
                       size_t          n_words,
                       uint8_t         channel,
                       int16_t        *out,
                       size_t          out_cap,
                       uint32_t       *dropped)
{
    size_t   written = 0;
    uint32_t rejects = 0;

    if (dsp == NULL || words == NULL || out == NULL) {
        if (dropped != NULL) {
            *dropped = 0;
        }
        return 0;
    }

    for (size_t i = 0; i < n_words && written < out_cap; i++) {
        if (mic_word_channel(words[i]) != channel) {
            rejects++;
            continue;
        }
        out[written++] = mic_dsp_convert(dsp, mic_word_value(words[i]));
    }

    if (dropped != NULL) {
        *dropped = rejects;
    }
    return written;
}

void mic_dsp_analyze(const int16_t *pcm, size_t n, mic_pcm_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    if (pcm == NULL || n == 0) {
        out->min = 0;
        out->max = 0;
        out->sum_sq = 0;
        out->n = 0;
        return;
    }

    int16_t  lo = pcm[0];
    int16_t  hi = pcm[0];
    uint64_t acc = 0;

    for (size_t i = 0; i < n; i++) {
        const int16_t v = pcm[i];
        if (v < lo) lo = v;
        if (v > hi) hi = v;
        acc += (uint64_t)((int32_t)v * (int32_t)v);
    }

    out->min = lo;
    out->max = hi;
    out->sum_sq = acc;
    out->n = n;
}

/* ---------------------------------------------------------------------
 * Goertzel
 * --------------------------------------------------------------------- */

/* Con -std=c11 no se expone M_PI: es una extensión X/Open, no C estándar. */
#ifndef MIC_PI
#define MIC_PI 3.14159265358979323846f
#endif

/* Por debajo de unas pocas muestras la recurrencia no llega a formar la
 * ventana y el resultado no significa nada. */
#define MIC_GOERTZEL_WINDOW_MIN 8u

void mic_goertzel_init(mic_goertzel_t *g, float freq_hz, float sample_hz, size_t window)
{
    if (g == NULL) {
        return;
    }

    if (window < MIC_GOERTZEL_WINDOW_MIN) {
        window = MIC_GOERTZEL_WINDOW_MIN;
    }

    /* Fuera de [0, fs/2] no hay nada que medir: por encima de Nyquist la
     * frecuencia se pliega y el resultado mediría otra cosa sin avisar. */
    if (!(sample_hz > 0.0f)) {
        sample_hz = 1.0f;
    }
    if (freq_hz < 0.0f) {
        freq_hz = 0.0f;
    }
    const float nyquist = sample_hz * 0.5f;
    if (freq_hz > nyquist) {
        freq_hz = nyquist;
    }

    /* El índice de bin puede ser fraccionario: no se redondea a propósito.
     * Redondear desplazaría el punto evaluado hasta medio bin y restaría
     * hasta 3,9 dB por pérdida de festoneado justo en la medida que importa. */
    g->coeff  = 2.0f * cosf(2.0f * MIC_PI * freq_hz / sample_hz);
    g->window = window;
    mic_goertzel_reset(g);
}

void mic_goertzel_reset(mic_goertzel_t *g)
{
    if (g == NULL) {
        return;
    }
    g->s1 = 0.0f;
    g->s2 = 0.0f;
    g->n  = 0;
}

void mic_goertzel_push_block(mic_goertzel_t *g, const int16_t *pcm, size_t n)
{
    if (g == NULL || pcm == NULL) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        mic_goertzel_push(g, pcm[i]);
    }
}

float mic_goertzel_rms(const mic_goertzel_t *g)
{
    if (g == NULL || g->n == 0) {
        return 0.0f;
    }

    /* |X|^2 = s1^2 + s2^2 - coeff*s1*s2. La identidad no supone que el índice
     * de bin sea entero, así que vale igual para frecuencias arbitrarias. */
    float power = g->s1 * g->s1 + g->s2 * g->s2 - g->coeff * g->s1 * g->s2;

    /* La resta puede dar un negativo diminuto por redondeo cuando la señal es
     * casi nula. Recortar a cero evita un NaN en la raíz. */
    if (power < 0.0f) {
        power = 0.0f;
    }

    /* Para x[n] = A*sin(...) resulta |X| = A*N/2, luego A = 2|X|/N y el valor
     * eficaz es A/raiz(2) = |X|*raiz(2)/N. Con esa normalización el resultado
     * queda en las mismas cuentas de PCM que sqrt(sum_sq/n) de mic_dsp_analyze,
     * y los dos números se pueden comparar directamente.
     *
     * Se normaliza por las muestras acumuladas, no por la ventana nominal, para
     * que una ventana incompleta siga dando una amplitud con sentido. */
    return sqrtf(power) * 1.41421356f / (float)g->n;
}

float mic_goertzel_block(const int16_t *pcm, size_t n, float freq_hz, float sample_hz)
{
    mic_goertzel_t g;
    mic_goertzel_init(&g, freq_hz, sample_hz, n);
    mic_goertzel_push_block(&g, pcm, n);
    return mic_goertzel_rms(&g);
}
