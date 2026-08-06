/*
 * es_proto — protocol adapters for Earshot.
 *
 * Translates between Earshot's internal model (L16 frames + simple control) and
 * a specific agent wire protocol:
 *
 *   native    — raw binary audio frames (codec-coded) + small JSON control
 *   twilio    — Twilio Media Streams: connected/start/media/mark/stop JSON, base64 mu-law,
 *               `clear` = barge-in
 *   openai    — OpenAI Realtime: session.update handshake, input_audio_buffer.append (base64) out,
 *               response.output_audio.delta (base64) in, speech_started = barge-in
 *   deepgram  — Deepgram Voice Agent: Settings handshake, raw binary audio both ways,
 *               UserStartedSpeaking = barge-in
 *   elevenlabs— Conversational AI: user_audio_chunk out / audio events in, ping->pong keepalive
 *   gemini    — Gemini Live: setup handshake, realtimeInput out / serverContent in, 16k-in/24k-out
 *   pipecat   — Pipecat protobuf Frame{ audio: AudioRawFrame } both ways, InterruptionFrame = barge-in
 *   vapi      — Vapi WebSocket transport: raw binary audio both ways (pcm_s16le/mulaw, set at REST
 *               create-call), JSON control; user-interrupted / speech-update(user,started) = barge-in
 *   assemblyai— AssemblyAI Universal-Streaming STT: raw binary PCM16 out, `Turn` transcript JSON in
 *               (audio in only — no playback); transcripts surface via the on_transcript sink
 *
 * An agent written for any of these works against Earshot unmodified.
 *
 * For openai/deepgram the session handshake (voice, model, provider keys, prompt)
 * is agent-specific: the adapter sends a minimal audio-format default, or, if the
 * caller supplies a full config JSON (channel var EARSHOT_SESSION_CONFIG), sends
 * that verbatim as the first message.
 *
 * Unlike es_codec/es_pb this unit is module-side: it uses FreeSWITCH's cJSON
 * and base64 helpers (switch.h), so it is not part of the off-box unit tests.
 *
 * Copyright (c) 2026 Varun Pratap Singh. MIT License.
 */
#ifndef ES_PROTO_H
#define ES_PROTO_H

#include <switch.h>
#include "es_codec.h"
#include "es_ws.h"

typedef enum {
    ES_PROTO_NATIVE = 0,
    ES_PROTO_TWILIO,
    ES_PROTO_OPENAI,
    ES_PROTO_DEEPGRAM,
    ES_PROTO_ELEVENLABS,   /* ElevenLabs Conversational AI */
    ES_PROTO_GEMINI,       /* Google Gemini Live (BidiGenerateContent) */
    ES_PROTO_PIPECAT,      /* Pipecat protobuf frames */
    ES_PROTO_VAPI,         /* Vapi WebSocket transport (raw binary audio + JSON control) */
    ES_PROTO_ASSEMBLYAI    /* AssemblyAI Universal-Streaming STT (audio in, transcripts out; no playback) */
} es_proto_kind_t;

es_proto_kind_t es_proto_from_name(const char *name);   /* defaults to native */
const char     *es_proto_name(es_proto_kind_t k);

/* Some protocols pin the wire codec (Twilio = mu-law 8k; OpenAI = g711 8k). Returns
 * the codec the proto will actually use given the requested one. */
es_codec_t      es_proto_force_codec(es_proto_kind_t k, es_codec_t requested);

/* Optional protocol-specific handshake header, or NULL (currently none — the name,
 * when present, includes the trailing ':'). Kept as an extension point. */
const char     *es_proto_extra_header_name(es_proto_kind_t k);
const char     *es_proto_extra_header_value(es_proto_kind_t k);
const char     *es_proto_subprotocol(es_proto_kind_t k);   /* ws subprotocol to advertise, or NULL */

/*
 * Sink: how a parsed inbound message is delivered up to the module. All of these
 * run on the es_ws service thread — keep them fast and non-blocking.
 */
typedef struct {
    void *user;
    void (*on_audio)(void *user, const int16_t *pcm, size_t nsamples); /* agent -> caller */
    void (*on_clear)(void *user);                                      /* barge-in */
    void (*on_mark) (void *user, const char *name);                    /* twilio mark from agent */
    void (*on_dtmf) (void *user, const char *digit);                   /* agent-signalled dtmf */
    /* control channel: agent -> call action, pre-translated to a whitelisted uuid_* API.
     * api==NULL means the action was not recognized. id echoes back in the result. */
    void (*on_command)(void *user, const char *action, const char *api, const char *arg, const char *id);
    /* STT transcript (assemblyai): recognized text; is_final marks an end-of-turn result. May be NULL. */
    void (*on_transcript)(void *user, const char *text, int is_final);
} es_proto_sink_t;

typedef struct es_proto_ctx es_proto_ctx_t;

/* wire_rate  = the agent's sample rate (g711=8k, pcm16=24k, linear16=8/16/24k).
 * chan_rate  = the FreeSWITCH channel's L16 rate (media-bug frames); if it differs
 *              from wire_rate the adapter resamples transparently in both directions.
 * call_id / channel_uuid populate the Twilio `start` metadata (and correlation).
 * cfg (may be NULL) is a verbatim session-config JSON sent as the first message for
 * openai/deepgram (overrides the built-in audio-format default). */
es_proto_ctx_t *es_proto_create(es_proto_kind_t kind, es_codec_t codec,
                                int wire_rate, int chan_rate,
                                const char *call_id, const char *channel_uuid, const char *cfg);
void            es_proto_destroy(es_proto_ctx_t *p);

/* Give the adapter the transport handle so it can auto-respond on the receive path
 * (e.g. ElevenLabs ping->pong). Call once after es_ws_create, before es_ws_start. */
void            es_proto_set_ws(es_proto_ctx_t *p, es_ws_t *ws);

/* Lifecycle preamble/postamble (Twilio connected+start / stop; no-op for native). */
void es_proto_send_start(es_proto_ctx_t *p, es_ws_t *ws);
void es_proto_send_stop (es_proto_ctx_t *p, es_ws_t *ws);

/* Outbound caller audio (L16) -> wire (native binary / Twilio media JSON). */
void es_proto_send_audio(es_proto_ctx_t *p, es_ws_t *ws, const int16_t *pcm, size_t nsamples);

/* Echo a Twilio mark back to the agent (playback of preceding audio finished). */
void es_proto_send_mark(es_proto_ctx_t *p, es_ws_t *ws, const char *name);

/* Forward a caller DTMF digit to the agent (Twilio dtmf event / native {"type":"dtmf"}). */
void es_proto_send_dtmf(es_proto_ctx_t *p, es_ws_t *ws, const char *digit);

/* Inbound: parse one wire message and drive the sink. */
void es_proto_on_text  (es_proto_ctx_t *p, const char *data, size_t len, const es_proto_sink_t *sink);
void es_proto_on_binary(es_proto_ctx_t *p, const void *data, size_t len, const es_proto_sink_t *sink);

#endif /* ES_PROTO_H */
