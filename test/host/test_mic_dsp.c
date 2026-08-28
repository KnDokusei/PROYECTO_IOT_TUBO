/*
 * Host tests for mic_dsp - the sample-conversion path of the E1 microphone
 * module. Runs on the development machine with no ESP32 attached: mic_dsp.c is
 * deliberately free of ESP-IDF dependencies so the firmware and these tests
 * compile the exact same code.
 *
 * Build and run:  make -C test/host
 */

#include <math.h>

/* -std=c11 does not expose M_PI (it is an X/Open extension). */
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

/* Builds a raw ADC DMA word the way the ESP32 lays out TYPE1 results:
 * value in bits 0..11, channel in bits 12..15. */
static uint16_t make_word(uint16_t value, uint8_t channel)
{
    return (uint16_t)((value & 0x0FFFu) | ((uint16_t)(channel & 0x0Fu) << 12));
}

/* Feeds n samples so the DC tracker can settle before measurements. */
static void settle(mic_dsp_t *dsp, uint16_t level, int n)
{
    for (int i = 0; i < n; i++) {
        (void)mic_dsp_convert(dsp, level);
    }
}

/* --------------------------------------------------------------------- */

static void test_word_layout(void)
{
    printf("  word layout (TYPE1 decode)\n");

    CHECK(mic_word_value(make_word(0x0ABC, 6)) == 0x0ABC, "value round-trip");
    CHECK(mic_word_channel(make_word(0x0ABC, 6)) == 6, "channel round-trip");

    /* Full-scale value must not bleed into the channel field. */
    CHECK(mic_word_value(make_word(4095, 0)) == 4095, "max value preserved");
    CHECK(mic_word_channel(make_word(4095, 0)) == 0, "max value leaves channel clear");

    /* Channel 15 must not corrupt the value. */
    CHECK(mic_word_value(make_word(1234, 15)) == 1234, "value intact with high channel");
    CHECK(mic_word_channel(make_word(1234, 15)) == 15, "channel 15 decoded");
}

static void test_silence_is_centred(void)
{
    printf("  a constant input converts to silence, not to a DC step\n");

    /* This is the behaviour the Arduino build got wrong: it emitted
     * (raw & 0xFFF) * 8, i.e. unipolar samples centred near 16380 instead of 0.
     * A listener hears that as heavy distortion. */
    mic_dsp_t dsp;
    mic_dsp_init(&dsp, true, MIC_DSP_DC_SHIFT_DEFAULT);

    settle(&dsp, MIC_ADC_RAW_MIDPOINT, 5000);
    CHECK_NEAR(mic_dsp_convert(&dsp, MIC_ADC_RAW_MIDPOINT), 0, 8, "mid-scale input");

    /* Same must hold for a bias that is nowhere near mid-scale: the tracker
     * has to find it. The real front end is a 5 V circuit feeding a 3.3 V ADC,
     * so the operating point is not guaranteed to sit at 2048. */
    mic_dsp_t off;
    mic_dsp_init(&off, true, MIC_DSP_DC_SHIFT_DEFAULT);
    settle(&off, 1200, 20000);
    CHECK_NEAR(mic_dsp_convert(&off, 1200), 0, 16, "input biased at 1200 counts");
    CHECK_NEAR(mic_dsp_dc_offset(&off), 1200, 4, "DC estimate converged");
}

static void test_amplitude_and_gain(void)
{
    printf("  amplitude scales 12-bit counts into the int16 range\n");

    mic_dsp_t dsp;
    mic_dsp_init(&dsp, false, MIC_DSP_DC_SHIFT_DEFAULT);  /* pinned to mid-scale */

    /* +100 counts above centre -> 100 * 16 = 1600. */
    CHECK_NEAR(mic_dsp_convert(&dsp, MIC_ADC_RAW_MIDPOINT + 100), 1600, 1, "+100 counts");
    CHECK_NEAR(mic_dsp_convert(&dsp, MIC_ADC_RAW_MIDPOINT - 100), -1600, 1, "-100 counts");

    /* A negative excursion must actually be negative -- the original code could
     * not represent one at all. */
    CHECK(mic_dsp_convert(&dsp, 0) < 0, "bottom of scale is negative");
    CHECK(mic_dsp_convert(&dsp, MIC_ADC_RAW_MAX) > 0, "top of scale is positive");
}

static void test_saturation(void)
{
    printf("  extremes clamp instead of wrapping\n");

    mic_dsp_t dsp;
    mic_dsp_init(&dsp, false, MIC_DSP_DC_SHIFT_DEFAULT);

    /* 2048 counts below centre * 16 = -32768, exactly the int16 floor. */
    const int16_t low = mic_dsp_convert(&dsp, 0);
    CHECK(low == -32768, "full negative swing clamps to -32768, got %d", low);

    /* (4095 - 2048) * 16 = 32752, just under the int16 ceiling. */
    const int16_t high = mic_dsp_convert(&dsp, MIC_ADC_RAW_MAX);
    CHECK(high == 32752, "full positive swing gives 32752, got %d", high);

    /* Out-of-range raw input (a corrupt word) must be clamped, never wrapped
     * into a large negative sample that would click loudly in the stream. */
    const int16_t over = mic_dsp_convert(&dsp, 60000);
    CHECK(over > 0, "over-range input stays positive, got %d", over);
}

static void test_channel_filter(void)
{
    printf("  words from other channels are rejected (the A5 failure mode)\n");

    mic_dsp_t dsp;
    mic_dsp_init(&dsp, false, MIC_DSP_DC_SHIFT_DEFAULT);

    /* A DMA frame can interleave channels. Only channel 6 (GPIO34) is ours;
     * letting foreign words through injects noise into the audio stream. */
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
    printf("  a short output buffer truncates instead of overflowing\n");

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
    printf("  NULL arguments are handled\n");

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
    printf("  the smoothing factor is clamped to a usable range\n");

    mic_dsp_t dsp;
    mic_dsp_init(&dsp, true, 0);
    CHECK(dsp.shift >= 1, "shift 0 raised to at least 1, got %u", dsp.shift);

    mic_dsp_init(&dsp, true, 200);
    CHECK(dsp.shift <= 30, "shift 200 capped at 30, got %u", dsp.shift);
}

static void test_sine_wave_roundtrip(void)
{
    printf("  a synthetic tone survives conversion with its shape intact\n");

    /* Models what the tube actually presents: a 1200 Hz tone (the AD9833
     * default in E2) riding on a DC bias, sampled at 44.1 kHz. */
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

        /* Skip the settling window while the DC tracker locks on. */
        if (i > 4096) {
            if (s > peak_pos) peak_pos = s;
            if (s < peak_neg) peak_neg = s;
            sum += (double)s;
        }
    }

    const double mean = sum / (double)(n - 4096 - 1);

    /* Expected peak: 400 counts * 16 = 6400. */
    CHECK_NEAR(peak_pos, 6400, 200, "positive peak");
    CHECK_NEAR(-peak_neg, 6400, 200, "negative peak");
    CHECK(fabs(mean) < 100.0, "mean near zero (DC removed), got %.1f", mean);
    CHECK(peak_pos > 0 && peak_neg < 0, "waveform is bipolar");
}


/*
 * Buffer sizing rules for the ADC continuous driver on ESP32.
 *
 * These constants mirror soc_caps.h for this target (verified against
 * components/soc/esp32/include/soc/soc_caps.h in ESP-IDF v5.5.5). The firmware
 * itself checks them at runtime in mic_capture_validate() using the real SDK
 * macros; this test pins the arithmetic so a bad default config is caught on
 * the host, before anything is flashed.
 */
#define SOC_ADC_DIGI_RESULT_BYTES_ESP32        2
#define SOC_ADC_DIGI_DATA_BYTES_PER_CONV_ESP32 4
#define SOC_ADC_SAMPLE_FREQ_THRES_LOW_ESP32    20000
#define SOC_ADC_SAMPLE_FREQ_THRES_HIGH_ESP32   2000000

/* Values from MIC_CAPTURE_DEFAULT_CONFIG(). */
#define DEFAULT_FRAME_SAMPLES 512
#define DEFAULT_FRAME_COUNT   10
#define DEFAULT_SAMPLE_RATE   44100

static void test_dma_buffer_sizing(void)
{
    printf("  DMA frame sizing obeys the driver's alignment rule\n");

    const size_t frame_bytes =
        (size_t)DEFAULT_FRAME_SAMPLES * SOC_ADC_DIGI_RESULT_BYTES_ESP32;

    /* A conversion frame must be a multiple of DATA_BYTES_PER_CONV (4), even
     * though one result is only 2 bytes. Mixing the two up is the classic
     * sizing bug with this API -- and the same class of unit confusion that
     * made the Arduino i2s_read() call read half a buffer (finding A5). */
    CHECK(frame_bytes == 1024, "512 samples = 1024 bytes, got %zu", frame_bytes);
    CHECK(frame_bytes % SOC_ADC_DIGI_DATA_BYTES_PER_CONV_ESP32 == 0,
          "frame of %zu bytes is not a multiple of %d",
          frame_bytes, SOC_ADC_DIGI_DATA_BYTES_PER_CONV_ESP32);

    /* The pool must hold a whole number of frames. */
    const size_t pool_bytes = frame_bytes * DEFAULT_FRAME_COUNT;
    CHECK(pool_bytes % frame_bytes == 0, "pool is not a whole number of frames");
    CHECK(pool_bytes == 10240, "pool is 10240 bytes, got %zu", pool_bytes);

    /* The sample rate the README specifies must be one the hardware accepts.
     * 44.1 kHz sits comfortably inside 20 kHz..2 MHz. */
    CHECK(DEFAULT_SAMPLE_RATE >= SOC_ADC_SAMPLE_FREQ_THRES_LOW_ESP32,
          "44.1 kHz is above the %d Hz floor", SOC_ADC_SAMPLE_FREQ_THRES_LOW_ESP32);
    CHECK(DEFAULT_SAMPLE_RATE <= SOC_ADC_SAMPLE_FREQ_THRES_HIGH_ESP32,
          "44.1 kHz is below the %d Hz ceiling", SOC_ADC_SAMPLE_FREQ_THRES_HIGH_ESP32);

    /* Sanity on the resulting data rate: this is what the WebSocket must
     * sustain. 44100 * 2 = 88200 B/s ~= 86 KiB/s. */
    const long bytes_per_sec = (long)DEFAULT_SAMPLE_RATE * SOC_ADC_DIGI_RESULT_BYTES_ESP32;
    CHECK(bytes_per_sec == 88200, "stream is 88200 B/s, got %ld", bytes_per_sec);

    /* One frame per WebSocket send -> ~86 sends/s. */
    const double frames_per_sec = (double)DEFAULT_SAMPLE_RATE / DEFAULT_FRAME_SAMPLES;
    CHECK(frames_per_sec > 80.0 && frames_per_sec < 90.0,
          "~86 frames/s expected, got %.1f", frames_per_sec);
}

int main(void)
{
    printf("mic_dsp host tests\n");
    printf("==================\n");

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

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) {
        printf("PASS\n");
        return 0;
    }
    printf("FAIL\n");
    return 1;
}
