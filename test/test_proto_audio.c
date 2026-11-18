/* Inbound agent audio of any size must reach the sink whole and in order.
 *
 * Before 0.4.1 the binary paths decoded into one fixed buffer and clamped the input to
 * it (8 KB of L16 = 256 ms at 16 kHz), silently dropping the rest of any frame larger
 * than that. An agent that sends a whole utterance per frame lost most of every reply.
 * Runs on the test-only <switch.h> stub: wire rate == channel rate, so no resampler and
 * the sink sees the decoded samples verbatim. */
#include <switch.h>
#include "es_proto.h"
#include "es_pb.h"
#include "es_codec.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int16_t *got; static size_t ngot, cap_got, calls;
static void on_audio(void *u, const int16_t *p, size_t n)
{
    (void) u;
    if (ngot + n > cap_got) { cap_got = (ngot + n) * 2; got = realloc(got, cap_got * sizeof *got); }
    memcpy(got + ngot, p, n * sizeof *p); ngot += n; calls++;
}
static void nc(void *u) { (void) u; }
static void reset(void) { ngot = 0; calls = 0; }

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("    !! FAIL " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static int ramp_ok(const int16_t *s, size_t n) { for (size_t i = 0; i < n; i++) if (s[i] != (int16_t) (i * 7)) return 0; return 1; }

int main(void)
{
    es_proto_sink_t sink; memset(&sink, 0, sizeof sink);
    sink.on_audio = on_audio; sink.on_clear = nc;

    /* --- native L16: 25,000 samples (50,000 bytes) in ONE frame, 3 s at 8 kHz --- */
    {
        es_proto_ctx_t *p = es_proto_create(ES_PROTO_NATIVE, ES_CODEC_L16, 8000, 8000, "c", "u", 0);
        size_t n = 25000; int16_t *pcm = malloc(n * 2);
        for (size_t i = 0; i < n; i++) pcm[i] = (int16_t) (i * 7);
        reset(); es_proto_on_binary(p, pcm, n * 2, &sink);
        printf("  native L16 %zu samples in one frame -> %zu samples, %zu sink calls\n", n, ngot, calls);
        CHECK(ngot == n, "expected %zu samples, got %zu (truncated)", n, ngot);
        CHECK(ramp_ok(got, ngot), "samples out of order or corrupted");
        CHECK(calls > 1, "should be delivered in several pieces");
        /* odd trailing byte: whole samples only, nothing read past the end */
        reset(); es_proto_on_binary(p, pcm, n * 2 - 1, &sink);
        CHECK(ngot == n - 1, "odd-length L16: expected %zu, got %zu", n - 1, ngot);
        CHECK(ramp_ok(got, ngot), "odd-length L16: samples corrupted");
        free(pcm); es_proto_destroy(p);
    }

    /* --- native PCMU: 40,000 bytes = 5 s at 8 kHz, 1 byte per sample --- */
    {
        es_proto_ctx_t *p = es_proto_create(ES_PROTO_NATIVE, ES_CODEC_PCMU, 8000, 8000, "c", "u", 0);
        size_t n = 40000; uint8_t *ul = malloc(n); int16_t ref[1];
        memset(ul, 0xFF, n);                           /* mu-law 0xFF = silence (0) */
        es_decode(ES_CODEC_PCMU, ul, 1, ref);
        reset(); es_proto_on_binary(p, ul, n, &sink);
        printf("  native PCMU %zu bytes in one frame -> %zu samples\n", n, ngot);
        CHECK(ngot == n, "expected %zu samples, got %zu (truncated)", n, ngot);
        for (size_t i = 0; i < ngot; i++) if (got[i] != ref[0]) { CHECK(0, "PCMU sample %zu wrong", i); break; }
        free(ul); es_proto_destroy(p);
    }

    /* --- pipecat protobuf frame carrying 20,000 L16 samples at the channel rate --- */
    {
        es_proto_ctx_t *p = es_proto_create(ES_PROTO_PIPECAT, ES_CODEC_L16, 16000, 16000, "c", "u", 0);
        size_t n = 20000; int16_t *pcm = malloc(n * 2); uint8_t *frame = malloc(n * 2 + 64); size_t flen;
        for (size_t i = 0; i < n; i++) pcm[i] = (int16_t) (i * 7);
        flen = es_pb_encode_audio(frame, n * 2 + 64, (const uint8_t *) pcm, n * 2, 16000);
        CHECK(flen > 0, "protobuf encode failed");
        reset(); es_proto_on_binary(p, frame, flen, &sink);
        printf("  pipecat frame %zu samples -> %zu samples, %zu sink calls\n", n, ngot, calls);
        CHECK(ngot == n, "expected %zu samples, got %zu (truncated)", n, ngot);
        CHECK(ramp_ok(got, ngot), "pipecat samples out of order or corrupted");
        free(pcm); free(frame); es_proto_destroy(p);
    }

    /* --- OpenAI Realtime: one response.output_audio.delta carrying 30,000 L16 samples (60,000
     *     bytes, 1.25 s at 24 kHz) as base64: exercises the JSON path's heap branch --- */
    {
        es_proto_ctx_t *p = es_proto_create(ES_PROTO_OPENAI, ES_CODEC_L16, 24000, 24000, "c", "u", 0);
        size_t n = 30000; int16_t *pcm = malloc(n * 2);
        size_t b64cap = n * 2 * 4 / 3 + 8; char *b64 = malloc(b64cap), *msg = malloc(b64cap + 64);
        for (size_t i = 0; i < n; i++) pcm[i] = (int16_t) (i * 7);
        CHECK(switch_b64_encode_real((const unsigned char *) pcm, n * 2, b64, b64cap) > 0, "b64 encode failed");
        sprintf(msg, "{\"type\":\"response.output_audio.delta\",\"delta\":\"%s\"}", b64);
        reset(); es_proto_on_text(p, msg, strlen(msg), &sink);
        printf("  openai delta %zu samples (base64, one message) -> %zu samples, %zu sink calls\n", n, ngot, calls);
        CHECK(ngot == n, "expected %zu samples, got %zu (truncated)", n, ngot);
        CHECK(ramp_ok(got, ngot), "openai samples out of order or corrupted");
        free(pcm); free(b64); free(msg); es_proto_destroy(p);
    }

    /* --- a normal 20 ms frame still arrives as one piece --- */
    {
        es_proto_ctx_t *p = es_proto_create(ES_PROTO_NATIVE, ES_CODEC_L16, 8000, 8000, "c", "u", 0);
        int16_t pcm[160]; for (size_t i = 0; i < 160; i++) pcm[i] = (int16_t) (i * 7);
        reset(); es_proto_on_binary(p, pcm, sizeof pcm, &sink);
        CHECK(ngot == 160 && calls == 1, "20 ms frame: expected 160 samples in 1 call, got %zu in %zu", ngot, calls);
        es_proto_destroy(p);
    }

    /* --- a rate the agent declares is checked before it reaches the resampler ---
     * The declared rate sizes a resampler on the service thread every stream shares; 1 Hz
     * asks for an 8000:1 upsample. Such a frame is dropped (once logged), the stream keeps
     * its rate, and the next good frame plays. */
    {
        es_proto_ctx_t *p = es_proto_create(ES_PROTO_PIPECAT, ES_CODEC_L16, 16000, 16000, "c", "u", 0);
        static const uint32_t hostile[] = { 1, 7999, 96000, 2000000000u, 4294967295u };
        size_t n = 320, flen, i; int16_t pcm[320]; uint8_t frame[1024];
        int creates = stub_resample_creates, logs = stub_log_lines;
        for (i = 0; i < n; i++) pcm[i] = (int16_t) (i * 7);

        for (i = 0; i < sizeof hostile / sizeof *hostile; i++) {
            flen = es_pb_encode_audio(frame, sizeof frame, (const uint8_t *) pcm, n * 2, hostile[i]);
            reset(); es_proto_on_binary(p, frame, flen, &sink);
            CHECK(ngot == 0, "audio declared at %u Hz was played (%zu samples)", hostile[i], ngot);
        }
        CHECK(stub_resample_creates == creates, "a resampler was built for a hostile rate");
        CHECK(stub_log_lines == logs + 1, "expected one warning for the stream, got %d", stub_log_lines - logs);

        flen = es_pb_encode_audio(frame, sizeof frame, (const uint8_t *) pcm, n * 2, 16000);
        reset(); es_proto_on_binary(p, frame, flen, &sink);
        CHECK(ngot == n && ramp_ok(got, ngot), "the stream's own rate stopped working after a hostile frame");

        /* a real change of rate still rebuilds the resampler, for exactly that rate */
        flen = es_pb_encode_audio(frame, sizeof frame, (const uint8_t *) pcm, n * 2, 24000);
        reset(); es_proto_on_binary(p, frame, flen, &sink);
        CHECK(stub_resample_creates == creates + 1 && stub_resample_from == 24000,
              "24 kHz audio: expected one resampler from 24000, got %d from %d",
              stub_resample_creates - creates, stub_resample_from);
        CHECK(ngot == n, "24 kHz audio was dropped");
        printf("  hostile agent sample rates dropped, supported rates still play\n");
        es_proto_destroy(p);
    }

    free(got);
    printf(fails ? "proto_audio: %d FAILURE(S)\n" : "proto_audio: all passed\n", fails);
    return fails ? 1 : 0;
}
