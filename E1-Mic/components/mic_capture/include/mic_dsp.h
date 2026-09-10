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

/*
 * ------------------------------------------------------------------------
 * Goertzel: nivel de la señal en una sola frecuencia.
 * ------------------------------------------------------------------------
 *
 * El RMS de mic_dsp_analyze mide TODO lo que entra: el motor paso a paso, los
 * ventiladores, el rizado de conmutación de la fuente y el tono del tubo, todo
 * sumado. Como la frecuencia de excitación la fija E2 y por tanto se conoce,
 * medir sólo esa componente rechaza el resto.
 *
 * Goertzel evalúa un único punto de la DFT con una recurrencia de dos estados,
 * en tiempo lineal y sin buffer intermedio. Frente a una FFT completa evita
 * reordenar, no necesita memoria adicional y calcula sólo lo que hace falta.
 *
 * El ancho de banda de ruido equivalente es fs/window. Con 4096 muestras a
 * 44,1 kHz son 10,8 Hz frente a los 22 kHz del RMS de banda ancha: unos 33 dB
 * de mejora en relación señal a ruido para ruido blanco.
 *
 * Se usa float y no punto fijo porque el ESP32 tiene FPU de precisión simple por
 * hardware. Con double la FPU no sirve y se emula por software.
 *
 * La frecuencia NO tiene por qué caer en un bin exacto: el coeficiente admite
 * un índice fraccionario y la fórmula de potencia sigue siendo válida. Eso
 * importa porque E2 fija frecuencias arbitrarias, no múltiplos de fs/window.
 */
typedef struct {
    float  coeff;   /* 2*cos(2*pi*f/fs); fija la frecuencia evaluada. */
    float  s1;      /* Estado de la recurrencia, retardo de una muestra. */
    float  s2;      /* Estado de la recurrencia, retardo de dos muestras. */
    size_t n;       /* Muestras acumuladas en la ventana en curso. */
    size_t window;  /* Muestras por ventana. */
} mic_goertzel_t;

/* Ventana por defecto: 4096 muestras son 93 ms a 44,1 kHz y unos 10,8 Hz de
 * ancho de bin. Suficientemente corta para que el émbolo no se desplace de
 * forma apreciable durante la medida, y suficientemente larga para promediar
 * unos 100 ciclos a 1 kHz. */
#define MIC_GOERTZEL_WINDOW_DEFAULT 4096u

/**
 * @brief Prepara el detector para una frecuencia concreta.
 *
 * Reinicia el estado y el contador de muestras. Hay que volver a llamarla cada
 * vez que E2 cambia la frecuencia de excitación.
 *
 * @param g            Instancia (no puede ser NULL).
 * @param freq_hz      Frecuencia a medir. Se acota a [0, fs/2].
 * @param sample_hz    Frecuencia de muestreo del ADC.
 * @param window       Muestras por ventana; se acota a un mínimo de 8.
 */
void mic_goertzel_init(mic_goertzel_t *g, float freq_hz, float sample_hz, size_t window);

/** @brief Vacía el estado y empieza una ventana nueva sin recalcular el coeficiente. */
void mic_goertzel_reset(mic_goertzel_t *g);

/** @brief Introduce una muestra PCM. Es el camino crítico: tres operaciones. */
static inline void mic_goertzel_push(mic_goertzel_t *g, int16_t sample)
{
    const float s0 = (float)sample + g->coeff * g->s1 - g->s2;
    g->s2 = g->s1;
    g->s1 = s0;
    g->n++;
}

/** @brief Introduce un bloque completo de PCM. */
void mic_goertzel_push_block(mic_goertzel_t *g, const int16_t *pcm, size_t n);

/** @brief true cuando ya hay una ventana entera acumulada. */
static inline bool mic_goertzel_ready(const mic_goertzel_t *g)
{
    return g->n >= g->window;
}

/**
 * @brief Valor eficaz de la componente medida, en cuentas de PCM.
 *
 * Devuelve la MISMA unidad que sqrt(sum_sq / n) de mic_dsp_analyze, a propósito:
 * para un tono puro sin ruido los dos coinciden, y el cociente entre ambos dice
 * qué fracción del nivel es realmente el tono. Ese cociente es el diagnóstico
 * que distingue "hay señal" de "hay ruido".
 *
 * Normaliza por las muestras realmente acumuladas, así que una ventana a medias
 * da un resultado válido aunque más ruidoso.
 */
float mic_goertzel_rms(const mic_goertzel_t *g);

/**
 * @brief Atajo de una sola llamada sobre un bloque ya completo.
 *
 * Equivale a init, push_block y rms. Pensado para los tests y para código que ya
 * tiene la ventana entera en memoria; el camino del firmware usa la versión por
 * bloques, porque el DMA entrega tramas, no ventanas.
 */
float mic_goertzel_block(const int16_t *pcm, size_t n, float freq_hz, float sample_hz);

#ifdef __cplusplus
}
#endif
