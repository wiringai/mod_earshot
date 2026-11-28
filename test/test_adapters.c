/* What each vendor adapter puts on the wire, and what it does with the vendor's replies,
 * checked against the vendors' published message references.
 *
 *   Twilio      the echoed `mark` and the `dtmf` message carry a sequenceNumber, in step
 *               with the other messages of the stream
 *   Gemini      no caller audio before the server's setupComplete, on every connection
 *   ElevenLabs  the agent's audio is played in the format the agent announces, not the
 *               one the stream was started with
 *   Deepgram    one KeepAlive every 8 seconds while no audio is sent, none while it is
 *
 * Runs on the test-only <switch.h> stub: no FreeSWITCH, no network. */
#include <switch.h>
#include "es_proto.h"
#include "es_codec.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static char   sent[64][512];
static int    nsent, nbin;
static void on_text(const char *d, size_t l)
{
    if (nsent < 64) snprintf(sent[nsent], sizeof sent[0], "%.*s", (int) (l < 500 ? l : 500), d);
    nsent++;
}
static void on_bin(const void *d, size_t l) { (void) d; (void) l; nbin++; }
static int count(const char *needle)
{
    int i, n = 0;
    for (i = 0; i < nsent && i < 64; i++) if (strstr(sent[i], needle)) n++;
    return n;
}
static const char *last(void) { return nsent ? sent[(nsent < 64 ? nsent : 64) - 1] : ""; }
static void reset(void) { nsent = 0; nbin = 0; }

static size_t ngot, calls;
static void on_audio(void *u, const int16_t *p, size_t n) { (void) u; (void) p; ngot += n; calls++; }
static void nc(void *u) { (void) u; }

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("    !! FAIL " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static es_ws_t *WS = (es_ws_t *) 0x1;   /* never dereferenced: the stub transport ignores it */

static long seq_of(const char *msg)
{
    const char *p = strstr(msg, "\"sequenceNumber\":\"");
    return p ? atol(p + 18) : -1;
}

int main(void)
{
    es_proto_sink_t sink; int16_t pcm[160] = { 0 };
    memset(&sink, 0, sizeof sink);
    sink.on_audio = on_audio; sink.on_clear = nc;
    stub_on_send_text = on_text; stub_on_send_binary = on_bin;

    printf("Twilio: sequence numbers on every message that Twilio numbers\n");
    {
        es_proto_ctx_t *p = es_proto_create(ES_PROTO_TWILIO, ES_CODEC_PCMU, 8000, 8000, "c", "u", 0);
        long a, b, c, d;
        reset(); es_proto_send_start(p, WS);
        CHECK(nsent == 2 && strstr(sent[0], "\"connected\"") && seq_of(sent[0]) == -1,
              "connected must come first and carry no sequenceNumber: %s", sent[0]);
        a = seq_of(sent[1]);
        CHECK(a == 1, "start must be number 1, got %ld", a);
        es_proto_send_audio(p, WS, pcm, 160);                  b = seq_of(last());
        es_proto_send_dtmf(p, WS, "5");                        c = seq_of(last());
        CHECK(strstr(last(), "\"track\":\"inbound\"") && strstr(last(), "\"digit\":\"5\""), "dtmf shape: %s", last());
        es_proto_send_mark(p, WS, "utterance-1");              d = seq_of(last());
        CHECK(strstr(last(), "\"name\":\"utterance-1\""), "mark shape: %s", last());
        CHECK(b == 2 && c == 3 && d == 4, "media, dtmf and mark must be numbered 2, 3, 4: got %ld, %ld, %ld", b, c, d);
        es_proto_send_stop(p, WS);
        CHECK(seq_of(last()) == 5, "stop must be number 5, got %ld", seq_of(last()));
        es_proto_destroy(p);
    }

    printf("Gemini: caller audio waits for setupComplete, on each connection\n");
    {
        es_proto_ctx_t *p = es_proto_create(ES_PROTO_GEMINI, ES_CODEC_L16, 16000, 16000, "c", "u", 0);
        const char *done = "{\"setupComplete\":{}}";
        reset(); es_proto_send_start(p, WS);
        CHECK(nsent == 1 && strstr(sent[0], "\"setup\"") && strstr(sent[0], "models/gemini-3.8-live"),
              "the first message must be setup, with the current stable Live model: %s", sent[0]);
        CHECK(!strstr(sent[0], "-latest"), "no Live model has a -latest alias");
        es_proto_send_audio(p, WS, pcm, 160); es_proto_send_audio(p, WS, pcm, 160);
        CHECK(nsent == 1, "audio was sent before setupComplete (%d messages)", nsent);
        es_proto_on_binary(p, done, strlen(done), &sink);      /* Gemini sends its JSON in binary frames */
        es_proto_send_audio(p, WS, pcm, 160);
        CHECK(nsent == 2 && strstr(last(), "realtimeInput"), "audio must flow after setupComplete: %s", last());
        es_proto_send_start(p, WS);                             /* a reconnect: a new session */
        reset(); es_proto_send_audio(p, WS, pcm, 160);
        CHECK(nsent == 0, "after a reconnect audio must wait for the new session's setupComplete");
        es_proto_on_text(p, done, strlen(done), &sink);
        es_proto_send_audio(p, WS, pcm, 160);
        CHECK(nsent == 1, "and flow again once it arrives");
        es_proto_destroy(p);
    }

    printf("ElevenLabs: the agent's audio is played in the format the agent announces\n");
    {
        /* started with the defaults (mu-law, 8 kHz); the agent is set to pcm_16000 at ElevenLabs */
        es_proto_ctx_t *p = es_proto_create(ES_PROTO_ELEVENLABS, ES_CODEC_PCMU, 8000, 8000, "c", "u", 0);
        const char *meta = "{\"type\":\"conversation_initiation_metadata\",\"conversation_initiation_metadata_event\":"
                           "{\"conversation_id\":\"conv_1\",\"agent_output_audio_format\":\"pcm_16000\","
                           "\"user_input_audio_format\":\"pcm_16000\"}}";
        /* 320 bytes of audio: 320 samples if read as mu-law, 160 if read as 16-bit PCM */
        unsigned char raw[320]; char b64[512], msg[700];
        int logs0 = stub_log_lines, rs0 = stub_resample_creates;
        memset(raw, 0, sizeof raw);
        switch_b64_encode_real(raw, sizeof raw, b64, sizeof b64);
        snprintf(msg, sizeof msg, "{\"type\":\"audio\",\"audio_event\":{\"audio_base_64\":\"%s\",\"event_id\":1}}", b64);

        ngot = 0; es_proto_on_text(p, msg, strlen(msg), &sink);
        CHECK(ngot == 320, "before any announcement the start options apply (mu-law): %zu samples", ngot);

        es_proto_on_text(p, meta, strlen(meta), &sink);
        CHECK(stub_resample_creates == rs0 + 1 && stub_resample_from == 16000,
              "a resampler from 16000 Hz must be built (built %d, from %d)", stub_resample_creates - rs0, stub_resample_from);
        ngot = 0; es_proto_on_text(p, msg, strlen(msg), &sink);
        CHECK(ngot == 160, "after pcm_16000 is announced the same bytes are 160 PCM samples, got %zu", ngot);
        CHECK(stub_log_lines == logs0 + 1, "a mismatch of the caller-audio format must be logged once (%d lines)", stub_log_lines - logs0);
        es_proto_on_text(p, meta, strlen(meta), &sink);         /* a reconnect announces it again */
        CHECK(stub_log_lines == logs0 + 1, "and only once per stream");
        es_proto_destroy(p);

        /* an agent whose formats match the stream: nothing to rebuild, nothing to report */
        p = es_proto_create(ES_PROTO_ELEVENLABS, ES_CODEC_PCMU, 8000, 8000, "c", "u", 0);
        meta = "{\"type\":\"conversation_initiation_metadata\",\"conversation_initiation_metadata_event\":"
               "{\"conversation_id\":\"conv_2\",\"agent_output_audio_format\":\"ulaw_8000\",\"user_input_audio_format\":\"ulaw_8000\"}}";
        logs0 = stub_log_lines; rs0 = stub_resample_creates;
        es_proto_on_text(p, meta, strlen(meta), &sink);
        ngot = 0; es_proto_on_text(p, msg, strlen(msg), &sink);
        CHECK(ngot == 320 && stub_log_lines == logs0 && stub_resample_creates == rs0,
              "matching formats: %zu samples, %d log lines, %d resamplers", ngot, stub_log_lines - logs0, stub_resample_creates - rs0);
        es_proto_destroy(p);

        /* a format that cannot be decoded is not played as something else */
        p = es_proto_create(ES_PROTO_ELEVENLABS, ES_CODEC_PCMU, 8000, 8000, "c", "u", 0);
        meta = "{\"type\":\"conversation_initiation_metadata\",\"conversation_initiation_metadata_event\":"
               "{\"conversation_id\":\"conv_3\",\"agent_output_audio_format\":\"mp3_44100\",\"user_input_audio_format\":\"ulaw_8000\"}}";
        logs0 = stub_log_lines;
        es_proto_on_text(p, meta, strlen(meta), &sink);
        ngot = 0; es_proto_on_text(p, msg, strlen(msg), &sink);
        CHECK(ngot == 0 && stub_log_lines == logs0 + 1, "an unknown format must be dropped and logged: %zu samples, %d lines",
              ngot, stub_log_lines - logs0);
        es_proto_destroy(p);
    }

    printf("Deepgram: a KeepAlive every 8 seconds while no audio is sent, none while it is\n");
    {
        es_proto_ctx_t *p = es_proto_create(ES_PROTO_DEEPGRAM, ES_CODEC_PCMU, 8000, 8000, "c", "u", 0);
        long t;
        reset();
        for (t = 1000; t < 31000; t += 20) es_proto_tick(p, WS, t, 1);     /* 30 s of audio */
        CHECK(count("KeepAlive") == 0, "no KeepAlive while audio flows (%d sent)", count("KeepAlive"));
        for (; t < 61000; t += 20) es_proto_tick(p, WS, t, 0);             /* 30 s paused */
        CHECK(count("KeepAlive") == 3, "30 s without audio needs 3 KeepAlives (at 8, 16, 24 s), sent %d", count("KeepAlive"));
        CHECK(!strcmp(sent[0], "{\"type\":\"KeepAlive\"}"), "KeepAlive shape: %s", sent[0]);
        reset();
        for (; t < 71000; t += 20) es_proto_tick(p, WS, t, 1);             /* audio again */
        CHECK(count("KeepAlive") == 0, "none once audio resumes");
        es_proto_destroy(p);

        p = es_proto_create(ES_PROTO_NATIVE, ES_CODEC_PCMU, 8000, 8000, "c", "u", 0);
        reset();
        for (t = 1000; t < 61000; t += 20) es_proto_tick(p, WS, t, 0);
        CHECK(nsent == 0, "other protocols send nothing of the kind (%d messages)", nsent);
        es_proto_destroy(p);
    }

    printf(fails ? "\nFAILED: %d check(s)\n" : "\nok\n", fails);
    return fails ? 1 : 0;
}
