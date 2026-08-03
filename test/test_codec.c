/*
 * test_codec — standalone (no FreeSWITCH). Validates the G.711 codecs by the
 * structural properties that define them: monotonic decode(encode(x)),
 * sign preservation, and bounded round-trip error. Build+run:
 *
 *   clang -Wall -O2 src/es_codec.c test/test_codec.c -o /tmp/tc && /tmp/tc
 */
#include "../src/es_codec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void test_roundtrip(const char *name, es_codec_t c)
{
    int x, prev = -100000, prev_set = 0;
    long worst = 0;

    for (x = -32768; x <= 32767; x++) {
        uint8_t enc = (c == ES_CODEC_PCMU) ? es_l16_to_ulaw((int16_t)x)
                                           : es_l16_to_alaw((int16_t)x);
        int16_t y   = (c == ES_CODEC_PCMU) ? es_ulaw_to_l16(enc)
                                           : es_alaw_to_l16(enc);
        long err = labs((long)y - x);

        /* monotonic non-decreasing reconstruction */
        if (prev_set) CHECK(y >= prev, "%s not monotonic at x=%d (y=%d < prev=%d)", name, x, y, prev);
        prev = y; prev_set = 1;

        /* sign preserved for non-trivial magnitudes */
        if (x > 256)  CHECK(y > 0,  "%s lost + sign at x=%d (y=%d)", name, x, y);
        if (x < -256) CHECK(y < 0,  "%s lost - sign at x=%d (y=%d)", name, x, y);

        /* bounded error: G.711 is ~logarithmic, error grows with magnitude */
        CHECK(err <= (labs(x) / 8) + 256, "%s error too large at x=%d: |%d-%d|=%ld", name, x, y, x, err);
        if (err > worst) worst = err;
    }
    printf("  %-5s round-trip OK  (worst abs error %ld over full int16 range)\n", name, worst);
}

static void test_frames(void)
{
    int16_t pcm[160], back[160];
    uint8_t enc[320];
    size_t i, nb, ns;

    for (i = 0; i < 160; i++) pcm[i] = (int16_t)(3000 * ((i % 20) - 10)); /* a little sawtooth */

    /* L16 is exact passthrough */
    nb = es_encode(ES_CODEC_L16, pcm, 160, enc);
    CHECK(nb == 320, "L16 encode size %zu != 320", nb);
    ns = es_decode(ES_CODEC_L16, enc, nb, back);
    CHECK(ns == 160, "L16 decode count %zu != 160", ns);
    CHECK(memcmp(pcm, back, sizeof pcm) == 0, "L16 not lossless");

    /* PCMU/PCMA frame sizes: 1 byte/sample */
    nb = es_encode(ES_CODEC_PCMU, pcm, 160, enc);
    CHECK(nb == 160, "PCMU encode size %zu != 160", nb);
    ns = es_decode(ES_CODEC_PCMU, enc, nb, back);
    CHECK(ns == 160, "PCMU decode count %zu != 160", ns);

    CHECK(es_encoded_size(ES_CODEC_L16, 160) == 320, "encoded_size L16");
    CHECK(es_encoded_size(ES_CODEC_PCMU, 160) == 160, "encoded_size PCMU");
    CHECK(es_decoded_samples(ES_CODEC_PCMA, 160) == 160, "decoded_samples PCMA");
    printf("  frame encode/decode + sizing OK\n");
}

static void test_names(void)
{
    CHECK(es_codec_from_name("pcmu") == ES_CODEC_PCMU, "name pcmu");
    CHECK(es_codec_from_name("mulaw") == ES_CODEC_PCMU, "name mulaw");
    CHECK(es_codec_from_name("alaw") == ES_CODEC_PCMA, "name alaw");
    CHECK(es_codec_from_name("l16") == ES_CODEC_L16, "name l16");
    CHECK(es_codec_from_name(NULL) == ES_CODEC_L16, "name NULL default");
    CHECK(strcmp(es_codec_name(ES_CODEC_PCMU), "pcmu") == 0, "codec_name pcmu");
    printf("  name mapping OK\n");
}

int main(void)
{
    printf("es_codec tests:\n");
    test_roundtrip("pcmu", ES_CODEC_PCMU);
    test_roundtrip("pcma", ES_CODEC_PCMA);
    test_frames();
    test_names();
    if (fails) { printf("\n%d CHECK(s) FAILED\n", fails); return 1; }
    printf("\nALL PASS\n");
    return 0;
}
