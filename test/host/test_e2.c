/*
 * Host tests for the pure logic of module E2 (audio generation).
 *
 * Covers the three places where E2 can be wrong in ways that are invisible on
 * a bench: the AD9833 tuning-word maths, the servo angle mapping, and the
 * parsing of the backend response.
 *
 * Build and run:  make -C test/host e2
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ad9833_regs.h"
#include "kundt_api.h"
#include "servo_map.h"

static int g_checks, g_failures;

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

static void test_ad9833_freq_word(void)
{
    printf("  AD9833 tuning word follows the data sheet formula\n");

    /* FREQREG = f_out * 2^28 / f_MCLK.
     * 1200 Hz on a 25 MHz crystal: 1200 * 268435456 / 25e6 = 12884.9 -> 12885. */
    const uint32_t w = ad9833_freq_word(1200, 25000000);
    CHECK(w == 12885, "1200 Hz -> 12885, got %u", w);

    const uint32_t back = ad9833_word_to_freq(w, 25000000);
    CHECK(back == 1200, "round trip gives 1200 Hz, got %u", back);

    CHECK(ad9833_freq_word(0, 25000000) == 0, "0 Hz -> word 0");
    CHECK(ad9833_freq_word(1000, 0) == 0, "zero MCLK yields 0 rather than dividing");

    /* The word must never exceed 28 bits, or it corrupts the register address
     * bits once split into the two 14-bit halves. */
    const uint32_t hi = ad9833_freq_word(10000000, 25000000);
    CHECK(hi < (1u << 28), "word stays inside 28 bits, got %u", hi);
}

static void test_ad9833_nyquist_clamp(void)
{
    printf("  frequencies at or above Nyquist are clamped, not wrapped\n");

    const uint32_t at    = ad9833_freq_word(12500000, 25000000);
    const uint32_t above = ad9833_freq_word(20000000, 25000000);

    CHECK(at < (1u << 28), "at Nyquist stays in range, got %u", at);
    CHECK(above < (1u << 28), "above Nyquist stays in range, got %u", above);
    CHECK(at == above, "both clamp to the same ceiling (%u vs %u)", at, above);
}

static void test_ad9833_split(void)
{
    printf("  the 28-bit word splits into two addressed 14-bit halves\n");

    const uint32_t w = ad9833_freq_word(1200, 25000000);
    const uint16_t lsb = ad9833_freq_lsb(w);
    const uint16_t msb = ad9833_freq_msb(w);

    CHECK((lsb & 0xC000) == AD9833_REG_FREQ0, "LSB addressed to FREQ0, got 0x%04x", lsb);
    CHECK((msb & 0xC000) == AD9833_REG_FREQ0, "MSB addressed to FREQ0, got 0x%04x", msb);

    const uint32_t rebuilt = (uint32_t)(lsb & 0x3FFF) | ((uint32_t)(msb & 0x3FFF) << 14);
    CHECK(rebuilt == w, "halves reassemble to %u, got %u", w, rebuilt);
}

static void test_ad9833_control(void)
{
    printf("  control words select the right waveform\n");

    const uint16_t sine = ad9833_control_word(AD9833_WAVE_SINE, false);
    CHECK(sine == 0x2000, "sine -> 0x2000, got 0x%04x", sine);
    CHECK((sine & AD9833_CTRL_OPBITEN) == 0, "sine leaves OPBITEN clear");
    CHECK((sine & AD9833_CTRL_MODE) == 0, "sine leaves MODE clear");

    const uint16_t tri = ad9833_control_word(AD9833_WAVE_TRIANGLE, false);
    CHECK(tri == 0x2002, "triangle -> 0x2002, got 0x%04x", tri);

    const uint16_t sq = ad9833_control_word(AD9833_WAVE_SQUARE, false);
    CHECK(sq == 0x2028, "square -> 0x2028 (OPBITEN|DIV2), got 0x%04x", sq);

    const uint16_t rst = ad9833_control_word(AD9833_WAVE_SINE, true);
    CHECK((rst & AD9833_CTRL_RESET) != 0, "reset flag sets bit 8, got 0x%04x", rst);
}

static void test_servo_mapping(void)
{
    printf("  servo angles map onto standard 0.5-2.5 ms pulses\n");

    CHECK(servo_angle_to_pulse_us(0)   == 500,  "0 deg -> 500 us");
    CHECK(servo_angle_to_pulse_us(90)  == 1500, "90 deg -> 1500 us");
    CHECK(servo_angle_to_pulse_us(180) == 2500, "180 deg -> 2500 us");

    CHECK(servo_angle_to_duty(0)   == 1638, "0 deg -> duty 1638, got %u", servo_angle_to_duty(0));
    CHECK(servo_angle_to_duty(90)  == 4915, "90 deg -> duty 4915, got %u", servo_angle_to_duty(90));
    CHECK(servo_angle_to_duty(180) == 8192, "180 deg -> duty 8192, got %u", servo_angle_to_duty(180));

    uint32_t prev = 0;
    for (int a = 0; a <= 180; a++) {
        const uint32_t d = servo_angle_to_duty(a);
        CHECK(d >= prev, "duty is monotonic at %d deg (%u after %u)", a, d, prev);
        prev = d;
    }
}

static void test_servo_clamping(void)
{
    printf("  out-of-range angles are clamped (finding M1)\n");

    /* The Arduino build handed the backend value straight to Servo::write(),
     * where anything >= 544 is reinterpreted as microseconds and drives the
     * servo into its end stop. */
    CHECK(servo_clamp_angle(-1)    == 0,   "negative clamps to 0");
    CHECK(servo_clamp_angle(-1000) == 0,   "very negative clamps to 0");
    CHECK(servo_clamp_angle(181)   == 180, "above range clamps to 180");
    CHECK(servo_clamp_angle(544)   == 180, "the dangerous 544 clamps to 180");
    CHECK(servo_clamp_angle(5000)  == 180, "wildly out of range clamps to 180");
    CHECK(servo_clamp_angle(90)    == 90,  "in-range passes through");

    CHECK(servo_angle_to_duty(-500) == servo_angle_to_duty(0), "negative maps to the 0 deg duty");
    CHECK(servo_angle_to_duty(9999) == servo_angle_to_duty(180), "huge maps to the 180 deg duty");
}

static void test_api_parse_valid(void)
{
    printf("  a well-formed response parses\n");

    kundt_valores_t v;
    const char *body = "{\"valores\":{\"frecuencia\":1500,\"volumen\":90,\"embolo\":42.5}}";

    CHECK(kundt_api_parse_valores(body, &v) == 0, "valid body parses");
    CHECK(v.has_frecuencia && v.frecuencia == 1500, "frecuencia = 1500, got %d", v.frecuencia);
    CHECK(v.has_volumen && v.volumen == 90, "volumen = 90, got %d", v.volumen);
    CHECK(v.has_embolo, "embolo present");
}

static void test_api_parse_rejects_garbage(void)
{
    printf("  malformed responses are rejected, not silently zeroed (finding A4)\n");

    kundt_valores_t v;

    /* The exact failure mode of the Arduino build: a 404 HTML page fed to the
     * JSON parser, error ignored, every field read back as 0 -- which drove the
     * generator to 0 Hz and the servo to 0 degrees. */
    const char *html = "<!DOCTYPE html><html><body>404 Not Found</body></html>";
    CHECK(kundt_api_parse_valores(html, &v) != 0, "an HTML error page is rejected");

    CHECK(kundt_api_parse_valores("", &v) != 0, "empty body is rejected");
    CHECK(kundt_api_parse_valores("{", &v) != 0, "truncated JSON is rejected");
    CHECK(kundt_api_parse_valores("[1,2,3]", &v) != 0, "a bare array is rejected");
    CHECK(kundt_api_parse_valores("{\"otra\":{}}", &v) != 0, "JSON without 'valores' is rejected");
    CHECK(kundt_api_parse_valores("{\"valores\":5}", &v) != 0, "'valores' as a scalar is rejected");
    CHECK(kundt_api_parse_valores(NULL, &v) != 0, "NULL body is rejected");
}

static void test_api_parse_partial(void)
{
    printf("  missing fields are distinguished from zero values\n");

    kundt_valores_t v;

    CHECK(kundt_api_parse_valores("{\"valores\":{\"frecuencia\":800}}", &v) == 0,
          "partial object still parses");
    CHECK(v.has_frecuencia && v.frecuencia == 800, "frecuencia read");
    CHECK(!v.has_volumen, "volumen correctly reported as absent");
    CHECK(!v.has_embolo, "embolo correctly reported as absent");

    CHECK(kundt_api_parse_valores("{\"valores\":{\"volumen\":0}}", &v) == 0, "zero parses");
    CHECK(v.has_volumen && v.volumen == 0, "volumen 0 is present, not absent");

    CHECK(kundt_api_parse_valores("{\"valores\":{\"frecuencia\":\"1200\"}}", &v) == 0,
          "string field still yields a valid object");
    CHECK(!v.has_frecuencia, "a string frecuencia is not accepted as a number");
}

static void test_api_end_to_end_ranges(void)
{
    printf("  parsed values survive the clamps used by the firmware\n");

    kundt_valores_t v;
    CHECK(kundt_api_parse_valores(
              "{\"valores\":{\"frecuencia\":1200,\"volumen\":270}}", &v) == 0, "parses");

    /* 270 deg is what a 270-degree potentiometer suggests, but the servo only
     * spans 180 -- the README flags this mismatch explicitly. */
    CHECK(servo_clamp_angle(v.volumen) == 180, "270 deg clamps to the servo's 180");

    const uint32_t w = ad9833_freq_word((uint32_t)v.frecuencia, AD9833_DEFAULT_MCLK_HZ);
    CHECK(w == 12885, "1200 Hz still gives 12885, got %u", w);
}

int main(void)
{
    printf("E2-SineGen host tests\n");
    printf("=====================\n");

    test_ad9833_freq_word();
    test_ad9833_nyquist_clamp();
    test_ad9833_split();
    test_ad9833_control();
    test_servo_mapping();
    test_servo_clamping();
    test_api_parse_valid();
    test_api_parse_rejects_garbage();
    test_api_parse_partial();
    test_api_end_to_end_ranges();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) { printf("PASS\n"); return 0; }
    printf("FAIL\n");
    return 1;
}
