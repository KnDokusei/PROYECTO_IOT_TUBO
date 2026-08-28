/*
 * mic_dsp.c - ver mic_dsp.h.
 *
 * Sin cabeceras de ESP-IDF a propósito: este archivo se compila tanto en el
 * firmware como en los tests de host.
 */

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
