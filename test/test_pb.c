/*
 * test_pb — unit tests for the Pipecat protobuf codec (es_pb).
 * Pure C, no FreeSWITCH. Covers varints, the audio round-trip, and — importantly —
 * adversarial/malformed input (the pointer-wrap OOB read peer review flagged).
 */
#include "../src/es_pb.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", #cond, __LINE__); failures++; } } while (0)

static void test_varint_roundtrip(void)
{
    uint64_t vals[] = {0, 1, 2, 127, 128, 129, 255, 256, 300, 16383, 16384,
                       2097151, 2097152, 0xFFFFFFFFull, 0x100000000ull, 0xFFFFFFFFFFFFFFFFull};
    size_t i;
    for (i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
        uint8_t buf[10];
        uint64_t got = 0;
        size_t n = es_pb_varint(buf, vals[i]);
        const uint8_t *after = es_pb_read_varint(buf, buf + n, &got);
        CHECK(after == buf + n);
        CHECK(got == vals[i]);
    }
    /* small values are 1 byte, 128 is 2 bytes, max is 10 bytes */
    { uint8_t b[10]; CHECK(es_pb_varint(b, 0) == 1); CHECK(es_pb_varint(b, 127) == 1);
      CHECK(es_pb_varint(b, 128) == 2); CHECK(es_pb_varint(b, 0xFFFFFFFFFFFFFFFFull) == 10); }
    /* truncated varint (continuation bit set, no more bytes) -> NULL */
    { uint8_t t[1] = {0x80}; uint64_t v; CHECK(es_pb_read_varint(t, t + 1, &v) == NULL); }
}

static void test_audio_roundtrip(void)
{
    uint8_t audio[320], out[512];
    es_pb_frame_t f;
    size_t i, n;
    for (i = 0; i < sizeof audio; i++) audio[i] = (uint8_t)(i * 7 + 3);

    n = es_pb_encode_audio(out, sizeof out, audio, sizeof audio, 16000);
    CHECK(n > 0 && n <= sizeof out);
    CHECK(es_pb_decode_frame(out, n, &f) == 0);
    CHECK(f.has_audio == 1);
    CHECK(f.audio_len == sizeof audio);
    CHECK(f.sample_rate == 16000);
    CHECK(f.audio && memcmp(f.audio, audio, sizeof audio) == 0);
    CHECK(f.interrupted == 0);

    /* zero-length audio still round-trips */
    n = es_pb_encode_audio(out, sizeof out, audio, 0, 8000);
    CHECK(n > 0);
    CHECK(es_pb_decode_frame(out, n, &f) == 0 && f.has_audio == 1 && f.audio_len == 0 && f.sample_rate == 8000);
}

static void test_encode_overflow(void)
{
    uint8_t audio[100], out[8];
    /* out buffer far too small -> returns 0, writes nothing past cap */
    CHECK(es_pb_encode_audio(out, sizeof out, audio, sizeof audio, 16000) == 0);
    /* exact-fit boundary: encode into a snug buffer */
    { uint8_t big[200]; size_t n = es_pb_encode_audio(big, sizeof big, audio, sizeof audio, 16000);
      CHECK(n > 0); CHECK(es_pb_encode_audio(big, n - 1, audio, sizeof audio, 16000) == 0); }
}

static void test_interruption(void)
{
    /* Frame{ interruption: InterruptionFrame{} }  ==  field 5, wire-type 2, len 0 */
    uint8_t frame[] = { (5 << 3) | 2, 0x00 };
    es_pb_frame_t f;
    CHECK(es_pb_decode_frame(frame, sizeof frame, &f) == 0);
    CHECK(f.interrupted == 1);
    CHECK(f.has_audio == 0);
}

static void test_malformed(void)
{
    es_pb_frame_t f;
    /* empty input -> ok, nothing set */
    CHECK(es_pb_decode_frame((const uint8_t *) "", 0, &f) == 0 && f.has_audio == 0);

    /* truncated tag */
    { uint8_t d[] = {0x80}; CHECK(es_pb_decode_frame(d, sizeof d, &f) == -1); }

    /* field 2, length says 100 but only 2 bytes remain -> -1, no overread */
    { uint8_t d[] = { (2 << 3) | 2, 100, 0x01, 0x02 }; CHECK(es_pb_decode_frame(d, sizeof d, &f) == -1); }

    /* THE ADVERSARIAL CASE the review found: field 2, wire-type 2, length ~2^64-100.
     * A pointer-arithmetic check (pos+flen>end) would wrap and pass; the remaining-bytes
     * check must reject it. Bytes: tag 0x12, then varint 0xFFFFFFFFFFFFFF9C. */
    { uint8_t d[] = { 0x12, 0x9C, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01 };
      CHECK(es_pb_decode_frame(d, sizeof d, &f) == -1); }

    /* same wrap attack on the inner AudioRawFrame length */
    { uint8_t d[] = { 0x12, 0x0C, /* Frame.audio len 12 */
                      0x1A, 0x9C, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01 };
      CHECK(es_pb_decode_frame(d, sizeof d, &f) == -1); }

    /* unknown scalar field (varint) before the audio is skipped, audio still found */
    { uint8_t audio[4] = {1, 2, 3, 4}, out[64];
      /* prepend field 1 (id, varint) = 42 to a normal frame by hand is complex; instead
       * verify a Frame with an unknown wire-type-0 top-level field is skipped safely */
      uint8_t d[] = { 0x08, 0x2A,                    /* field 1 varint 42 (unknown top-level) */
                      0 };                            /* placeholder, replaced below */
      size_t n = es_pb_encode_audio(out, sizeof out, audio, sizeof audio, 8000);
      /* concatenate: unknown field + real audio frame */
      uint8_t combo[80]; size_t c = 0;
      combo[c++] = d[0]; combo[c++] = d[1];
      memcpy(combo + c, out, n); c += n;
      CHECK(es_pb_decode_frame(combo, c, &f) == 0 && f.has_audio == 1 && f.audio_len == 4); }
}

int main(void)
{
    printf("test_pb:\n");
    test_varint_roundtrip();
    test_audio_roundtrip();
    test_encode_overflow();
    test_interruption();
    test_malformed();
    if (failures) { printf("  %d FAILURE(S)\n", failures); return 1; }
    printf("  all passed\n");
    return 0;
}
