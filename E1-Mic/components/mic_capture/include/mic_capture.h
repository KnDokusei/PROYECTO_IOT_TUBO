/*
 * mic_capture.h - Adquisición continua del micrófono del tubo de Kundt (E1).
 *
 * Reemplaza a la versión Arduino, que manejaba el SAR ADC a través del driver
 * I2S antiguo (`I2S_MODE_ADC_BUILT_IN`). Ese modo quedó obsoleto en ESP-IDF 5.0
 * y fue eliminado en 6.0; `esp_adc/adc_continuous.h` es el reemplazo soportado y
 * mueve el mismo hardware (en este chip el DMA del ADC sigue usando I2S0 por
 * debajo).
 *
 * Hardware: micrófono electret -> preamplificador -> LM324 -> GPIO34 (ADC1 canal 6).
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
 * Parámetros ajustables. Corresponden uno a uno con las "Variables de interés"
 * que el README original lista para E1; se renombraron porque aquí ya no hay I2S:
 *
 *   I2S_SAMPLE_RATE    -> sample_rate_hz
 *   I2S_DMA_BUF_LEN    -> frame_samples
 *   I2S_DMA_BUF_COUNT  -> frame_count
 */
typedef struct {
    uint32_t     sample_rate_hz;  /* Debe caer dentro del rango del ADC del chip. */
    uint16_t     frame_samples;   /* Muestras por trama de conversión DMA. */
    uint8_t      frame_count;     /* Tramas que mantiene el pool del driver. */
    adc_channel_t adc_channel;    /* Canal de ADC1; en ESP32 el 6 es GPIO34. */
    adc_atten_t  atten;           /* Atenuación de entrada (fija el rango de tensión). */
    bool         track_dc;        /* Seguir la continua medida (recomendado). */
    uint8_t      dc_shift;        /* Suavizado del estimador de continua. */
} mic_capture_config_t;

/*
 * Los valores por defecto conservan los de la versión Arduino: 44,1 kHz, tramas
 * de 512 muestras, 10 tramas en vuelo, ADC1_CH6 (GPIO34).
 *
 * ADC_ATTEN_DB_12 aplica la mayor atenuación disponible (11 dB reales); según la
 * documentación de Espressif para ESP32 no admite entradas sobre 2800 mV. El
 * frontend del esquemático se alimenta de 5 V, y las "Mejoras Futuras" del
 * README piden rediseñarlo a 3,3 V con punto de operación en 1,65 V. Hasta que
 * eso ocurra, los pasajes fuertes empujan la entrada fuera de rango y saturan.
 * Es una limitación del lado analógico: el firmware no puede corregirla.
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

/** @brief Contadores de ejecución; delatan pérdidas sin necesidad de osciloscopio. */
typedef struct {
    uint64_t samples_captured;  /* Muestras PCM entregadas al llamador. */
    uint64_t words_dropped;     /* Palabras DMA rechazadas por el filtro de canal. */
    uint32_t pool_overflows;    /* El pool del driver se llenó: se perdieron muestras. */
    int32_t  dc_offset;         /* Offset de continua actual, en cuentas del ADC. */
    int16_t  pcm_min;           /* Mínimo PCM del último bloque. */
    int16_t  pcm_max;           /* Máximo PCM del último bloque. */
    uint32_t pcm_rms;           /* RMS del último bloque. */
} mic_capture_stats_t;

/**
 * @brief Valida una configuración sin arrancar el driver.
 *
 * Separada de mic_capture_start() para que los tests puedan ejercitarla.
 * Comprueba la frecuencia de muestreo contra el rango del chip y que el tamaño
 * de trama resultante sea múltiplo de SOC_ADC_DIGI_DATA_BYTES_PER_CONV, la regla
 * de alineación que impone el driver continuo.
 *
 * @return ESP_OK, o ESP_ERR_INVALID_ARG dejando el motivo en el log.
 */
esp_err_t mic_capture_validate(const mic_capture_config_t *cfg);

/** @brief Configura y arranca la adquisición continua. */
esp_err_t mic_capture_start(const mic_capture_config_t *cfg);

/** @brief Detiene la adquisición y libera el driver. Seguro si ya está detenida. */
esp_err_t mic_capture_stop(void);

/** @brief Indica si la adquisición está corriendo. */
bool mic_capture_is_running(void);

/**
 * @brief Lee un bloque de PCM de 16 bits con signo.
 *
 * Bloquea hasta que haya datos o hasta que venzan @p timeout_ms.
 *
 * @param out          Buffer de destino.
 * @param out_cap      Capacidad de @p out, en muestras.
 * @param out_samples  Recibe la cantidad de muestras escritas.
 * @param timeout_ms   Límite de bloqueo en milisegundos.
 * @return ESP_OK; ESP_ERR_TIMEOUT si no llegaron datos; ESP_ERR_INVALID_STATE si
 *         no se ha arrancado; o ESP_ERR_INVALID_ARG.
 */
esp_err_t mic_capture_read(int16_t *out,
                           size_t   out_cap,
                           size_t  *out_samples,
                           uint32_t timeout_ms);

/** @brief Toma una instantánea de los contadores. */
void mic_capture_get_stats(mic_capture_stats_t *stats);

#ifdef __cplusplus
}
#endif
