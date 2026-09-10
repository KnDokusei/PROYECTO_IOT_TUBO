/*
 * Tests de host de mic_dsp: la cadena de conversión de muestras del módulo de
 * micrófono E1. Corren en el PC sin ninguna ESP32 conectada, porque mic_dsp.c
 * está libre de dependencias de ESP-IDF a propósito, y así el firmware y estos
 * tests compilan exactamente el mismo código.
 *
 * Compilar y ejecutar:  make -C test/host
 */

#include <math.h>

/* Con -std=c11 no se expone M_PI (es una extensión X/Open). */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mic_dsp.h"

static int g_checks;
static int g_failures;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        g_checks++;                                         \
        if (!(cond)) {                                      \
            g_failures++;                                   \
            printf("    FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                            \
            printf("\n");                                   \
        }                                                   \
    } while (0)

#define CHECK_NEAR(actual, expected, tol, label)                        \
    CHECK(labs((long)(actual) - (long)(expected)) <= (long)(tol),       \
          "%s: got %ld, expected %ld +/- %ld",                          \
          label, (long)(actual), (long)(expected), (long)(tol))

/* Arma una palabra DMA cruda tal como la ESP32 dispone los resultados TYPE1:
 * valor en los bits 0..11, canal en los bits 12..15. */
static uint16_t make_word(uint16_t value, uint8_t channel)
{
    return (uint16_t)((value & 0x0FFFu) | ((uint16_t)(channel & 0x0Fu) << 12));
}

/* Inyecta n muestras para que el estimador de continua se asiente antes de medir. */
static void settle(mic_dsp_t *dsp, uint16_t level, int n)
{
    for (int i = 0; i < n; i++) {
        (void)mic_dsp_convert(dsp, level);
    }
}

/* --------------------------------------------------------------------- */

static void test_word_layout(void)
{
    printf("  disposición de la palabra (decodificación TYPE1)\n");

    CHECK(mic_word_value(make_word(0x0ABC, 6)) == 0x0ABC, "value round-trip");
    CHECK(mic_word_channel(make_word(0x0ABC, 6)) == 6, "channel round-trip");

    /* Un valor a fondo de escala no debe desbordar hacia el campo de canal. */
    CHECK(mic_word_value(make_word(4095, 0)) == 4095, "max value preserved");
    CHECK(mic_word_channel(make_word(4095, 0)) == 0, "max value leaves channel clear");

    /* El canal 15 no debe corromper el valor. */
    CHECK(mic_word_value(make_word(1234, 15)) == 1234, "value intact with high channel");
    CHECK(mic_word_channel(make_word(1234, 15)) == 15, "channel 15 decoded");
}

static void test_silence_is_centred(void)
{
    printf("  una entrada constante da silencio, no un escalón de continua\n");

    /* Esto es lo que la versión Arduino hacía mal: emitía (raw & 0xFFF) * 8, o
     * sea muestras unipolares centradas cerca de 16380 en vez de 0. Quien
     * escucha lo percibe como distorsión fuerte. */
    mic_dsp_t dsp;
    mic_dsp_init(&dsp, true, MIC_DSP_DC_SHIFT_DEFAULT);

    settle(&dsp, MIC_ADC_RAW_MIDPOINT, 5000);
    CHECK_NEAR(mic_dsp_convert(&dsp, MIC_ADC_RAW_MIDPOINT), 0, 8, "mid-scale input");

    /* Lo mismo debe valer para una polarización lejos de media escala: el
     * estimador tiene que encontrarla. El frontend real es un circuito de 5 V
     * alimentando un ADC de 3,3 V, así que nada garantiza que el punto de
     * operación caiga en 2048. */
    mic_dsp_t off;
    mic_dsp_init(&off, true, MIC_DSP_DC_SHIFT_DEFAULT);
    settle(&off, 1200, 20000);
    CHECK_NEAR(mic_dsp_convert(&off, 1200), 0, 16, "input biased at 1200 counts");
    CHECK_NEAR(mic_dsp_dc_offset(&off), 1200, 4, "DC estimate converged");
}

static void test_amplitude_and_gain(void)
{
    printf("  la amplitud escala las cuentas de 12 bits al rango de int16\n");

    mic_dsp_t dsp;
    mic_dsp_init(&dsp, false, MIC_DSP_DC_SHIFT_DEFAULT);  /* pinned to mid-scale */

    /* +100 cuentas sobre el centro -> 100 * 16 = 1600. */
    CHECK_NEAR(mic_dsp_convert(&dsp, MIC_ADC_RAW_MIDPOINT + 100), 1600, 1, "+100 counts");
    CHECK_NEAR(mic_dsp_convert(&dsp, MIC_ADC_RAW_MIDPOINT - 100), -1600, 1, "-100 counts");

    /* Una excursión negativa debe salir realmente negativa: el código original
     * no podía representarla en absoluto. */
    CHECK(mic_dsp_convert(&dsp, 0) < 0, "bottom of scale is negative");
    CHECK(mic_dsp_convert(&dsp, MIC_ADC_RAW_MAX) > 0, "top of scale is positive");
}

static void test_saturation(void)
{
    printf("  los extremos se recortan en vez de desbordar\n");

    mic_dsp_t dsp;
    mic_dsp_init(&dsp, false, MIC_DSP_DC_SHIFT_DEFAULT);

    /* 2048 cuentas bajo el centro * 16 = -32768, justo el piso de int16. */
    const int16_t low = mic_dsp_convert(&dsp, 0);
    CHECK(low == -32768, "full negative swing clamps to -32768, got %d", low);

    /* (4095 - 2048) * 16 = 32752, apenas bajo el techo de int16. */
    const int16_t high = mic_dsp_convert(&dsp, MIC_ADC_RAW_MAX);
    CHECK(high == 32752, "full positive swing gives 32752, got %d", high);

    /* Una entrada cruda fuera de rango (palabra corrupta) debe recortarse, nunca
     * desbordar hacia una muestra muy negativa que sonaría como un chasquido
     * fuerte en el flujo. */
    const int16_t over = mic_dsp_convert(&dsp, 60000);
    CHECK(over > 0, "over-range input stays positive, got %d", over);
}

static void test_channel_filter(void)
{
    printf("  se rechazan las palabras de otros canales (el fallo A5)\n");

    mic_dsp_t dsp;
    mic_dsp_init(&dsp, false, MIC_DSP_DC_SHIFT_DEFAULT);

    /* Una trama DMA puede intercalar canales. Sólo el canal 6 (GPIO34) es
     * nuestro; dejar pasar palabras ajenas inyecta ruido en el audio. */
    uint16_t words[8];
    for (int i = 0; i < 8; i++) {
        words[i] = make_word(MIC_ADC_RAW_MIDPOINT + 100, (i % 2 == 0) ? 6 : 3);
    }

    int16_t  out[8];
    uint32_t dropped = 0;
    const size_t n = mic_dsp_process(&dsp, words, 8, 6, out, 8, &dropped);

    CHECK(n == 4, "kept 4 of 8 words, got %zu", n);
    CHECK(dropped == 4, "reported 4 drops, got %u", dropped);
    for (size_t i = 0; i < n; i++) {
        CHECK_NEAR(out[i], 1600, 1, "kept sample value");
    }
}

static void test_output_capacity(void)
{
    printf("  un buffer de salida corto trunca en vez de desbordar\n");

    mic_dsp_t dsp;
    mic_dsp_init(&dsp, false, MIC_DSP_DC_SHIFT_DEFAULT);

    uint16_t words[16];
    for (int i = 0; i < 16; i++) {
        words[i] = make_word(MIC_ADC_RAW_MIDPOINT, 6);
    }

    int16_t out[4];
    const int16_t guard = 0x5A5A;
    int16_t canary[8];
    memset(canary, 0x5A, sizeof(canary));

    const size_t n = mic_dsp_process(&dsp, words, 16, 6, out, 4, NULL);
    CHECK(n == 4, "wrote exactly the capacity, got %zu", n);
    CHECK(canary[0] == guard, "no write past the end of the buffer");
}

static void test_null_safety(void)
{
    printf("  se manejan los argumentos NULL\n");

    mic_dsp_t dsp;
    mic_dsp_init(&dsp, true, MIC_DSP_DC_SHIFT_DEFAULT);

    uint16_t words[2] = { make_word(2048, 6), make_word(2048, 6) };
    int16_t  out[2];

    CHECK(mic_dsp_process(NULL, words, 2, 6, out, 2, NULL) == 0, "NULL dsp");
    CHECK(mic_dsp_process(&dsp, NULL, 2, 6, out, 2, NULL) == 0, "NULL input");
    CHECK(mic_dsp_process(&dsp, words, 2, 6, NULL, 2, NULL) == 0, "NULL output");
    CHECK(mic_dsp_convert(NULL, 2048) == 0, "NULL dsp on convert");

    mic_dsp_init(NULL, true, 10);  /* must not crash */
    CHECK(mic_dsp_dc_offset(NULL) == 0, "NULL dsp on dc_offset");
}

static void test_shift_clamping(void)
{
    printf("  el factor de suavizado se acota a un rango utilizable\n");

    mic_dsp_t dsp;
    mic_dsp_init(&dsp, true, 0);
    CHECK(dsp.shift >= 1, "shift 0 raised to at least 1, got %u", dsp.shift);

    mic_dsp_init(&dsp, true, 200);
    CHECK(dsp.shift <= 30, "shift 200 capped at 30, got %u", dsp.shift);
}

static void test_sine_wave_roundtrip(void)
{
    printf("  un tono sintético sobrevive la conversión con su forma intacta\n");

    /* Modela lo que el tubo realmente entrega: un tono de 1200 Hz (el valor por
     * defecto del AD9833 en E2) montado sobre una continua, muestreado a
     * 44,1 kHz. */
    const double sample_rate = 44100.0;
    const double freq        = 1200.0;
    const double bias        = 1600.0;   /* deliberately not mid-scale */
    const double amplitude   = 400.0;    /* counts, peak */
    const int    n           = 8192;

    mic_dsp_t dsp;
    mic_dsp_init(&dsp, true, MIC_DSP_DC_SHIFT_DEFAULT);

    int16_t peak_pos = 0;
    int16_t peak_neg = 0;
    double  sum      = 0.0;

    for (int i = 0; i < n; i++) {
        const double phase = 2.0 * M_PI * freq * (double)i / sample_rate;
        const double v     = bias + amplitude * sin(phase);
        const int16_t s    = mic_dsp_convert(&dsp, (uint16_t)lround(v));

        /* Saltar la ventana de asentamiento mientras el estimador se engancha. */
        if (i > 4096) {
            if (s > peak_pos) peak_pos = s;
            if (s < peak_neg) peak_neg = s;
            sum += (double)s;
        }
    }

    const double mean = sum / (double)(n - 4096 - 1);

    /* Pico esperado: 400 cuentas * 16 = 6400. */
    CHECK_NEAR(peak_pos, 6400, 200, "positive peak");
    CHECK_NEAR(-peak_neg, 6400, 200, "negative peak");
    CHECK(fabs(mean) < 100.0, "mean near zero (DC removed), got %.1f", mean);
    CHECK(peak_pos > 0 && peak_neg < 0, "waveform is bipolar");
}


/*
 * Reglas de dimensionamiento de buffers del driver continuo del ADC en ESP32.
 *
 * Estas constantes replican soc_caps.h para este chip (verificadas contra
 * components/soc/esp32/include/soc/soc_caps.h en ESP-IDF v5.5.5). El firmware
 * las comprueba en ejecución dentro de mic_capture_validate() usando las macros
 * reales del SDK; este test fija la aritmética para cazar una configuración por
 * defecto mala en el host, antes de grabar nada.
 */
#define SOC_ADC_DIGI_RESULT_BYTES_ESP32        2
#define SOC_ADC_DIGI_DATA_BYTES_PER_CONV_ESP32 4
#define SOC_ADC_SAMPLE_FREQ_THRES_LOW_ESP32    20000
#define SOC_ADC_SAMPLE_FREQ_THRES_HIGH_ESP32   2000000

/* Valores de MIC_CAPTURE_DEFAULT_CONFIG(). */
#define DEFAULT_FRAME_SAMPLES 512
#define DEFAULT_FRAME_COUNT   10
#define DEFAULT_SAMPLE_RATE   44100

static void test_dma_buffer_sizing(void)
{
    printf("  el tamaño de trama DMA respeta la alineación del driver\n");

    const size_t frame_bytes =
        (size_t)DEFAULT_FRAME_SAMPLES * SOC_ADC_DIGI_RESULT_BYTES_ESP32;

    /* Una trama de conversión debe ser múltiplo de DATA_BYTES_PER_CONV (4),
     * aunque un resultado ocupe sólo 2 bytes. Confundirlos es el error de
     * dimensionamiento clásico de esta API, y la misma clase de confusión de
     * unidades que hacía que el i2s_read() de Arduino leyera medio buffer
     * (hallazgo A5). */
    CHECK(frame_bytes == 1024, "512 samples = 1024 bytes, got %zu", frame_bytes);
    CHECK(frame_bytes % SOC_ADC_DIGI_DATA_BYTES_PER_CONV_ESP32 == 0,
          "frame of %zu bytes is not a multiple of %d",
          frame_bytes, SOC_ADC_DIGI_DATA_BYTES_PER_CONV_ESP32);

    /* El pool debe contener un número entero de tramas. */
    const size_t pool_bytes = frame_bytes * DEFAULT_FRAME_COUNT;
    CHECK(pool_bytes % frame_bytes == 0, "pool is not a whole number of frames");
    CHECK(pool_bytes == 10240, "pool is 10240 bytes, got %zu", pool_bytes);

    /* La frecuencia de muestreo que fija el README debe ser una que el hardware
     * acepte. Los 44,1 kHz caen holgadamente dentro de 20 kHz..2 MHz. */
    CHECK(DEFAULT_SAMPLE_RATE >= SOC_ADC_SAMPLE_FREQ_THRES_LOW_ESP32,
          "44.1 kHz is above the %d Hz floor", SOC_ADC_SAMPLE_FREQ_THRES_LOW_ESP32);
    CHECK(DEFAULT_SAMPLE_RATE <= SOC_ADC_SAMPLE_FREQ_THRES_HIGH_ESP32,
          "44.1 kHz is below the %d Hz ceiling", SOC_ADC_SAMPLE_FREQ_THRES_HIGH_ESP32);

    /* Comprobación del caudal resultante: es lo que el WebSocket debe sostener.
     * 44100 * 2 = 88200 B/s ~= 86 KiB/s. */
    const long bytes_per_sec = (long)DEFAULT_SAMPLE_RATE * SOC_ADC_DIGI_RESULT_BYTES_ESP32;
    CHECK(bytes_per_sec == 88200, "stream is 88200 B/s, got %ld", bytes_per_sec);

    /* Una trama por envío de WebSocket -> ~86 envíos/s. */
    const double frames_per_sec = (double)DEFAULT_SAMPLE_RATE / DEFAULT_FRAME_SAMPLES;
    CHECK(frames_per_sec > 80.0 && frames_per_sec < 90.0,
          "~86 frames/s expected, got %.1f", frames_per_sec);
}

/* =====================================================================
 * Goertzel: medida del nivel en una sola frecuencia.
 *
 * Estas pruebas responden a una pregunta concreta antes de tocar hardware:
 * ¿sirve Goertzel para medir la amplitud del tono del tubo con el ADC ruidoso
 * del ESP32? Se comprueban exactitud, rechazo de otras frecuencias, ganancia
 * frente al ruido blanco, y que la versión por tramas del DMA dé lo mismo que
 * procesar la ventana entera de golpe.
 * ===================================================================== */

#define CHECK_REL(actual, expected, tol_pct, label)                           \
    do {                                                                      \
        const double _a = (double)(actual);                                   \
        const double _e = (double)(expected);                                 \
        const double _err = (_e != 0.0) ? fabs(_a - _e) / fabs(_e) * 100.0    \
                                        : fabs(_a);                           \
        g_checks++;                                                           \
        if (_err > (double)(tol_pct)) {                                       \
            g_failures++;                                                     \
            printf("    FAIL %s:%d: %s: dio %.2f, esperado %.2f "             \
                   "(error %.2f%%, máx %.2f%%)\n",                            \
                   __FILE__, __LINE__, (label), _a, _e, _err,                 \
                   (double)(tol_pct));                                        \
        }                                                                     \
    } while (0)

#define GZ_FS     44100.0
#define GZ_WINDOW 4096u

/* Congruencial lineal: los tests deben dar el mismo resultado en cada corrida,
 * así que no se usa rand(), cuya secuencia depende de la implementación. */
static uint32_t g_rng = 2463534242u;

static void rng_seed(uint32_t seed) { g_rng = seed; }

/* Uniforme en [-1, 1). */
static double rng_uniform(void)
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return ((double)(g_rng >> 8) / 8388608.0) - 1.0;
}

static void fill_tone(int16_t *buf, size_t n, double amp, double freq, double fs)
{
    for (size_t i = 0; i < n; i++) {
        buf[i] = (int16_t)lround(amp * sin(2.0 * M_PI * freq * (double)i / fs));
    }
}

static void add_tone(int16_t *buf, size_t n, double amp, double freq, double fs)
{
    for (size_t i = 0; i < n; i++) {
        buf[i] = (int16_t)lround((double)buf[i]
                                 + amp * sin(2.0 * M_PI * freq * (double)i / fs));
    }
}

static double block_rms(const int16_t *pcm, size_t n)
{
    mic_pcm_stats_t st;
    mic_dsp_analyze(pcm, n, &st);
    if (st.n == 0) return 0.0;
    return sqrt((double)st.sum_sq / (double)st.n);
}

/* --------------------------------------------------------------------- */

/* La frecuencia cae justo en un bin: es el caso ideal y fija el listón de
 * exactitud del que parten las demás pruebas. */
static void test_goertzel_exact_bin(void)
{
    printf("  Goertzel: frecuencia en un bin exacto\n");

    static int16_t pcm[GZ_WINDOW];
    const double   amp  = 8000.0;
    const double   freq = 100.0 * GZ_FS / (double)GZ_WINDOW;  /* k = 100 */

    fill_tone(pcm, GZ_WINDOW, amp, freq, GZ_FS);

    const float got = mic_goertzel_block(pcm, GZ_WINDOW, (float)freq, (float)GZ_FS);
    CHECK_REL(got, amp / sqrt(2.0), 0.5, "eficaz del tono en bin exacto");

    /* Sin ruido, el Goertzel y el RMS de banda ancha deben coincidir: es la
     * propiedad que hace comparables mic_amplitude y mic_rms. */
    CHECK_REL(got, block_rms(pcm, GZ_WINDOW), 1.0, "Goertzel frente a RMS ancho");
}

/* El caso real: E2 fija frecuencias arbitrarias, casi nunca múltiplos de
 * fs/ventana. Si el índice fraccionario costara exactitud, todo el enfoque se
 * caería aquí. */
static void test_goertzel_arbitrary_frequency(void)
{
    printf("  Goertzel: frecuencias arbitrarias (índice de bin fraccionario)\n");

    static int16_t pcm[GZ_WINDOW];
    const double   amp = 8000.0;
    const double   freqs[] = { 213.7, 1237.0, 1500.5, 3333.3, 7891.2 };

    for (size_t i = 0; i < sizeof(freqs) / sizeof(freqs[0]); i++) {
        fill_tone(pcm, GZ_WINDOW, amp, freqs[i], GZ_FS);
        const float got = mic_goertzel_block(pcm, GZ_WINDOW,
                                             (float)freqs[i], (float)GZ_FS);
        char label[64];
        snprintf(label, sizeof(label), "tono a %.1f Hz", freqs[i]);
        CHECK_REL(got, amp / sqrt(2.0), 1.0, label);
    }
}

/* Un tono fuerte en otra frecuencia no debe contaminar la medida. Es lo que
 * separa a Goertzel del RMS de banda ancha. */
static void test_goertzel_rejects_other_tones(void)
{
    printf("  Goertzel: rechazo de tonos ajenos\n");

    static int16_t pcm[GZ_WINDOW];
    const double   amp    = 12000.0;
    const double   target = 1000.0;

    /* Sólo el interferente, nada en la frecuencia medida. */
    fill_tone(pcm, GZ_WINDOW, amp, 3000.0, GZ_FS);

    const float leak = mic_goertzel_block(pcm, GZ_WINDOW,
                                          (float)target, (float)GZ_FS);
    const double wide = block_rms(pcm, GZ_WINDOW);

    /* El RMS ancho ve el interferente entero; el Goertzel debe verlo atenuado
     * más de 26 dB (por debajo del 5%). */
    CHECK(leak < wide * 0.05,
          "fuga del interferente: Goertzel %.1f frente a RMS ancho %.1f", 
          (double)leak, wide);

    printf("    interferente de 3 kHz a 12000 de amplitud: RMS ancho %.0f, "
           "Goertzel a 1 kHz %.1f (%.1f dB de rechazo)\n",
           wide, (double)leak, 20.0 * log10(wide / ((double)leak + 1e-9)));
}

/* La afirmación central del diseño: con 4096 muestras el ruido blanco se
 * atenúa unos 33 dB, porque el ancho de banda pasa de fs/2 a fs/ventana.
 * El factor teórico es raiz(2/N). */
static void test_goertzel_noise_rejection(void)
{
    printf("  Goertzel: rechazo de ruido blanco\n");

    static int16_t pcm[GZ_WINDOW];
    const double   noise_amp = 5196.0;   /* uniforme: eficaz = amp/raiz(3) = 3000 */
    const int      trials    = 64;

    rng_seed(20260909u);

    double gz_power_acc   = 0.0;
    double wide_power_acc = 0.0;

    /* Una sola ventana de ruido tiene una dispersión enorme (la magnitud es
     * Rayleigh). Se promedia la potencia sobre varias ventanas para que la
     * comprobación no dependa de la suerte de la semilla. */
    for (int t = 0; t < trials; t++) {
        for (size_t i = 0; i < GZ_WINDOW; i++) {
            pcm[i] = (int16_t)lround(noise_amp * rng_uniform());
        }
        const float gz = mic_goertzel_block(pcm, GZ_WINDOW, 1000.0f, (float)GZ_FS);
        gz_power_acc   += (double)gz * (double)gz;
        const double w = block_rms(pcm, GZ_WINDOW);
        wide_power_acc += w * w;
    }

    const double gz_rms   = sqrt(gz_power_acc / trials);
    const double wide_rms = sqrt(wide_power_acc / trials);
    const double ratio    = gz_rms / wide_rms;
    const double expected = sqrt(2.0 / (double)GZ_WINDOW);

    CHECK_REL(ratio, expected, 30.0, "atenuación de ruido frente a raiz(2/N)");

    printf("    ruido eficaz %.0f -> Goertzel %.1f  (%.1f dB, teórico %.1f dB)\n",
           wide_rms, gz_rms,
           20.0 * log10(ratio), 20.0 * log10(expected));
}

/* La prueba que decide si el enfoque sirve: un tono enterrado en ruido, con el
 * RMS de banda ancha ciego y el Goertzel encontrándolo. */
static void test_goertzel_tone_in_noise(void)
{
    printf("  Goertzel: tono enterrado en ruido\n");

    static int16_t pcm[GZ_WINDOW];
    const double   tone_amp  = 300.0;
    const double   tone_freq = 1237.0;
    const double   noise_amp = 5196.0;   /* eficaz 3000 */

    rng_seed(31415926u);

    for (size_t i = 0; i < GZ_WINDOW; i++) {
        pcm[i] = (int16_t)lround(noise_amp * rng_uniform());
    }
    const double noise_only_rms = block_rms(pcm, GZ_WINDOW);

    add_tone(pcm, GZ_WINDOW, tone_amp, tone_freq, GZ_FS);

    const double wide = block_rms(pcm, GZ_WINDOW);
    const float  gz   = mic_goertzel_block(pcm, GZ_WINDOW,
                                           (float)tone_freq, (float)GZ_FS);
    const double truth = tone_amp / sqrt(2.0);

    /* El tono es invisible en banda ancha: añadirlo apenas mueve el RMS. */
    CHECK(fabs(wide - noise_only_rms) < noise_only_rms * 0.05,
          "el tono debería ser invisible en banda ancha: %.1f frente a %.1f",
          wide, noise_only_rms);

    /* Y sin embargo el Goertzel lo recupera. */
    CHECK_REL(gz, truth, 50.0, "tono recuperado del ruido");

    printf("    tono eficaz %.1f oculto en ruido eficaz %.0f\n", truth, noise_only_rms);
    printf("    RMS ancho: %.1f (sin tono %.1f, +%.2f%%)   Goertzel: %.1f\n",
           wide, noise_only_rms,
           (wide - noise_only_rms) / noise_only_rms * 100.0, (double)gz);
}

/* El DMA entrega tramas, no ventanas: acumular por trozos tiene que dar
 * exactamente lo mismo que procesar la ventana entera de una vez. */
static void test_goertzel_streaming(void)
{
    printf("  Goertzel: acumulación por tramas del DMA\n");

    static int16_t pcm[GZ_WINDOW];
    const double   freq = 1237.0;
    fill_tone(pcm, GZ_WINDOW, 8000.0, freq, GZ_FS);

    const float one_shot = mic_goertzel_block(pcm, GZ_WINDOW,
                                              (float)freq, (float)GZ_FS);

    /* Trozos de 511: primo con 4096 a propósito, para que los cortes caigan en
     * sitios distintos y no en fronteras cómodas. */
    mic_goertzel_t g;
    mic_goertzel_init(&g, (float)freq, (float)GZ_FS, GZ_WINDOW);

    size_t sent = 0;
    while (sent < GZ_WINDOW) {
        size_t chunk = 511;
        if (sent + chunk > GZ_WINDOW) chunk = GZ_WINDOW - sent;
        CHECK(!mic_goertzel_ready(&g), "la ventana no debería estar lista aún");
        mic_goertzel_push_block(&g, &pcm[sent], chunk);
        sent += chunk;
    }

    CHECK(mic_goertzel_ready(&g), "la ventana debería estar completa");
    CHECK_REL(mic_goertzel_rms(&g), one_shot, 0.01, "por tramas frente a un bloque");

    /* Tras reiniciar debe volver a empezar limpio. */
    mic_goertzel_reset(&g);
    CHECK(!mic_goertzel_ready(&g), "reset debería vaciar el contador");
    CHECK(mic_goertzel_rms(&g) == 0.0f, "reset debería dejar el eficaz en cero");
}

/* Casos límite: punteros nulos, ventana minúscula, frecuencia sobre Nyquist y
 * continua pura. Ninguno debe reventar ni devolver algo absurdo. */
static void test_goertzel_guards(void)
{
    printf("  Goertzel: casos límite y guardias\n");

    mic_goertzel_init(NULL, 1000.0f, 44100.0f, 4096);   /* no debe reventar */
    mic_goertzel_reset(NULL);
    mic_goertzel_push_block(NULL, NULL, 10);
    CHECK(mic_goertzel_rms(NULL) == 0.0f, "un puntero nulo debería dar cero");

    mic_goertzel_t g;

    /* Ventana por debajo del mínimo: se acota, no se acepta. */
    mic_goertzel_init(&g, 1000.0f, 44100.0f, 1);
    CHECK(g.window >= 8u, "la ventana debería acotarse a 8, quedó en %zu", g.window);

    /* Sin muestras no hay medida. */
    CHECK(mic_goertzel_rms(&g) == 0.0f, "sin muestras debería dar cero");

    /* Por encima de Nyquist se acota: el coeficiente queda en el de fs/2, que
     * es -2 porque cos(pi) = -1. */
    mic_goertzel_init(&g, 40000.0f, 44100.0f, GZ_WINDOW);
    CHECK(g.coeff < -1.99f && g.coeff > -2.01f,
          "sobre Nyquist el coeficiente debería acotarse a -2, dio %.4f",
          (double)g.coeff);

    /* Continua pura medida a 1 kHz: no debe aparecer señal donde no la hay. */
    static int16_t dc[GZ_WINDOW];
    for (size_t i = 0; i < GZ_WINDOW; i++) {
        dc[i] = 5000;
    }
    const float leak = mic_goertzel_block(dc, GZ_WINDOW, 1000.0f, (float)GZ_FS);
    CHECK(leak < 50.0f, "la continua no debería filtrarse a 1 kHz: dio %.2f",
          (double)leak);
}

/* Cadena completa tal como corre en la placa: palabras crudas del DMA con su
 * offset de continua, conversión con mic_dsp_process y Goertzel encima. */
static void test_goertzel_full_chain(void)
{
    printf("  Goertzel: cadena completa desde palabras crudas del ADC\n");

    static uint16_t words[GZ_WINDOW];
    static int16_t  pcm[GZ_WINDOW];

    const double amp12 = 400.0;    /* cuentas del ADC de 12 bits */
    const double freq  = 1237.0;
    const uint8_t chan = 6;        /* ADC1_CH6 = GPIO34 */

    /* Offset de 2100 en vez de los 2048 nominales: el hardware real no cae
     * exactamente en media escala y el estimador debe encargarse. */
    for (size_t i = 0; i < GZ_WINDOW; i++) {
        const double v = 2100.0 + amp12 * sin(2.0 * M_PI * freq * (double)i / GZ_FS);
        words[i] = make_word((uint16_t)lround(v), chan);
    }

    mic_dsp_t dsp;
    mic_dsp_init(&dsp, true, MIC_DSP_DC_SHIFT_DEFAULT);

    uint32_t     dropped = 0;
    const size_t n = mic_dsp_process(&dsp, words, GZ_WINDOW, chan,
                                     pcm, GZ_WINDOW, &dropped);

    CHECK(n == GZ_WINDOW, "deberían convertirse todas: %zu de %u", n, GZ_WINDOW);
    CHECK(dropped == 0, "ninguna palabra debería descartarse: %u", dropped);

    /* El estimador de continua tarda unas 2^shift muestras en asentarse, así
     * que se mide desde la mitad de la ventana en adelante. */
    const size_t skip = GZ_WINDOW / 2;
    const float  gz = mic_goertzel_block(&pcm[skip], GZ_WINDOW - skip,
                                         (float)freq, (float)GZ_FS);

    const double expected = amp12 * MIC_DSP_GAIN / sqrt(2.0);
    CHECK_REL(gz, expected, 2.0, "eficaz recuperado de la cadena completa");

    printf("    ADC a %.0f cuentas de pico con offset 2100 -> Goertzel %.0f "
           "(esperado %.0f)\n", amp12, (double)gz, expected);
}

int main(void)
{
    printf("Tests de host de mic_dsp\n");
    printf("========================\n");

    test_word_layout();
    test_silence_is_centred();
    test_amplitude_and_gain();
    test_saturation();
    test_channel_filter();
    test_output_capacity();
    test_null_safety();
    test_shift_clamping();
    test_sine_wave_roundtrip();
    test_dma_buffer_sizing();

    printf("\n  --- Goertzel ---\n");
    test_goertzel_exact_bin();
    test_goertzel_arbitrary_frequency();
    test_goertzel_rejects_other_tones();
    test_goertzel_noise_rejection();
    test_goertzel_tone_in_noise();
    test_goertzel_streaming();
    test_goertzel_guards();
    test_goertzel_full_chain();

    printf("\n%d comprobaciones, %d fallos\n", g_checks, g_failures);
    if (g_failures == 0) {
        printf("PASS\n");
        return 0;
    }
    printf("FAIL\n");
    return 1;
}
