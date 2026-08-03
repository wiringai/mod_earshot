/*
 * es_proto — protocol adapters for Earshot.  Copyright (c) 2026 Varun Pratap Singh. MIT.
 */
#include "es_proto.h"
#include "es_pb.h"      /* Pipecat protobuf codec (pure C, unit-tested) */
#include <string.h>
#include <strings.h>
#include <stdlib.h>

struct es_proto_ctx {
    es_proto_kind_t kind;
    es_codec_t      codec;      /* wire codec (twilio forces mu-law) */
    int             rate;       /* wire send rate (caller->agent) */
    int             rate_out;   /* wire receive rate (agent->caller); differs for gemini (24k) */
    int             chan_rate;  /* channel (media-bug) L16 rate */
    es_ws_t        *ws;         /* transport, for receive-path auto-responses (pong) */
    switch_audio_resampler_t *rs_out;  /* chan_rate -> rate (caller->agent), NULL if equal */
    switch_audio_resampler_t *rs_in;   /* rate_out -> chan_rate (agent->caller), NULL if equal */
    char            stream_sid[40];
    char            account_sid[40];
    char            call_sid[40];
    char            call_id[256];      /* real correlation id (SIP Call-ID) */
    char            channel_uuid[64];
    char           *cfg;        /* verbatim session-config JSON (openai/deepgram), or NULL */
    uint64_t        out_seq;    /* Twilio sequenceNumber */
    uint64_t        out_chunk;  /* Twilio media chunk */
    uint64_t        out_ts;     /* cumulative media timestamp (ms) */
};

/* ---- names -------------------------------------------------------------- */
es_proto_kind_t es_proto_from_name(const char *name)
{
    if (!name)                          return ES_PROTO_NATIVE;
    if (!strcasecmp(name, "twilio"))    return ES_PROTO_TWILIO;
    if (!strcasecmp(name, "openai"))    return ES_PROTO_OPENAI;
    if (!strcasecmp(name, "deepgram"))  return ES_PROTO_DEEPGRAM;
    if (!strcasecmp(name, "elevenlabs"))return ES_PROTO_ELEVENLABS;
    if (!strcasecmp(name, "gemini"))    return ES_PROTO_GEMINI;
    if (!strcasecmp(name, "pipecat"))   return ES_PROTO_PIPECAT;
    return ES_PROTO_NATIVE;
}

const char *es_proto_name(es_proto_kind_t k)
{
    switch (k) {
    case ES_PROTO_TWILIO:     return "twilio";
    case ES_PROTO_OPENAI:     return "openai";
    case ES_PROTO_DEEPGRAM:   return "deepgram";
    case ES_PROTO_ELEVENLABS: return "elevenlabs";
    case ES_PROTO_GEMINI:     return "gemini";
    case ES_PROTO_PIPECAT:    return "pipecat";
    default:                  return "native";
    }
}

es_codec_t es_proto_force_codec(es_proto_kind_t k, es_codec_t requested)
{
    switch (k) {
    case ES_PROTO_TWILIO:                             /* Twilio Media Streams = mu-law 8k */
        return ES_CODEC_PCMU;
    case ES_PROTO_ELEVENLABS:                         /* ulaw_8000 (telephony) */
        return ES_CODEC_PCMU;
    case ES_PROTO_GEMINI:                             /* pcm 16k in / 24k out */
    case ES_PROTO_PIPECAT:                            /* raw L16 protobuf */
        return ES_CODEC_L16;
    case ES_PROTO_OPENAI:                             /* OpenAI Realtime: g711 8k, or pcm16 (resampled) */
    case ES_PROTO_DEEPGRAM:                           /* Deepgram Voice Agent: g711 or linear16 */
        return (requested == ES_CODEC_PCMU || requested == ES_CODEC_PCMA ||
                requested == ES_CODEC_L16) ? requested : ES_CODEC_PCMU;
    default:
        return requested;
    }
}

const char *es_proto_extra_header_name(es_proto_kind_t k)
{
    (void) k;
    return NULL;   /* the GA Realtime endpoint rejects the old "OpenAI-Beta: realtime=v1"
                    * header with a session error; the handshake needs only Authorization. */
}
const char *es_proto_extra_header_value(es_proto_kind_t k)
{
    (void) k;
    return NULL;
}
const char *es_proto_subprotocol(es_proto_kind_t k)
{
    /* OpenAI Realtime answers the upgrade with Sec-WebSocket-Protocol: realtime,
     * so we must advertise exactly that. Other agents use no subprotocol. */
    return (k == ES_PROTO_OPENAI) ? "realtime" : NULL;
}

/* codec -> the vendor's audio-format name for the session handshake */
static const char *es_openai_fmt(es_codec_t c)
{
    return c == ES_CODEC_PCMA ? "g711_alaw" : c == ES_CODEC_L16 ? "pcm16" : "g711_ulaw";
}
static const char *es_deepgram_fmt(es_codec_t c)
{
    return c == ES_CODEC_PCMA ? "alaw" : c == ES_CODEC_L16 ? "linear16" : "mulaw";
}

/* ---- helpers ------------------------------------------------------------ */
static void es_gen_sid(char *out, size_t olen, const char *prefix)
{
    switch_uuid_t uuid;
    char buf[SWITCH_UUID_FORMATTED_LENGTH + 1] = {0}, hex[33] = {0};
    int i, j = 0;
    switch_uuid_get(&uuid);
    switch_uuid_format(buf, &uuid);
    for (i = 0; buf[i] && j < 32; i++) if (buf[i] != '-') hex[j++] = buf[i];
    hex[j] = 0;
    snprintf(out, olen, "%s%s", prefix, hex);
}

static void es_send_text_z(es_ws_t *ws, const char *s) { es_ws_send_text(ws, s, strlen(s)); }

/* ---- lifecycle ---------------------------------------------------------- */
es_proto_ctx_t *es_proto_create(es_proto_kind_t kind, es_codec_t codec,
                                int wire_rate, int chan_rate,
                                const char *call_id, const char *channel_uuid, const char *cfg)
{
    es_proto_ctx_t *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->kind      = kind;
    p->codec     = codec;
    p->rate      = wire_rate > 0 ? wire_rate : 8000;
    p->rate_out  = p->rate;
    p->chan_rate = chan_rate > 0 ? chan_rate : 8000;
    if (kind == ES_PROTO_GEMINI) { p->rate = 16000; p->rate_out = 24000; }  /* Gemini: 16k in / 24k out */
    if (kind == ES_PROTO_PIPECAT && wire_rate <= 8000) { p->rate = 16000; p->rate_out = 16000; }  /* pipecat default 16k */
    p->cfg       = (cfg && *cfg) ? strdup(cfg) : NULL;
    switch_copy_string(p->call_id, call_id ? call_id : "", sizeof p->call_id);
    switch_copy_string(p->channel_uuid, channel_uuid ? channel_uuid : "", sizeof p->channel_uuid);
    es_gen_sid(p->stream_sid,  sizeof p->stream_sid,  "MZ");
    es_gen_sid(p->account_sid, sizeof p->account_sid, "AC");
    es_gen_sid(p->call_sid,    sizeof p->call_sid,    "CA");

    /* transparent resampling per direction when channel/wire rates differ (e.g. gemini 8k<->16k/24k) */
    if (p->rate != p->chan_rate)
        switch_resample_create(&p->rs_out, p->chan_rate, p->rate, 8192, SWITCH_RESAMPLE_QUALITY, 1);
    if (p->rate_out != p->chan_rate)
        switch_resample_create(&p->rs_in,  p->rate_out, p->chan_rate, 8192, SWITCH_RESAMPLE_QUALITY, 1);
    return p;
}

void es_proto_set_ws(es_proto_ctx_t *p, es_ws_t *ws) { if (p) p->ws = ws; }

void es_proto_destroy(es_proto_ctx_t *p)
{
    if (!p) return;
    if (p->rs_out) switch_resample_destroy(&p->rs_out);
    if (p->rs_in)  switch_resample_destroy(&p->rs_in);
    free(p->cfg);
    free(p);
}

void es_proto_send_start(es_proto_ctx_t *p, es_ws_t *ws)
{
    char msg[1200];
    if (!p || !ws) return;

    if (p->kind == ES_PROTO_TWILIO) {
        es_send_text_z(ws, "{\"event\":\"connected\",\"protocol\":\"Call\",\"version\":\"1.0.0\"}");
        snprintf(msg, sizeof msg,
            "{\"event\":\"start\",\"sequenceNumber\":\"%llu\",\"streamSid\":\"%s\","
            "\"start\":{\"streamSid\":\"%s\",\"accountSid\":\"%s\",\"callSid\":\"%s\","
            "\"tracks\":[\"inbound\"],"
            "\"customParameters\":{\"earshot_call_id\":\"%s\",\"earshot_channel_uuid\":\"%s\"},"
            "\"mediaFormat\":{\"encoding\":\"audio/x-mulaw\",\"sampleRate\":8000,\"channels\":1}}}",
            (unsigned long long)++p->out_seq, p->stream_sid, p->stream_sid,
            p->account_sid, p->call_sid, p->call_id, p->channel_uuid);
        es_send_text_z(ws, msg);
        return;
    }

    /* openai/deepgram: a full user-supplied config wins; else send an audio-format default */
    if (p->kind == ES_PROTO_OPENAI) {
        if (p->cfg) { es_send_text_z(ws, p->cfg); return; }
        snprintf(msg, sizeof msg,
            "{\"type\":\"session.update\",\"session\":{"
            "\"input_audio_format\":\"%s\",\"output_audio_format\":\"%s\","
            "\"turn_detection\":{\"type\":\"server_vad\"}}}",
            es_openai_fmt(p->codec), es_openai_fmt(p->codec));
        es_send_text_z(ws, msg);
        return;
    }

    if (p->kind == ES_PROTO_DEEPGRAM) {
        if (p->cfg) { es_send_text_z(ws, p->cfg); return; }
        snprintf(msg, sizeof msg,
            "{\"type\":\"Settings\",\"audio\":{"
            "\"input\":{\"encoding\":\"%s\",\"sample_rate\":%d},"
            "\"output\":{\"encoding\":\"%s\",\"sample_rate\":%d,\"container\":\"none\"}},"
            "\"agent\":{\"language\":\"en\","
            "\"listen\":{\"provider\":{\"type\":\"deepgram\",\"model\":\"nova-3\"}},"
            "\"think\":{\"provider\":{\"type\":\"open_ai\",\"model\":\"gpt-4o-mini\"}},"
            "\"speak\":{\"provider\":{\"type\":\"deepgram\",\"model\":\"aura-2-thalia-en\"}}}}",
            es_deepgram_fmt(p->codec), p->rate, es_deepgram_fmt(p->codec), p->rate);
        es_send_text_z(ws, msg);
        return;
    }

    if (p->kind == ES_PROTO_GEMINI) {                 /* setup must be the first message */
        if (p->cfg) { es_send_text_z(ws, p->cfg); return; }
        es_send_text_z(ws,
            "{\"setup\":{\"model\":\"models/gemini-2.0-flash-exp\","
            "\"generationConfig\":{\"responseModalities\":[\"AUDIO\"]}}}");
        return;
    }

    if (p->kind == ES_PROTO_ELEVENLABS) {             /* optional init override */
        if (p->cfg) es_send_text_z(ws, p->cfg);       /* else the agent uses its configured defaults */
        return;
    }

    /* native / pipecat: no preamble */
}

void es_proto_send_stop(es_proto_ctx_t *p, es_ws_t *ws)
{
    char msg[512];
    if (!p || !ws || p->kind != ES_PROTO_TWILIO) return;
    snprintf(msg, sizeof msg,
        "{\"event\":\"stop\",\"sequenceNumber\":\"%llu\",\"streamSid\":\"%s\","
        "\"stop\":{\"accountSid\":\"%s\",\"callSid\":\"%s\"}}",
        (unsigned long long)++p->out_seq, p->stream_sid, p->account_sid, p->call_sid);
    es_send_text_z(ws, msg);
}

void es_proto_send_mark(es_proto_ctx_t *p, es_ws_t *ws, const char *name)
{
    char msg[256];
    if (!p || !ws || p->kind != ES_PROTO_TWILIO || !name) return;
    snprintf(msg, sizeof msg,
        "{\"event\":\"mark\",\"streamSid\":\"%s\",\"mark\":{\"name\":\"%s\"}}",
        p->stream_sid, name);
    es_send_text_z(ws, msg);
}

void es_proto_send_dtmf(es_proto_ctx_t *p, es_ws_t *ws, const char *digit)
{
    char msg[256];
    if (!p || !ws || !digit) return;
    if (p->kind == ES_PROTO_TWILIO)
        snprintf(msg, sizeof msg,
                 "{\"event\":\"dtmf\",\"streamSid\":\"%s\",\"dtmf\":{\"track\":\"inbound\",\"digit\":\"%s\"}}",
                 p->stream_sid, digit);
    else
        snprintf(msg, sizeof msg, "{\"type\":\"dtmf\",\"digit\":\"%s\"}", digit);
    es_send_text_z(ws, msg);
}

/* ---- outbound audio (caller -> agent) ----------------------------------- */
void es_proto_send_audio(es_proto_ctx_t *p, es_ws_t *ws, const int16_t *pcm, size_t nsamples)
{
    uint8_t enc[SWITCH_RECOMMENDED_BUFFER_SIZE];
    size_t  nb;
    if (!p || !ws || !pcm || !nsamples) return;

    /* channel-rate L16 -> wire-rate L16 (no-op when the rates match) */
    if (p->rs_out) {
        uint32_t on = switch_resample_process(p->rs_out, (int16_t *) pcm, (uint32_t) nsamples);
        pcm = p->rs_out->to;
        nsamples = on;
        if (!nsamples) return;
    }
    /* bound the encode: enc[] holds at most sizeof(enc)/2 L16 samples (the worst case) */
    if (nsamples > sizeof(enc) / 2) nsamples = sizeof(enc) / 2;

    if (p->kind == ES_PROTO_TWILIO) {
        char b64[2 * SWITCH_RECOMMENDED_BUFFER_SIZE];
        char msg[3 * SWITCH_RECOMMENDED_BUFFER_SIZE];
        int  n;
        nb = es_encode(ES_CODEC_PCMU, pcm, nsamples, enc);
        if (nb == (size_t) -1) return;
        switch_b64_encode(enc, nb, (unsigned char *) b64, sizeof b64);
        n = snprintf(msg, sizeof msg,
            "{\"event\":\"media\",\"sequenceNumber\":\"%llu\",\"streamSid\":\"%s\","
            "\"media\":{\"track\":\"inbound\",\"chunk\":\"%llu\",\"timestamp\":\"%llu\",\"payload\":\"%s\"}}",
            (unsigned long long)++p->out_seq, p->stream_sid,
            (unsigned long long)++p->out_chunk, (unsigned long long)p->out_ts, b64);
        if (n > 0) es_ws_send_text(ws, msg, (size_t) n);
        p->out_ts += (p->rate >= 1000) ? nsamples / (p->rate / 1000) : 0;   /* advance ms */
        return;
    }

    if (p->kind == ES_PROTO_OPENAI || p->kind == ES_PROTO_ELEVENLABS || p->kind == ES_PROTO_GEMINI) {
        char b64[2 * SWITCH_RECOMMENDED_BUFFER_SIZE];
        char msg[3 * SWITCH_RECOMMENDED_BUFFER_SIZE];
        int  n;
        nb = es_encode(p->codec, pcm, nsamples, enc);   /* codec: openai g711/l16, 11labs ulaw, gemini l16 */
        if (nb == (size_t) -1) return;
        switch_b64_encode(enc, nb, (unsigned char *) b64, sizeof b64);
        if (p->kind == ES_PROTO_OPENAI)
            n = snprintf(msg, sizeof msg, "{\"type\":\"input_audio_buffer.append\",\"audio\":\"%s\"}", b64);
        else if (p->kind == ES_PROTO_ELEVENLABS)
            n = snprintf(msg, sizeof msg, "{\"user_audio_chunk\":\"%s\"}", b64);
        else  /* gemini */
            n = snprintf(msg, sizeof msg,
                "{\"realtimeInput\":{\"mediaChunks\":[{\"mimeType\":\"audio/pcm;rate=%d\",\"data\":\"%s\"}]}}",
                p->rate, b64);
        if (n > 0) es_ws_send_text(ws, msg, (size_t) n);
        return;
    }

    if (p->kind == ES_PROTO_PIPECAT) {                    /* protobuf Frame{ audio: AudioRawFrame } */
        uint8_t frame[2 * SWITCH_RECOMMENDED_BUFFER_SIZE + 64];
        size_t flen;
        nb = es_encode(ES_CODEC_L16, pcm, nsamples, enc);       /* raw L16 */
        if (nb == (size_t) -1) return;
        flen = es_pb_encode_audio(frame, sizeof frame, enc, nb, (uint32_t) p->rate);
        if (flen) es_ws_send_binary(ws, frame, flen);
        return;
    }

    /* native + deepgram: raw codec-coded binary frame */
    nb = es_encode(p->codec, pcm, nsamples, enc);
    if (nb != (size_t) -1) es_ws_send_binary(ws, enc, nb);
}

/* ---- inbound (agent -> caller) ------------------------------------------ */
/* deliver wire-rate L16 to the sink, resampling down to the channel rate first if needed */
static void es_emit_audio(es_proto_ctx_t *p, int16_t *pcm, size_t ns, const es_proto_sink_t *sink)
{
    if (!ns || !sink || !sink->on_audio) return;
    if (p->rs_in) {
        uint32_t cn = switch_resample_process(p->rs_in, pcm, (uint32_t) ns);
        if (cn) sink->on_audio(sink->user, p->rs_in->to, cn);
    } else {
        sink->on_audio(sink->user, pcm, ns);
    }
}

static void es_deliver_ulaw_b64(es_proto_ctx_t *p, const char *b64, es_codec_t codec,
                                const es_proto_sink_t *sink)
{
    char    raw[2 * SWITCH_RECOMMENDED_BUFFER_SIZE];
    int16_t pcm[SWITCH_RECOMMENDED_BUFFER_SIZE];
    switch_size_t nb, blen, pad = 0, expected;
    size_t ns;
    if (!b64 || !sink || !sink->on_audio) return;
    nb = switch_b64_decode(b64, raw, sizeof raw);
    if (!nb) return;
    /* switch_b64_decode's length convention varies by version; derive the exact
     * decoded size from the base64 string so we never keep a spurious trailing
     * byte (which would grow the buffer over a long call and click as loud mu-law). */
    blen = strlen(b64);
    if (blen >= 1 && b64[blen - 1] == '=') pad++;
    if (blen >= 2 && b64[blen - 2] == '=') pad++;
    expected = blen / 4 * 3 - pad;
    if (nb > expected) nb = expected;
    {   /* clamp to the byte count that fills pcm[] for THIS codec (L16=2B/sample, g711=1B) */
        size_t cap = es_encoded_size(codec, sizeof(pcm) / sizeof(pcm[0]));
        if (cap != (size_t) -1 && nb > cap) nb = cap;
    }
    ns = es_decode(codec, (const uint8_t *) raw, nb, pcm);
    if (ns != (size_t) -1 && ns) es_emit_audio(p, pcm, ns, sink);
}

static es_codec_t es_codec_from_wire(const char *name, es_codec_t deflt)
{
    if (!name) return deflt;
    if (!strcasecmp(name, "audio/x-mulaw") || !strcasecmp(name, "mulaw") || !strcasecmp(name, "pcmu"))
        return ES_CODEC_PCMU;
    if (!strcasecmp(name, "audio/x-alaw") || !strcasecmp(name, "alaw") || !strcasecmp(name, "pcma"))
        return ES_CODEC_PCMA;
    if (!strcasecmp(name, "l16") || !strcasecmp(name, "linear")) return ES_CODEC_L16;
    return deflt;
}

static const char *es_json_str(cJSON *o, const char *k)
{
    cJSON *i = cJSON_GetObjectItem(o, k);
    return (i && i->valuestring) ? i->valuestring : "";
}

/* Translate an agent {"type":"command",...} into a whitelisted uuid_* API call and
 * hand it to the sink. Only these call-control actions are allowed; anything else is
 * reported unsupported (api==NULL). The uuid is the channel this stream is tapping. */
static void es_dispatch_command(es_proto_ctx_t *p, cJSON *root, const es_proto_sink_t *sink)
{
    const char *action = es_json_str(root, "action");
    const char *id     = es_json_str(root, "id");
    const char *uuid   = p->channel_uuid;
    char api[32] = "", arg[1024] = "";
    if (!sink->on_command) return;

    if (!strcasecmp(action, "transfer")) {
        const char *to = *es_json_str(root, "to") ? es_json_str(root, "to") : es_json_str(root, "extension");
        int n = snprintf(arg, sizeof arg, "%s %s", uuid, to);
        if (n < 0 || n >= (int) sizeof arg) n = (int) sizeof arg - 1;   /* clamp against oversized fields */
        if (*es_json_str(root, "dialplan")) n += snprintf(arg + n, sizeof arg - n, " %s", es_json_str(root, "dialplan"));
        if (n < 0 || n >= (int) sizeof arg) n = (int) sizeof arg - 1;
        if (*es_json_str(root, "context"))  snprintf(arg + n, sizeof arg - n, " %s", es_json_str(root, "context"));
        snprintf(api, sizeof api, "uuid_transfer");
    } else if (!strcasecmp(action, "hangup")) {
        const char *c = es_json_str(root, "cause");
        snprintf(api, sizeof api, "uuid_kill");
        snprintf(arg, sizeof arg, "%s %s", uuid, *c ? c : "NORMAL_CLEARING");
    } else if (!strcasecmp(action, "send_dtmf") || !strcasecmp(action, "dtmf")) {
        snprintf(api, sizeof api, "uuid_send_dtmf");
        snprintf(arg, sizeof arg, "%s %s", uuid, es_json_str(root, "digits"));
    } else if (!strcasecmp(action, "play") || !strcasecmp(action, "broadcast")) {
        const char *leg = es_json_str(root, "leg");
        snprintf(api, sizeof api, "uuid_broadcast");
        snprintf(arg, sizeof arg, "%s %s %s", uuid, es_json_str(root, "file"), *leg ? leg : "aleg");
    } else if (!strcasecmp(action, "stop_play") || !strcasecmp(action, "break")) {
        snprintf(api, sizeof api, "uuid_break");
        snprintf(arg, sizeof arg, "%s", uuid);
    } else if (!strcasecmp(action, "record")) {
        const char *state = es_json_str(root, "state");
        snprintf(api, sizeof api, "uuid_record");
        snprintf(arg, sizeof arg, "%s %s %s", uuid, *state ? state : "start", es_json_str(root, "file"));
    } else if (!strcasecmp(action, "setvar")) {
        snprintf(api, sizeof api, "uuid_setvar");
        snprintf(arg, sizeof arg, "%s %s %s", uuid, es_json_str(root, "name"), es_json_str(root, "value"));
    } else if (!strcasecmp(action, "hold")) {
        const char *state = es_json_str(root, "state");
        snprintf(api, sizeof api, "uuid_hold");
        if (!strcasecmp(state, "off") || !strcasecmp(state, "unhold")) snprintf(arg, sizeof arg, "off %s", uuid);
        else snprintf(arg, sizeof arg, "%s", uuid);
    } else if (!strcasecmp(action, "bridge")) {
        snprintf(api, sizeof api, "uuid_bridge");
        snprintf(arg, sizeof arg, "%s %s", uuid, es_json_str(root, "peer_uuid"));
    } else if (!strcasecmp(action, "park")) {
        snprintf(api, sizeof api, "uuid_park");
        snprintf(arg, sizeof arg, "%s", uuid);
    } else if (!strcasecmp(action, "mask")) {
        snprintf(api, sizeof api, "__mask__");    /* earshot-internal; handled by the sink */
        snprintf(arg, sizeof arg, "%s", es_json_str(root, "state"));
    } else {
        sink->on_command(sink->user, action, NULL, NULL, id);   /* unsupported */
        return;
    }
    sink->on_command(sink->user, action, api, arg, id);
}

void es_proto_on_text(es_proto_ctx_t *p, const char *data, size_t len, const es_proto_sink_t *sink)
{
    cJSON *root, *ev, *node;
    const char *e;
    (void) len;
    if (!p || !data || !sink) return;
    if (!(root = cJSON_Parse(data))) return;

    /* Control channel: {"type":"command",...} is honored in every protocol (earshot-specific;
     * a vendor never sends this shape, so it's a safe universal escape hatch). */
    node = cJSON_GetObjectItem(root, "type");
    if (node && node->valuestring && !strcasecmp(node->valuestring, "command")) {
        es_dispatch_command(p, root, sink);
        cJSON_Delete(root);
        return;
    }

    if (p->kind == ES_PROTO_TWILIO) {
        ev = cJSON_GetObjectItem(root, "event");
        e  = (ev && ev->valuestring) ? ev->valuestring : "";
        if (!strcasecmp(e, "media")) {
            cJSON *media = cJSON_GetObjectItem(root, "media");
            cJSON *pl = media ? cJSON_GetObjectItem(media, "payload") : NULL;
            if (pl && pl->valuestring) es_deliver_ulaw_b64(p, pl->valuestring, ES_CODEC_PCMU, sink);
        } else if (!strcasecmp(e, "clear")) {
            if (sink->on_clear) sink->on_clear(sink->user);
        } else if (!strcasecmp(e, "mark")) {
            cJSON *m = cJSON_GetObjectItem(root, "mark");
            cJSON *nm = m ? cJSON_GetObjectItem(m, "name") : NULL;
            if (nm && nm->valuestring && sink->on_mark) sink->on_mark(sink->user, nm->valuestring);
        } else if (!strcasecmp(e, "dtmf")) {
            cJSON *d = cJSON_GetObjectItem(root, "dtmf");
            cJSON *dg = d ? cJSON_GetObjectItem(d, "digit") : NULL;
            if (dg && dg->valuestring && sink->on_dtmf) sink->on_dtmf(sink->user, dg->valuestring);
        }
        cJSON_Delete(root);
        return;
    }

    if (p->kind == ES_PROTO_OPENAI) {
        node = cJSON_GetObjectItem(root, "type");
        e = (node && node->valuestring) ? node->valuestring : "";
        if (!strcmp(e, "response.output_audio.delta") ||       /* agent audio (base64), GA name */
            !strcmp(e, "response.audio.delta")) {              /* legacy beta name */
            cJSON *d = cJSON_GetObjectItem(root, "delta");
            if (d && d->valuestring) es_deliver_ulaw_b64(p, d->valuestring, p->codec, sink);
        } else if (!strcmp(e, "input_audio_buffer.speech_started")) {
            if (sink->on_clear) sink->on_clear(sink->user);    /* server VAD: caller talked = barge-in */
        }
        cJSON_Delete(root);
        return;
    }

    if (p->kind == ES_PROTO_DEEPGRAM) {                        /* audio arrives as binary; text = control */
        node = cJSON_GetObjectItem(root, "type");
        e = (node && node->valuestring) ? node->valuestring : "";
        if (!strcmp(e, "UserStartedSpeaking")) {
            if (sink->on_clear) sink->on_clear(sink->user);    /* barge-in */
        }
        cJSON_Delete(root);
        return;
    }

    if (p->kind == ES_PROTO_ELEVENLABS) {
        node = cJSON_GetObjectItem(root, "type");
        e = (node && node->valuestring) ? node->valuestring : "";
        if (!strcmp(e, "audio")) {
            cJSON *ae = cJSON_GetObjectItem(root, "audio_event");
            cJSON *b  = ae ? cJSON_GetObjectItem(ae, "audio_base_64") : NULL;
            if (b && b->valuestring) es_deliver_ulaw_b64(p, b->valuestring, p->codec, sink);   /* ulaw_8000 */
        } else if (!strcmp(e, "interruption")) {
            if (sink->on_clear) sink->on_clear(sink->user);    /* barge-in */
        } else if (!strcmp(e, "ping")) {                       /* keepalive: must pong back */
            cJSON *pe = cJSON_GetObjectItem(root, "ping_event");
            cJSON *id = pe ? cJSON_GetObjectItem(pe, "event_id") : NULL;
            if (p->ws) {
                char msg[96];
                snprintf(msg, sizeof msg, "{\"type\":\"pong\",\"event_id\":%d}", id ? id->valueint : 0);
                es_send_text_z(p->ws, msg);
            }
        }
        cJSON_Delete(root);
        return;
    }

    if (p->kind == ES_PROTO_GEMINI) {
        cJSON *sc = cJSON_GetObjectItem(root, "serverContent");
        if (sc) {
            cJSON *interrupted = cJSON_GetObjectItem(sc, "interrupted");
            cJSON *mt = cJSON_GetObjectItem(sc, "modelTurn");
            cJSON *parts = mt ? cJSON_GetObjectItem(mt, "parts") : NULL;
            if (interrupted && cJSON_IsTrue(interrupted) && sink->on_clear)
                sink->on_clear(sink->user);                    /* barge-in */
            if (parts) {
                cJSON *part;
                cJSON_ArrayForEach(part, parts) {
                    cJSON *idata = cJSON_GetObjectItem(part, "inlineData");
                    cJSON *data  = idata ? cJSON_GetObjectItem(idata, "data") : NULL;
                    if (data && data->valuestring)
                        es_deliver_ulaw_b64(p, data->valuestring, ES_CODEC_L16, sink);   /* 24k pcm */
                }
            }
        }
        cJSON_Delete(root);
        return;
    }

    /* native: accept {"type":"playAudio","data":{"audio":b64,"codec":..}}, {"type":"clear"},
     * and tolerate twilio-shaped {"event":"media",...} for convenience. */
    node = cJSON_GetObjectItem(root, "type");
    e = (node && node->valuestring) ? node->valuestring : "";
    if (!*e) { node = cJSON_GetObjectItem(root, "event"); e = (node && node->valuestring) ? node->valuestring : ""; }

    if (!strcasecmp(e, "playAudio") || !strcasecmp(e, "media")) {
        cJSON *d = cJSON_GetObjectItem(root, "data");
        cJSON *m = cJSON_GetObjectItem(root, "media");
        cJSON *audio = d ? cJSON_GetObjectItem(d, "audio") : NULL;
        cJSON *pl    = m ? cJSON_GetObjectItem(m, "payload") : NULL;
        cJSON *cod   = d ? cJSON_GetObjectItem(d, "codec") : NULL;
        const char *b64 = audio && audio->valuestring ? audio->valuestring :
                          (pl && pl->valuestring ? pl->valuestring : NULL);
        es_codec_t c = es_codec_from_wire(cod ? cod->valuestring : NULL, p->codec);
        if (b64) es_deliver_ulaw_b64(p, b64, c, sink);
    } else if (!strcasecmp(e, "clear")) {
        if (sink->on_clear) sink->on_clear(sink->user);
    } else if (!strcasecmp(e, "mark")) {
        cJSON *m = cJSON_GetObjectItem(root, "mark");
        cJSON *nm = m ? cJSON_GetObjectItem(m, "name") : NULL;
        if (nm && nm->valuestring && sink->on_mark) sink->on_mark(sink->user, nm->valuestring);
    }
    cJSON_Delete(root);
}

/* Pipecat: parse a protobuf Frame; deliver audio, treat an interruption as barge-in. */
static void es_pipecat_on_binary(es_proto_ctx_t *p, const uint8_t *data, size_t len, const es_proto_sink_t *sink)
{
    es_pb_frame_t f;
    if (es_pb_decode_frame(data, len, &f) != 0) return;        /* malformed -> drop safely */
    if (f.interrupted && sink->on_clear) sink->on_clear(sink->user);
    if (f.has_audio && f.audio_len) {
        int16_t pcm[SWITCH_RECOMMENDED_BUFFER_SIZE];
        size_t alen = f.audio_len, ns;
        if (alen > sizeof(pcm)) alen = sizeof(pcm);           /* L16: bytes -> alen/2 samples <= buf */
        ns = es_decode(ES_CODEC_L16, f.audio, alen, pcm);
        if (ns != (size_t) -1 && ns) es_emit_audio(p, pcm, ns, sink);
    }
}

void es_proto_on_binary(es_proto_ctx_t *p, const void *data, size_t len, const es_proto_sink_t *sink)
{
    int16_t pcm[SWITCH_RECOMMENDED_BUFFER_SIZE];
    size_t ns;
    if (!p || !data || !len || !sink || !sink->on_audio) return;
    if (p->kind == ES_PROTO_TWILIO || p->kind == ES_PROTO_OPENAI ||
        p->kind == ES_PROTO_ELEVENLABS || p->kind == ES_PROTO_GEMINI) return;  /* audio is text-framed */
    if (p->kind == ES_PROTO_PIPECAT) { es_pipecat_on_binary(p, (const uint8_t *) data, len, sink); return; }
    /* native + deepgram: raw codec-coded audio */
    if (len > SWITCH_RECOMMENDED_BUFFER_SIZE) len = SWITCH_RECOMMENDED_BUFFER_SIZE;  /* clamp */
    ns = es_decode(p->codec, (const uint8_t *) data, len, pcm);
    if (ns != (size_t) -1 && ns) es_emit_audio(p, pcm, ns, sink);
}
