/*
 * mic_capture.c - ver mic_capture.h.
 */

#include "mic_capture.h"
#include "mic_dsp.h"

#include <math.h>
#include <string.h>

#include "esp_adc/adc_continuous.h"
#include "esp_log.h"
#include "soc/soc_caps.h"

static const char *TAG = "mic_capture";

/*
 * El driver devuelve palabras DMA crudas; en ESP32 cada resultado es una palabra
 * TYPE1 de 16 bits (valor de 12 bits, canal de 4). mic_dsp las decodifica con
 * desplazamientos simples para poder probarse en host, así que aquí se
 * verifica que el formato que reporta el SDK sigue siendo el que mic_dsp supone.
 */
_Static_assert(SOC_ADC_DIGI_RESULT_BYTES == sizeof(uint16_t),
               "mic_dsp decodifica palabras de ADC de 16 bits; este chip usa otro ancho");
_Static_assert(sizeof(adc_digi_output_data_t) == sizeof(uint16_t),
               "en ESP32 adc_digi_output_data_t debe ser una palabra de 16 bits");

typedef struct {
    adc_continuous_handle_t handle;
    mic_capture_config_t    cfg;
    mic_dsp_t               dsp;
    uint8_t                *raw;        /* Espacio de trabajo para una trama DMA. */
    size_t                  raw_bytes;
    mic_capture_stats_t     stats;
    bool                    running;
} mic_capture_ctx_t;

static mic_capture_ctx_t s_ctx;

/* Corre en contexto de ISR: sólo incrementa un contador. Un pool lleno significa
 * que el consumidor no vacía a tiempo y se descartaron muestras. */
static bool IRAM_ATTR on_pool_ovf(adc_continuous_handle_t handle,
                                 const adc_continuous_evt_data_t *edata,
                                 void *user_data)
{
    (void)handle;
    (void)edata;
    mic_capture_ctx_t *ctx = (mic_capture_ctx_t *)user_data;
    ctx->stats.pool_overflows++;
    return false;
}

esp_err_t mic_capture_validate(const mic_capture_config_t *cfg)
{
    if (cfg == NULL) {
        ESP_LOGE(TAG, "la configuración es NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (cfg->sample_rate_hz < SOC_ADC_SAMPLE_FREQ_THRES_LOW ||
        cfg->sample_rate_hz > SOC_ADC_SAMPLE_FREQ_THRES_HIGH) {
        ESP_LOGE(TAG, "sample_rate_hz %lu fuera del rango soportado %d..%d",
                 (unsigned long)cfg->sample_rate_hz,
                 SOC_ADC_SAMPLE_FREQ_THRES_LOW,
                 SOC_ADC_SAMPLE_FREQ_THRES_HIGH);
        return ESP_ERR_INVALID_ARG;
    }

    if (cfg->frame_samples == 0 || cfg->frame_count == 0) {
        ESP_LOGE(TAG, "frame_samples y frame_count no pueden ser cero");
        return ESP_ERR_INVALID_ARG;
    }

    /* El driver continuo exige que la trama de conversión sea múltiplo de
     * SOC_ADC_DIGI_DATA_BYTES_PER_CONV (aquí 4 bytes, aunque un resultado ocupe
     * sólo 2). Confundir ambas constantes es el error de dimensionamiento
     * clásico de esta API. */
    const size_t frame_bytes = (size_t)cfg->frame_samples * SOC_ADC_DIGI_RESULT_BYTES;
    if (frame_bytes % SOC_ADC_DIGI_DATA_BYTES_PER_CONV != 0) {
        ESP_LOGE(TAG,
                 "trama de %u muestras = %u bytes, no es múltiplo de %d",
                 (unsigned)cfg->frame_samples,
                 (unsigned)frame_bytes,
                 SOC_ADC_DIGI_DATA_BYTES_PER_CONV);
        return ESP_ERR_INVALID_ARG;
    }

    if (cfg->adc_channel >= SOC_ADC_CHANNEL_NUM(ADC_UNIT_1)) {
        ESP_LOGE(TAG, "canal %d fuera de rango para ADC1", (int)cfg->adc_channel);
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

esp_err_t mic_capture_start(const mic_capture_config_t *cfg)
{
    esp_err_t err = mic_capture_validate(cfg);
    if (err != ESP_OK) {
        return err;
    }

    if (s_ctx.running) {
        ESP_LOGW(TAG, "ya está corriendo");
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.cfg       = *cfg;
    s_ctx.raw_bytes = (size_t)cfg->frame_samples * SOC_ADC_DIGI_RESULT_BYTES;

    s_ctx.raw = calloc(1, s_ctx.raw_bytes);
    if (s_ctx.raw == NULL) {
        ESP_LOGE(TAG, "no se pudo reservar el buffer DMA de %u bytes",
                 (unsigned)s_ctx.raw_bytes);
        return ESP_ERR_NO_MEM;
    }

    mic_dsp_init(&s_ctx.dsp, cfg->track_dc, cfg->dc_shift);

    const adc_continuous_handle_cfg_t hdl_cfg = {
        .max_store_buf_size = s_ctx.raw_bytes * cfg->frame_count,
        .conv_frame_size    = s_ctx.raw_bytes,
        .flags              = { .flush_pool = true },
    };

    err = adc_continuous_new_handle(&hdl_cfg, &s_ctx.handle);
    if (err != ESP_OK) {
        /* Un ESP_ERR_NOT_FOUND aquí significa que I2S0 ya está tomado: en ESP32
         * el DMA del ADC se construye sobre él, así que nadie más puede usarlo. */
        ESP_LOGE(TAG, "adc_continuous_new_handle falló: %s", esp_err_to_name(err));
        free(s_ctx.raw);
        s_ctx.raw = NULL;
        return err;
    }

    adc_digi_pattern_config_t pattern = {
        .atten     = (uint8_t)cfg->atten,
        .channel   = (uint8_t)(cfg->adc_channel & 0x7),
        .unit      = (uint8_t)ADC_UNIT_1,
        .bit_width = SOC_ADC_DIGI_MAX_BITWIDTH,
    };

    const adc_continuous_config_t dig_cfg = {
        .pattern_num    = 1,
        .adc_pattern    = &pattern,
        .sample_freq_hz = cfg->sample_rate_hz,
        .conv_mode      = ADC_CONV_SINGLE_UNIT_1,
        .format         = ADC_DIGI_OUTPUT_FORMAT_TYPE1,
    };

    err = adc_continuous_config(s_ctx.handle, &dig_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_continuous_config falló: %s", esp_err_to_name(err));
        goto fail;
    }

    const adc_continuous_evt_cbs_t cbs = { .on_pool_ovf = on_pool_ovf };
    err = adc_continuous_register_event_callbacks(s_ctx.handle, &cbs, &s_ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no se pudieron registrar los callbacks: %s", esp_err_to_name(err));
        goto fail;
    }

    err = adc_continuous_start(s_ctx.handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_continuous_start falló: %s", esp_err_to_name(err));
        goto fail;
    }

    s_ctx.running = true;
    ESP_LOGI(TAG,
             "iniciado: %lu Hz, %u muestras/trama, %u tramas, ADC1_CH%d, seguimiento DC %s",
             (unsigned long)cfg->sample_rate_hz,
             (unsigned)cfg->frame_samples,
             (unsigned)cfg->frame_count,
             (int)cfg->adc_channel,
             cfg->track_dc ? "activo" : "inactivo");
    return ESP_OK;

fail:
    adc_continuous_deinit(s_ctx.handle);
    s_ctx.handle = NULL;
    free(s_ctx.raw);
    s_ctx.raw = NULL;
    return err;
}

esp_err_t mic_capture_stop(void)
{
    if (!s_ctx.running) {
        return ESP_OK;
    }

    s_ctx.running = false;
    adc_continuous_stop(s_ctx.handle);
    adc_continuous_deinit(s_ctx.handle);
    s_ctx.handle = NULL;

    free(s_ctx.raw);
    s_ctx.raw = NULL;

    ESP_LOGI(TAG, "detenido");
    return ESP_OK;
}

bool mic_capture_is_running(void)
{
    return s_ctx.running;
}

esp_err_t mic_capture_read(int16_t *out,
                           size_t   out_cap,
                           size_t  *out_samples,
                           uint32_t timeout_ms)
{
    if (out == NULL || out_samples == NULL || out_cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_samples = 0;

    if (!s_ctx.running) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Nunca pedir más de lo que caben el buffer de trabajo o el del llamador. */
    uint32_t want = (uint32_t)s_ctx.raw_bytes;
    const uint32_t cap_bytes = (uint32_t)(out_cap * SOC_ADC_DIGI_RESULT_BYTES);
    if (cap_bytes < want) {
        want = cap_bytes - (cap_bytes % SOC_ADC_DIGI_DATA_BYTES_PER_CONV);
        if (want == 0) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    uint32_t got = 0;
    esp_err_t err = adc_continuous_read(s_ctx.handle, s_ctx.raw, want, &got, timeout_ms);
    if (err != ESP_OK) {
        return err;  /* ESP_ERR_TIMEOUT es normal con el tubo en silencio. */
    }

    uint32_t dropped = 0;
    const size_t written = mic_dsp_process(&s_ctx.dsp,
                                           (const uint16_t *)s_ctx.raw,
                                           got / SOC_ADC_DIGI_RESULT_BYTES,
                                           (uint8_t)s_ctx.cfg.adc_channel,
                                           out,
                                           out_cap,
                                           &dropped);

    s_ctx.stats.samples_captured += written;
    s_ctx.stats.words_dropped    += dropped;
    s_ctx.stats.dc_offset         = mic_dsp_dc_offset(&s_ctx.dsp);

    /* Cifras por bloque: sin nada conectado al pin del ADC muestran el ruido que
     * capta una entrada al aire, que es justamente cómo se distingue una señal
     * real de un pin desconectado. */
    mic_pcm_stats_t pcm;
    mic_dsp_analyze(out, written, &pcm);
    s_ctx.stats.pcm_min = pcm.min;
    s_ctx.stats.pcm_max = pcm.max;
    s_ctx.stats.pcm_rms = (pcm.n > 0)
        ? (uint32_t)sqrt((double)pcm.sum_sq / (double)pcm.n)
        : 0;

    *out_samples = written;
    return ESP_OK;
}

void mic_capture_get_stats(mic_capture_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }
    *stats = s_ctx.stats;
    stats->dc_offset = mic_dsp_dc_offset(&s_ctx.dsp);
}
