/*
 * mic_dsp.h - Conversión de muestras del micrófono del tubo de Kundt (E1).
 *
 * Sin dependencias de ESP-IDF a propósito: el firmware y los tests de host
 * (test/host/) compilan exactamente la misma unidad de traducción.
 *
 * El SAR ADC del ESP32 en modo continuo (DMA) entrega palabras de 16 bits en
 * formato TYPE1: dato en los bits 0..11, canal en los bits 12..15. Una trama DMA
 * puede intercalar palabras de otros canales, así que el campo de canal se
 * comprueba, no se asume.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SAR ADC de 12 bits: la muestra cruda va de 0 a 4095 y el silencio nominal cae
 * a media escala. */
#define MIC_ADC_RAW_MAX      4095u
#define MIC_ADC_RAW_MIDPOINT 2048u

/* De 12 a 16 bits: ya centrada, la señal abarca ±2048 cuentas, y ±2048 × 16 es
 * justo el rango de int16. */
#define MIC_DSP_GAIN 16

/*
 * Estimador del offset de continua.
 *
 * El frontend analógico (electret -> preamplificador -> LM324) acopla en alterna
 * sobre un punto de operación en continua: las muestras crudas son unipolares y
 * están centradas en ese offset, no en cero. Emitirlas tal cual produce un PCM
 * desplazado media escala, que se oye como saturación. Este estimador mide el
 * offset y lo resta, dejando PCM con signo convencional.
 *
 * Con track == false el offset queda fijo en MIC_ADC_RAW_MIDPOINT, lo que sólo
 * es correcto si la polarización del hardware cae exactamente a media escala.
 */
typedef struct {
    int32_t dc_q16;   /* Offset estimado en Q16.16, en cuentas del ADC. */
    uint8_t shift;    /* Suavizado: mayor = seguimiento más lento y más estable. */
    bool    track;    /* false fija el offset a media escala. */
} mic_dsp_t;

/* Suavizado por defecto: memoria de ~2^10 muestras (~23 ms a 44,1 kHz). Rápido
 * para asentarse al arrancar, y muy por debajo de la banda de audio, así que no
 * atenúa las bajas frecuencias de interés. */
#define MIC_DSP_DC_SHIFT_DEFAULT 10

/**
 * @brief Inicializa el conversor.
 * @param dsp    Instancia a inicializar (no puede ser NULL).
 * @param track  Seguir el offset medido en vez de suponer media escala.
 * @param shift  Suavizado; se acota a 1..30. Se ignora si track == false.
 */
void mic_dsp_init(mic_dsp_t *dsp, bool track, uint8_t shift);

/** @brief Extrae el valor de 12 bits de una palabra cruda del DMA. */
static inline uint16_t mic_word_value(uint16_t word)
{
    return (uint16_t)(word & 0x0FFFu);
}

/** @brief Extrae el índice de canal de una palabra cruda del DMA. */
static inline uint8_t mic_word_channel(uint16_t word)
{
    return (uint8_t)((word >> 12) & 0x0Fu);
}

/**
 * @brief Convierte una muestra cruda a PCM de 16 bits con signo, actualizando el
 *        offset estimado.
 *
 * Los valores sobre MIC_ADC_RAW_MAX se recortan, y el resultado satura en los
 * límites de int16 en vez de desbordar.
 */
int16_t mic_dsp_convert(mic_dsp_t *dsp, uint16_t raw12);

/**
 * @brief Procesa una trama DMA: filtra por canal, convierte y cuenta descartes.
 *
 * Es el camino crítico. Las palabras cuyo campo de canal no coincide con
 * @p channel se descartan: pertenecen a otro patrón de conversión y no deben
 * entrar al flujo de audio.
 *
 * @param dsp      Estado del conversor.
 * @param words    Palabras crudas de 16 bits tal como salen del buffer DMA.
 * @param n_words  Palabras disponibles en @p words.
 * @param channel  Canal del ADC que se conserva.
 * @param out      Buffer PCM de destino.
 * @param out_cap  Capacidad de @p out, en muestras.
 * @param dropped  Opcional; recibe cuántas palabras rechazó el filtro de canal.
 *                 NULL si no interesa.
 * @return Muestras PCM escritas en @p out.
 */
size_t mic_dsp_process(mic_dsp_t *dsp,
                       const uint16_t *words,
                       size_t          n_words,
                       uint8_t         channel,
                       int16_t        *out,
                       size_t          out_cap,
                       uint32_t       *dropped);

/** @brief Offset de continua estimado, en cuentas del ADC. */
int32_t mic_dsp_dc_offset(const mic_dsp_t *dsp);

/** @brief Resumen de un bloque PCM. Basta para distinguir señal de un pin al aire. */
typedef struct {
    int16_t  min;
    int16_t  max;
    uint64_t sum_sq;  /* Suma de cuadrados; RMS = sqrt(sum_sq / n). */
    size_t   n;
} mic_pcm_stats_t;

/**
 * @brief Mide un bloque PCM: mínimo, máximo y suma de cuadrados.
 *
 * Vive aquí y no en mic_capture para seguir libre de ESP-IDF y poder probarse en
 * el host junto al resto de la conversión.
 */
void mic_dsp_analyze(const int16_t *pcm, size_t n, mic_pcm_stats_t *out);

#ifdef __cplusplus
}
#endif
