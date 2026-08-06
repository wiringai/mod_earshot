/*
 * mod_earshot — stream FreeSWITCH channel audio to an AI agent over WebSocket.
 *
 * Copyright (c) 2026 Varun Pratap Singh. MIT License (see ../LICENSE, ../NOTICE).
 *
 * Full-duplex bridge: caller audio -> agent over a WebSocket, and the agent's audio
 * -> caller via WRITE_REPLACE. Seven protocol adapters (native/twilio/openai/deepgram/
 * elevenlabs/gemini/pipecat), module-side VAD + turn events, an agent control channel,
 * PCI masking, multi-stream fan-out, latency metrics, G.711/L16 + resampling, and a
 * uuid_audio_stream compat shim. Layout: ../docs/ARCHITECTURE.md; roadmap: ../ROADMAP.md.
 *
 * RENAME: the module identity is the token "earshot" / "mod_earshot". To rebrand,
 * change: this filename, the SWITCH_MODULE_* names below, the SWITCH_ADD_API/APP
 * command string, the EARSHOT_EVENT_* subclasses, and the EARSHOT_ var prefix.
 */

#include <switch.h>
#include "es_codec.h"   /* es_codec_t + G.711/L16 (unit-tested) */
#include "es_ws.h"      /* WebSocket transport (bounded outbound queue lives here) */
#include "es_proto.h"   /* protocol adapters: native / twilio / ... */

#define EARSHOT_EVENT_CONNECTED    "earshot::connected"
#define EARSHOT_EVENT_DISCONNECTED "earshot::disconnected"
#define EARSHOT_EVENT_ERROR        "earshot::error"
#define EARSHOT_EVENT_METRICS      "earshot::metrics"
#define EARSHOT_EVENT_READY        "earshot::ready"
#define EARSHOT_EVENT_COMMAND      "earshot::command"
#define EARSHOT_EVENT_SPEECH_START "earshot::speech_started"
#define EARSHOT_EVENT_SPEECH_STOP  "earshot::speech_stopped"
#define EARSHOT_EVENT_DTMF         "earshot::dtmf"

/* turn notifications sent to the agent (vad_notify=on) */
#define ES_TURN_START "{\"type\":\"speech_started\"}"
#define ES_TURN_STOP  "{\"type\":\"speech_stopped\"}"
#define ES_PLAY_BUF_MAX (2 * 1024 * 1024)  /* play-buffer cap: must fit a full burst-delivered
                                            * agent response (see es_sink_audio) */
#define ES_GREETING_MAX_MS 15000           /* welcome-greeting file is loaded up to this length */
#define ES_META_MAX 1024                   /* max EARSHOT_META bytes put on the handshake header */

#define EARSHOT_BUG_NAME           "earshot"

#define EARSHOT_SYNTAX \
    "<uuid> start <url> [id=<name>] [codec=l16|pcmu|pcma] [rate=8000|16000|24000] [dir=in|out|both]\n" \
    "\t  fan-out: many streams per channel via id=; dir=in is a read-only fork (transcription/monitor)\n" \
    "\t\t[proto=native|twilio|openai|deepgram|elevenlabs|gemini|pipecat]\n" \
    "\t\t[ready=firstframe|connect|manual]\n" \
    "\t\t[vad=on [vad_barge=on] [vad_notify=on] [vad_mode=-1..3] [vad_voice_ms=200] [vad_silence_ms=500]]\n" \
    "\t\t[interruptible=none|dtmf|speech|any] [ignore_backchannel=on] [sensitivity=low|medium|high]\n" \
    "\t\t[barge_min_ms=<n>] [barge_fade_ms=<n>]\n" \
    "\t\t[dtmf=on] [mask=on] [commands=true] [metrics=<seconds>] [corr=auto|<id>] [auth=<token> | EARSHOT_AUTH var]\n" \
    "\t\t[greeting=<file> | EARSHOT_GREETING var]  (caller context: EARSHOT_META -> X-Earshot-Meta header)\n" \
    "\t  control channel (commands=true): agent sends {\"type\":\"command\",\"action\":...}\n" \
    "\t  actions: transfer|hangup|send_dtmf|play|stop_play|record|setvar|hold|bridge|park\n" \
    "<uuid> stop\n" \
    "<uuid> pause | resume | flush\n" \
    "<uuid> send <text|json>\n" \
    "<uuid> mask on|off       (PCI: mute caller audio + suppress DTMF to the agent)\n" \
    "<uuid> status | metrics"

SWITCH_MODULE_LOAD_FUNCTION(mod_earshot_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_earshot_shutdown);
SWITCH_MODULE_DEFINITION(mod_earshot, mod_earshot_load, mod_earshot_shutdown, NULL);

/* --------------------------------------------------------------------------
 * Per-stream state (hangs off the session's private data + the media bug).
 * -------------------------------------------------------------------------- */
/* es_codec_t comes from es_codec.h; es_proto_kind_t from es_proto.h */
typedef enum { ES_DIR_IN, ES_DIR_OUT, ES_DIR_BOTH } es_dir_t;
/* ready-gate: when playback to the caller is allowed to start */
typedef enum {
    ES_READY_FIRSTFRAME = 0,   /* default: on the first audio frame from the agent */
    ES_READY_CONNECT,          /* as soon as the WebSocket connects */
    ES_READY_MANUAL            /* only after an explicit `resume` */
} es_ready_mode_t;

/* barge-in policy: what (if anything) interrupts the agent while it's speaking. */
typedef enum {
    ES_BARGE_NONE = 0,   /* agent finishes; the caller can't cut in */
    ES_BARGE_DTMF,       /* caller DTMF interrupts */
    ES_BARGE_SPEECH,     /* caller speech interrupts (module-side VAD) */
    ES_BARGE_ANY         /* speech OR DTMF */
} es_barge_t;

/* Fade-out scratch cap (samples). Clamps barge_fade_ms so the ramp uses a small stack buffer;
 * covers ~256 ms at 16 kHz / ~85 ms at 48 kHz — telephony channels are 8/16 kHz. */
#define ES_FADE_MAX_SAMPLES 4096

typedef struct {
    switch_core_session_t *session;    /* owning session (WS thread is joined before it is torn down) */
    char             url[1024];
    es_codec_t       codec;
    int              rate;          /* negotiated wire rate */
    es_dir_t         dir;
    es_proto_kind_t  proto;
    char             corr[256];    /* correlation id (auto -> Call-ID + UUID) */
    char             uuid[64];     /* channel uuid (for control-channel events) */
    char             id[64];       /* stream id for fan-out (empty = the default stream) */
    switch_bool_t    owns_dtmf_hook; /* this stream registered the session recv_dtmf hook */
    char             auth[512];
    switch_bool_t    allow_commands; /* control channel opt-in (commands=true) */
    switch_bool_t    dtmf_on;       /* capture caller DTMF -> events + agent */
    switch_bool_t    masking;       /* PCI window: mute caller audio + suppress DTMF to the agent */
    uint64_t         dtmf_count;    /* caller DTMF digits seen (metric) */

    switch_media_bug_t *bug;
    switch_media_bug_flag_t bug_flags; /* READ_STREAM and/or WRITE_REPLACE, from dir */

    /* module-side VAD / turn detection (works with any agent) */
    switch_vad_t    *vad;
    switch_bool_t    vad_on, vad_barge, vad_notify;
    int              vad_mode, vad_voice_ms, vad_silence_ms, vad_thresh;
    switch_bool_t    talking;       /* current VAD talk state */
    uint64_t         speech_starts; /* number of caller talk-spurts (metric) */

    /* barge-in policy (interruptible= / ignore_backchannel / sensitivity / barge_min_ms / barge_fade_ms) */
    es_barge_t       barge;         /* what interrupts the agent */
    switch_bool_t    barge_explicit;/* interruptible= was given (so vad_barge shouldn't override it) */
    int              barge_min_ms;  /* sustained caller speech before a speech barge (0 = immediate) */
    int              barge_fade_ms; /* fade playback out over this long instead of a hard cut (0 = hard) */
    switch_bool_t    barge_pending; /* a speech barge is deferred, waiting for barge_min_ms of speech */
    switch_time_t    barge_at;      /* fire the deferred barge at/after this time (µs) */
    int              chan_rate;     /* channel sample rate (for the fade ramp) */
    uint64_t         barges;        /* barge-ins performed (metric) */

    es_ready_mode_t  ready_mode;    /* when to open the ready-gate */
    switch_bool_t    ready;         /* ready-gate: playback allowed? (set off media path) */
    switch_bool_t    ready_announced; /* earshot::ready fired? (media thread only) */

    es_ws_t         *ws;            /* WebSocket transport (owns its own thread) */
    es_proto_ctx_t  *proto_ctx;     /* wire-protocol adapter (native/twilio/...) */
    es_proto_sink_t  sink;          /* inbound-message sink -> play_buf/flush/marks */
    switch_buffer_t *play_buf;      /* agent audio (decoded L16) awaiting playback */
    switch_mutex_t  *play_mutex;    /* guards play_buf: es_ws thread writes, media thread reads */
    switch_queue_t  *marks;         /* pending Twilio marks to echo once playback drains */

    /* welcome greeting: a file preloaded at channel rate, dropped into the playout the moment
     * the ready-gate opens so it leads the agent's first words (see the ready-announce block) */
    char             greeting_path[256];    /* greeting audio file (greeting= / EARSHOT_GREETING) */
    int16_t         *greeting_pcm;          /* preloaded greeting, L16 mono @ chan_rate (pool-owned) */
    size_t           greeting_samples;      /* samples in greeting_pcm (0 = no greeting) */
    switch_bool_t    greeting_done;         /* already injected into play_buf */

    /* metrics counters (tx = media thread; rx/drops = ws thread; racy reads are fine for stats) */
    uint64_t         tx_frames, tx_bytes;   /* caller -> agent */
    uint64_t         rx_frames, rx_bytes;   /* agent -> caller */
    uint64_t         play_drops;            /* agent frames dropped when the play buffer was full */
    uint64_t         commands;              /* control-channel commands executed */
    switch_time_t    start_time;            /* stream start (µs) for duration */
    switch_time_t    last_metrics;          /* last periodic metrics emit (µs) */
    int              metrics_interval;      /* seconds between earshot::metrics events (0 = off) */

    /* latency KPIs */
    switch_bool_t    first_audio_seen;
    uint64_t         first_audio_ms;        /* start -> first agent audio frame */
    switch_bool_t    awaiting_response;     /* caller stopped talking, waiting on the agent */
    switch_time_t    turn_end_time;         /* last speech_stopped (µs) */
    uint64_t         resp_ms_last, resp_ms_max, turns;  /* per-turn response latency */
} es_stream_t;

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

/* Fire an earshot:: custom event carrying the channel uuid (+ optional body). */
static void es_fire_event(switch_core_session_t *session, const char *subclass,
                          const char *key, const char *val)
{
    switch_event_t *event = NULL;
    switch_channel_t *channel = switch_core_session_get_channel(session);

    if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, subclass) != SWITCH_STATUS_SUCCESS) {
        return;
    }
    switch_channel_event_set_data(channel, event);           /* adds Unique-ID, etc. */
    if (key && val) {
        switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, key, val);
    }
    switch_event_fire(&event);
}

/* Fire an earshot::metrics event with the current counters (frames/bytes/drops/ws). */
static void es_fire_metrics(switch_core_session_t *session, es_stream_t *st)
{
    switch_event_t *event = NULL;
    switch_channel_t *channel = switch_core_session_get_channel(session);
    es_ws_stats_t ws;
    uint64_t dur_ms;

    es_ws_get_stats(st->ws, &ws);
    dur_ms = st->start_time ? (uint64_t)(switch_micro_time_now() - st->start_time) / 1000 : 0;

    if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, EARSHOT_EVENT_METRICS) != SWITCH_STATUS_SUCCESS)
        return;
    switch_channel_event_set_data(channel, event);
    switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "corr", st->corr);
    switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "stream-id", st->id[0] ? st->id : "default");
    switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "proto", es_proto_name(st->proto));
    switch_event_add_header(event, SWITCH_STACK_BOTTOM, "tx-frames",      "%" SWITCH_UINT64_T_FMT, st->tx_frames);
    switch_event_add_header(event, SWITCH_STACK_BOTTOM, "tx-bytes",       "%" SWITCH_UINT64_T_FMT, st->tx_bytes);
    switch_event_add_header(event, SWITCH_STACK_BOTTOM, "rx-frames",      "%" SWITCH_UINT64_T_FMT, st->rx_frames);
    switch_event_add_header(event, SWITCH_STACK_BOTTOM, "rx-bytes",       "%" SWITCH_UINT64_T_FMT, st->rx_bytes);
    switch_event_add_header(event, SWITCH_STACK_BOTTOM, "play-drops",     "%" SWITCH_UINT64_T_FMT, st->play_drops);
    switch_event_add_header(event, SWITCH_STACK_BOTTOM, "commands",       "%" SWITCH_UINT64_T_FMT, st->commands);
    switch_event_add_header(event, SWITCH_STACK_BOTTOM, "speech-starts",   "%" SWITCH_UINT64_T_FMT, st->speech_starts);
    switch_event_add_header(event, SWITCH_STACK_BOTTOM, "barges",          "%" SWITCH_UINT64_T_FMT, st->barges);
    switch_event_add_header(event, SWITCH_STACK_BOTTOM, "talking",         "%d", st->talking);
    switch_event_add_header(event, SWITCH_STACK_BOTTOM, "dtmf",            "%" SWITCH_UINT64_T_FMT, st->dtmf_count);
    switch_event_add_header(event, SWITCH_STACK_BOTTOM, "masking",         "%d", st->masking);
    switch_event_add_header(event, SWITCH_STACK_BOTTOM, "play-buffered",  "%u",
                            st->play_buf ? (unsigned) switch_buffer_inuse(st->play_buf) : 0);
    switch_event_add_header(event, SWITCH_STACK_BOTTOM, "ws-connected",   "%d", ws.connected);
    switch_event_add_header(event, SWITCH_STACK_BOTTOM, "ws-reconnects",  "%u", ws.reconnects);
    switch_event_add_header(event, SWITCH_STACK_BOTTOM, "ws-queue-drops", "%" SWITCH_UINT64_T_FMT, ws.queue_drops);
    switch_event_add_header(event, SWITCH_STACK_BOTTOM, "ws-rtt-ms",      "%ld", ws.rtt_ms);
    switch_event_add_header(event, SWITCH_STACK_BOTTOM, "first-audio-ms", "%" SWITCH_UINT64_T_FMT, st->first_audio_ms);
    switch_event_add_header(event, SWITCH_STACK_BOTTOM, "response-ms",    "%" SWITCH_UINT64_T_FMT, st->resp_ms_last);
    switch_event_add_header(event, SWITCH_STACK_BOTTOM, "response-ms-max","%" SWITCH_UINT64_T_FMT, st->resp_ms_max);
    switch_event_add_header(event, SWITCH_STACK_BOTTOM, "turns",          "%" SWITCH_UINT64_T_FMT, st->turns);
    switch_event_add_header(event, SWITCH_STACK_BOTTOM, "duration-ms",    "%" SWITCH_UINT64_T_FMT, dur_ms);
    switch_event_fire(&event);
}

/* Fade the queued playback out over ~fade_ms instead of a hard cut, so barge-in doesn't click.
 * Keeps the immediate next fade_ms of audio, ramps it 1->0, drops everything after. Caller holds
 * play_mutex. */
static void es_fade_out(switch_buffer_t *buf, int fade_ms, int rate)
{
    int16_t tmp[ES_FADE_MAX_SAMPLES];
    size_t have = switch_buffer_inuse(buf);
    size_t n = (size_t) fade_ms * (rate > 0 ? rate : 8000) / 1000;
    if (n > ES_FADE_MAX_SAMPLES) n = ES_FADE_MAX_SAMPLES;
    if (n * 2 > have) n = have / 2;
    if (n == 0) { switch_buffer_zero(buf); return; }
    switch_buffer_read(buf, tmp, n * 2);   /* pull the immediate next audio out */
    switch_buffer_zero(buf);               /* drop everything queued after the fade tail */
    for (size_t i = 0; i < n; i++)         /* linear ramp 1.0 -> 0.0 across the tail */
        tmp[i] = (int16_t) ((int32_t) tmp[i] * (int32_t) (n - i) / (int32_t) n);
    switch_buffer_write(buf, tmp, n * 2);  /* the caller now hears a fade to silence, not a cut */
}

/* Interrupt the agent: fade or hard-cut the queued playback per barge_fade_ms. */
static void es_barge_now(es_stream_t *st)
{
    if (!st->play_buf) return;
    switch_mutex_lock(st->play_mutex);
    if (st->barge_fade_ms > 0) es_fade_out(st->play_buf, st->barge_fade_ms, st->chan_rate);
    else                       switch_buffer_zero(st->play_buf);
    switch_mutex_unlock(st->play_mutex);
    st->barges++;
}

/* Caller DTMF hook: capture digits -> earshot::dtmf + forward to the agent. During a
 * masking window the digit is suppressed from the agent (audit event carries no digit).
 * Always returns SUCCESS so the digit still reaches FreeSWITCH (a secure collector/dialplan). */
static switch_status_t es_dtmf_hook(switch_core_session_t *session, const switch_dtmf_t *dtmf,
                                    switch_dtmf_direction_t direction)
{
    switch_channel_t *channel = switch_core_session_get_channel(session);
    es_stream_t *st = (es_stream_t *) switch_channel_get_private(channel, EARSHOT_BUG_NAME);
    char d[2];
    if (!st || direction != SWITCH_DTMF_RECV) return SWITCH_STATUS_SUCCESS;
    st->dtmf_count++;
    if (st->masking) {
        es_fire_event(session, EARSHOT_EVENT_DTMF, "masked", "true");   /* audit only, digit redacted */
    } else if (st->dtmf_on) {
        d[0] = dtmf->digit; d[1] = 0;
        es_fire_event(session, EARSHOT_EVENT_DTMF, "digit", d);
        if (st->ws) es_proto_send_dtmf(st->proto_ctx, st->ws, d);
    }
    /* barge on caller DTMF (interruptible=dtmf|any), but not during a card-entry masking window */
    if (!st->masking && (st->barge == ES_BARGE_DTMF || st->barge == ES_BARGE_ANY))
        es_barge_now(st);
    return SWITCH_STATUS_SUCCESS;
}

/* Per-stream channel-private key: the default (unnamed) stream keeps EARSHOT_BUG_NAME for
 * back-compat; named fan-out streams use "earshot_<id>". */
static const char *es_privkey(switch_core_session_t *session, const char *id)
{
    if (!id || !id[0]) return EARSHOT_BUG_NAME;
    return switch_core_session_sprintf(session, EARSHOT_BUG_NAME "_%s", id);
}

/* Parse one key=value option into the stream config. Returns SWITCH_TRUE if consumed. */
static switch_bool_t es_apply_option(es_stream_t *st, const char *kv)
{
    char key[64] = {0}, *eq = strchr(kv, '=');
    const char *val;
    if (!eq) return SWITCH_FALSE;
    switch_copy_string(key, kv, (switch_size_t)(eq - kv) + 1);
    val = eq + 1;

    if (!strcasecmp(key, "codec")) {
        st->codec = es_codec_from_name(val);   /* es_codec.h */
    } else if (!strcasecmp(key, "rate")) {
        st->rate = atoi(val);
    } else if (!strcasecmp(key, "dir")) {
        st->dir = !strcasecmp(val, "in") ? ES_DIR_IN :
                  !strcasecmp(val, "out") ? ES_DIR_OUT : ES_DIR_BOTH;
    } else if (!strcasecmp(key, "proto")) {
        st->proto = es_proto_from_name(val);
    } else if (!strcasecmp(key, "ready")) {
        st->ready_mode = !strcasecmp(val, "connect") ? ES_READY_CONNECT :
                         !strcasecmp(val, "manual")  ? ES_READY_MANUAL : ES_READY_FIRSTFRAME;
    } else if (!strcasecmp(key, "vad")) {
        st->vad_on = (switch_bool_t) switch_true(val);
    } else if (!strcasecmp(key, "vad_mode")) {
        st->vad_mode = atoi(val);                /* -1 native, 0..3 fvad aggressiveness */
    } else if (!strcasecmp(key, "vad_voice_ms")) {
        st->vad_voice_ms = atoi(val);            /* speech duration before speech_started */
    } else if (!strcasecmp(key, "vad_silence_ms")) {
        st->vad_silence_ms = atoi(val);          /* silence (endpoint) before speech_stopped */
    } else if (!strcasecmp(key, "vad_thresh")) {
        st->vad_thresh = atoi(val);              /* energy threshold */
    } else if (!strcasecmp(key, "vad_barge")) {
        st->vad_barge = (switch_bool_t) switch_true(val);   /* back-compat alias for interruptible=speech */
        if (st->vad_barge && !st->barge_explicit) st->barge = ES_BARGE_SPEECH;
    } else if (!strcasecmp(key, "interruptible")) {         /* what interrupts the agent */
        st->barge = !strcasecmp(val, "any")    ? ES_BARGE_ANY    :
                    !strcasecmp(val, "dtmf")   ? ES_BARGE_DTMF   :
                    !strcasecmp(val, "speech") ? ES_BARGE_SPEECH : ES_BARGE_NONE;
        st->barge_explicit = SWITCH_TRUE;
    } else if (!strcasecmp(key, "ignore_backchannel")) {   /* drop short "yeah/okay" — require sustained speech */
        if (switch_true(val) && st->barge_min_ms <= 0) st->barge_min_ms = 300;
    } else if (!strcasecmp(key, "sensitivity")) {          /* preset for the sustained-speech gate */
        st->barge_min_ms = !strcasecmp(val, "high") ? 150 : !strcasecmp(val, "low") ? 600 : 300;
    } else if (!strcasecmp(key, "barge_min_ms")) {
        st->barge_min_ms = atoi(val);                      /* sustained speech before a barge (0 = immediate) */
    } else if (!strcasecmp(key, "barge_fade_ms")) {
        st->barge_fade_ms = atoi(val);                     /* fade playback out this long vs a hard cut */
    } else if (!strcasecmp(key, "vad_notify")) {
        st->vad_notify = (switch_bool_t) switch_true(val);  /* send turn JSON to the agent */
    } else if (!strcasecmp(key, "dtmf")) {
        st->dtmf_on = (switch_bool_t) switch_true(val);         /* capture + forward caller DTMF */
    } else if (!strcasecmp(key, "mask")) {
        st->masking = (switch_bool_t) switch_true(val);         /* start in a masking window */
    } else if (!strcasecmp(key, "commands")) {
        st->allow_commands = (switch_bool_t) switch_true(val);  /* control channel opt-in */
    } else if (!strcasecmp(key, "metrics")) {
        st->metrics_interval = atoi(val);       /* seconds between earshot::metrics events */
    } else if (!strcasecmp(key, "id")) {
        if (strcasecmp(val, "default")) switch_copy_string(st->id, val, sizeof st->id);  /* fan-out stream id */
    } else if (!strcasecmp(key, "corr")) {
        switch_copy_string(st->corr, val, sizeof st->corr);
    } else if (!strcasecmp(key, "auth")) {
        switch_copy_string(st->auth, val, sizeof st->auth);
    } else if (!strcasecmp(key, "greeting")) {
        switch_copy_string(st->greeting_path, val, sizeof st->greeting_path);  /* path w/o spaces; else EARSHOT_GREETING */
    } else {
        return SWITCH_FALSE;
    }
    return SWITCH_TRUE;
}

/* Preload the welcome-greeting file into a channel-rate L16 mono buffer (pool-owned), so the
 * ready-announce block can drop it into the playout without any file I/O on the media thread.
 * The core file layer resamples to chan_rate for us. Bounded by ES_GREETING_MAX_MS; any error
 * just leaves greeting_samples == 0 and the greeting is skipped. */
static void es_greeting_load(es_stream_t *st, switch_memory_pool_t *pool, const char *path, int rate)
{
    switch_file_handle_t fh = { 0 };
    size_t cap, n = 0;
    int16_t *buf;
    if (!path || !path[0] || rate <= 0) return;
    if (switch_core_file_open(&fh, path, 1, (uint32_t) rate,
            SWITCH_FILE_FLAG_READ | SWITCH_FILE_DATA_SHORT, pool) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
            "earshot: greeting '%s' could not be opened; continuing without it\n", path);
        return;
    }
    cap = (size_t) rate * ES_GREETING_MAX_MS / 1000;          /* sample cap (mono) */
    buf = switch_core_alloc(pool, cap * sizeof(int16_t));
    while (buf && n < cap) {
        switch_size_t got = cap - n;                          /* samples to read this pass */
        if (switch_core_file_read(&fh, buf + n, &got) != SWITCH_STATUS_SUCCESS || got == 0) break;
        n += got;
    }
    switch_core_file_close(&fh);
    if (buf && n) { st->greeting_pcm = buf; st->greeting_samples = n; }
}

/* Drop the welcome greeting into the playout exactly once, ahead of any agent audio. Called from
 * whichever thread opens the ready-gate — the WS thread on the first agent frame (before that
 * frame is written) or the media thread at ready-announce (if the agent is still silent) — so the
 * greeting always leads regardless of ready= mode. play_mutex + greeting_done keep it once. */
static void es_greeting_emit(es_stream_t *st)
{
    if (!st->greeting_pcm || !st->play_buf) return;
    switch_mutex_lock(st->play_mutex);
    if (!st->greeting_done) {
        switch_buffer_write(st->play_buf, st->greeting_pcm, st->greeting_samples * 2);
        st->greeting_done = SWITCH_TRUE;
    }
    switch_mutex_unlock(st->play_mutex);
}

/* ---- protocol sink: decoded inbound messages land here (es_ws thread) --- */
/* agent audio -> playback buffer, gated by the ready-gate + a ~2 s backlog cap. */
static void es_sink_audio(void *user, const int16_t *pcm, size_t nsamples)
{
    es_stream_t *st = (es_stream_t *) user;
    st->rx_frames++;
    st->rx_bytes += nsamples * 2;
    if (!st->first_audio_seen) {                 /* time-to-first-audio */
        st->first_audio_seen = SWITCH_TRUE;
        if (st->start_time) st->first_audio_ms = (uint64_t)(switch_micro_time_now() - st->start_time) / 1000;
    }
    if (st->awaiting_response && st->turn_end_time) {   /* per-turn response latency (needs VAD) */
        uint64_t r = (uint64_t)(switch_micro_time_now() - st->turn_end_time) / 1000;
        st->awaiting_response = SWITCH_FALSE;
        st->resp_ms_last = r;
        if (r > st->resp_ms_max) st->resp_ms_max = r;
        st->turns++;
    }
    if (!st->ready) {
        /* first-frame gate opens here; connect/manual gates are opened elsewhere */
        if (st->ready_mode == ES_READY_FIRSTFRAME) st->ready = SWITCH_TRUE;
        else return;                       /* manual/connect not yet ready -> drop */
    }
    if (!st->play_buf) return;
    es_greeting_emit(st);                  /* greeting leads: emitted before this first agent audio */
    switch_mutex_lock(st->play_mutex);
    /* The cap must hold an ENTIRE agent response, not just a jitter cushion: realtime
     * vendors (OpenAI GA especially) deliver a full answer several times faster than
     * realtime, so a small cap chops every long reply ~2s in — heard as mingled /
     * overlapped fragments (drops counted, audibly confirmed on a live phone test).
     * 2MB = ~130s @8k L16 / ~65s @16k. Barge-in flush still empties it instantly,
     * and the cap still bounds a stuck playback. */
    if (switch_buffer_inuse(st->play_buf) < ES_PLAY_BUF_MAX)
        switch_buffer_write(st->play_buf, pcm, nsamples * 2);
    else
        st->play_drops++;                  /* playback isn't draining: drop new agent audio */
    switch_mutex_unlock(st->play_mutex);
}

/* barge-in: drop any queued agent audio immediately (native clear / twilio clear). */
static void es_sink_clear(void *user)
{
    es_stream_t *st = (es_stream_t *) user;
    if (st->play_buf) {
        switch_mutex_lock(st->play_mutex);
        switch_buffer_zero(st->play_buf);
        switch_mutex_unlock(st->play_mutex);
    }
}

/* twilio mark from the agent: remember it; echo back once the audio before it drains. */
static void es_sink_mark(void *user, const char *name)
{
    es_stream_t *st = (es_stream_t *) user;
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "earshot: queued mark '%s'\n", name ? name : "");
    if (st->marks && name) switch_queue_trypush(st->marks, strdup(name));
}

/* agent-signalled DTMF (e.g. Deepgram): the caller's own DTMF is audited via earshot::dtmf on
 * the read path; DTMF the agent wants to *send* is a telephony action — use the control channel's
 * send_dtmf action rather than this inbound sink, which is intentionally a no-op. */
static void es_sink_dtmf(void *user, const char *digit) { (void) user; (void) digit; }

/* control channel: run the whitelisted uuid_* API on the caller's channel. Runs on the ws
 * thread; the uuid_* APIs locate the session themselves (the event-socket pattern). Gated by
 * commands=true so an agent can't drive the call unless the dialplan opted in. */
static void es_sink_command(void *user, const char *action, const char *api, const char *arg, const char *id)
{
    es_stream_t *st = (es_stream_t *) user;
    switch_stream_handle_t stream = { 0 };
    switch_status_t rc = SWITCH_STATUS_FALSE;
    const char *err = NULL;
    char resp[512];

    if (!st->allow_commands) err = "disabled";
    else if (!api)           err = "unsupported";

    if (!err && !strcasecmp(api, "__mask__")) {        /* earshot-internal, not a FreeSWITCH API */
        st->masking = (switch_bool_t) switch_true(arg);
        st->commands++;
        rc = SWITCH_STATUS_SUCCESS;
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "earshot: masking %s (agent)\n", st->masking ? "ON" : "OFF");
    } else if (!err) {
        SWITCH_STANDARD_STREAM(stream);
        rc = switch_api_execute(api, arg, NULL, &stream);
        st->commands++;
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                          "earshot command: %s %s -> %s\n", api, arg, stream.data ? (char *) stream.data : "");
    }

    {   /* audit event (not channel-bound; we're on the ws thread) */
        switch_event_t *ev = NULL;
        if (switch_event_create_subclass(&ev, SWITCH_EVENT_CUSTOM, EARSHOT_EVENT_COMMAND) == SWITCH_STATUS_SUCCESS) {
            switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "Unique-ID", st->uuid);
            switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "corr", st->corr);
            switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "action", action ? action : "");
            if (api) switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "api", api);
            switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "ok",
                                           (!err && rc == SWITCH_STATUS_SUCCESS) ? "true" : "false");
            if (err) switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "error", err);
            else if (stream.data) switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "result", (char *) stream.data);
            switch_event_fire(&ev);
        }
    }

    if (st->ws) {   /* echo a result back to the agent */
        if (err)
            snprintf(resp, sizeof resp, "{\"type\":\"command_result\",\"id\":\"%s\",\"action\":\"%s\",\"ok\":false,\"error\":\"%s\"}",
                     id ? id : "", action ? action : "", err);
        else
            snprintf(resp, sizeof resp, "{\"type\":\"command_result\",\"id\":\"%s\",\"action\":\"%s\",\"ok\":%s}",
                     id ? id : "", action ? action : "", rc == SWITCH_STATUS_SUCCESS ? "true" : "false");
        es_ws_send_text(st->ws, resp, strlen(resp));
    }
    switch_safe_free(stream.data);
}

/* ---- WebSocket callbacks (run on the es_ws service thread) ------------- */
static void es_on_ws_event(void *user, int connected, int code, const char *reason)
{
    es_stream_t *st = (es_stream_t *) user;
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                      "earshot ws %s: %s\n", connected ? "connected" : "closed", reason ? reason : "");
    if (connected) {
        /* (Re)send the proto preamble on EVERY (re)connect: a fresh WebSocket needs the
         * session handshake again (e.g. OpenAI session.update / Twilio start), otherwise a
         * reconnected agent is left unconfigured. */
        es_proto_send_start(st->proto_ctx, st->ws);
        if (st->ready_mode == ES_READY_CONNECT) st->ready = SWITCH_TRUE;
    } else if (st->session) {
        /* code < 0 = connect/handshake failure, otherwise a normal close (incl. reconnect churn).
         * Safe to fire from the ws service thread: es_ws_stop mutes further callbacks and
         * es_ws_destroy waits for the service thread to go quiescent before we free st. */
        es_fire_event(st->session, code < 0 ? EARSHOT_EVENT_ERROR : EARSHOT_EVENT_DISCONNECTED,
                      "reason", reason ? reason : "");
    }
}

static void es_on_ws_binary(void *user, const void *data, size_t len)
{
    es_stream_t *st = (es_stream_t *) user;
    es_proto_on_binary(st->proto_ctx, data, len, &st->sink);
}

static void es_on_ws_text(void *user, const char *data, size_t len)
{
    es_stream_t *st = (es_stream_t *) user;
    es_proto_on_text(st->proto_ctx, data, len, &st->sink);
}

static switch_bool_t es_media_bug_cb(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
    es_stream_t *st = (es_stream_t *) user_data;

    switch (type) {
    case SWITCH_ABC_TYPE_INIT:
        break;

    case SWITCH_ABC_TYPE_READ: {         /* caller audio -> agent (single read per cb) */
        uint8_t rbuf[SWITCH_RECOMMENDED_BUFFER_SIZE];
        switch_frame_t frame = { 0 };
        frame.data = rbuf;
        frame.buflen = sizeof(rbuf);
        if (switch_core_media_bug_read(bug, &frame, SWITCH_FALSE) == SWITCH_STATUS_SUCCESS && frame.datalen) {
            /* masking window (PCI): don't leak caller audio to the agent */
            if (st->ws && es_ws_connected(st->ws) && !st->masking) {
                /* the proto adapter frames it (native binary / twilio media JSON) */
                es_proto_send_audio(st->proto_ctx, st->ws, (const int16_t *) frame.data, frame.datalen / 2);
                st->tx_frames++;
                st->tx_bytes += frame.datalen;
            }
            /* module-side turn detection on the caller audio (independent of the agent) */
            if (st->vad) {
                switch_vad_state_t vs = switch_vad_process(st->vad, (int16_t *) frame.data, frame.datalen / 2);
                if (vs == SWITCH_VAD_STATE_START_TALKING && !st->talking) {
                    switch_core_session_t *s = switch_core_media_bug_get_session(bug);
                    st->talking = SWITCH_TRUE; st->speech_starts++;
                    if (s) {
                        switch_channel_set_variable(switch_core_session_get_channel(s), "earshot_talking", "true");
                        es_fire_event(s, EARSHOT_EVENT_SPEECH_START, "corr", st->corr);
                    }
                    if (st->barge == ES_BARGE_SPEECH || st->barge == ES_BARGE_ANY) {
                        if (st->barge_min_ms <= 0) {
                            es_barge_now(st);                    /* interrupt immediately */
                        } else {                                 /* defer: needs sustained speech (backchannel filter) */
                            st->barge_pending = SWITCH_TRUE;
                            /* Wait out the VAD's silence hangover on top of barge_min_ms: `talking` stays
                             * latched until vad_silence_ms of trailing silence, so a short backchannel's
                             * STOP_TALKING (which cancels barge_pending) must land first. Effective filter:
                             * a caller who keeps voicing past ~vad_voice_ms + barge_min_ms. */
                            st->barge_at = switch_micro_time_now()
                                         + (switch_time_t) (st->barge_min_ms + st->vad_silence_ms) * 1000;
                        }
                    }
                    if (st->vad_notify && st->ws) es_ws_send_text(st->ws, ES_TURN_START, strlen(ES_TURN_START));
                } else if (vs == SWITCH_VAD_STATE_STOP_TALKING && st->talking) {
                    switch_core_session_t *s = switch_core_media_bug_get_session(bug);
                    st->talking = SWITCH_FALSE;
                    st->barge_pending = SWITCH_FALSE;            /* speech ended before the gate -> it was a backchannel */
                    st->turn_end_time = switch_micro_time_now();   /* start the response-latency clock */
                    st->awaiting_response = SWITCH_TRUE;
                    if (s) {
                        switch_channel_set_variable(switch_core_session_get_channel(s), "earshot_talking", "false");
                        es_fire_event(s, EARSHOT_EVENT_SPEECH_STOP, "corr", st->corr);
                    }
                    if (st->vad_notify && st->ws) es_ws_send_text(st->ws, ES_TURN_STOP, strlen(ES_TURN_STOP));
                }
                /* deferred speech barge: fire once the caller sustains speech past barge_min_ms
                 * (short backchannels like "yeah/okay" stop first and never trip it) */
                if (st->barge_pending && st->talking && switch_micro_time_now() >= st->barge_at) {
                    es_barge_now(st);
                    st->barge_pending = SWITCH_FALSE;
                }
            }
        }
        /* announce the ready-gate exactly once, here on the media thread (valid session) */
        if (st->ready && !st->ready_announced) {
            switch_core_session_t *s = switch_core_media_bug_get_session(bug);
            st->ready_announced = SWITCH_TRUE;
            es_greeting_emit(st);   /* covers the agent-still-silent case; no-op if already emitted */
            if (s) {
                switch_channel_set_variable(switch_core_session_get_channel(s), "earshot_ready", "true");
                es_fire_event(s, EARSHOT_EVENT_READY, "corr", st->corr);
            }
        }
        /* periodic metrics (metrics=<seconds> option) */
        if (st->metrics_interval > 0) {
            switch_time_t now = switch_micro_time_now();
            if (now - st->last_metrics >= (switch_time_t) st->metrics_interval * 1000000) {
                switch_core_session_t *s = switch_core_media_bug_get_session(bug);
                st->last_metrics = now;
                if (s) es_fire_metrics(s, st);
            }
        }
        break;
    }

    case SWITCH_ABC_TYPE_WRITE_REPLACE: {   /* agent audio -> caller */
        /* MUST set a replace frame every time this fires (else FS derefs an unset
         * frame -> crash). Replace outbound audio with buffered agent audio; pad
         * silence when the agent hasn't sent enough yet. */
        switch_frame_t *frame = switch_core_media_bug_get_write_replace_frame(bug);
        if (frame && frame->data && frame->datalen && st->play_buf) {
            switch_size_t need = frame->datalen, got = 0, left;
            switch_mutex_lock(st->play_mutex);
            if (switch_buffer_inuse(st->play_buf) >= need)
                got = switch_buffer_read(st->play_buf, frame->data, need);
            left = switch_buffer_inuse(st->play_buf);
            switch_mutex_unlock(st->play_mutex);
            if (got < need) memset((uint8_t *) frame->data + got, 0, need - got);
            switch_core_media_bug_set_write_replace_frame(bug, frame);
            /* Twilio mark: echo pending marks once the agent's audio has drained
             * (less than a full frame remaining = the utterance has played out). */
            if (left < need && st->marks) {
                void *m;
                while (switch_queue_trypop(st->marks, &m) == SWITCH_STATUS_SUCCESS) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
                                      "earshot: echoing mark '%s' (playback drained)\n", (const char *) m);
                    es_proto_send_mark(st->proto_ctx, st->ws, (const char *) m);
                    free(m);
                }
            }
        }
        break;
    }

    case SWITCH_ABC_TYPE_CLOSE: {        /* single teardown path */
        switch_core_session_t *s = switch_core_media_bug_get_session(bug);
        if (s) {
            if (st->owns_dtmf_hook) switch_core_event_hook_remove_recv_dtmf(s, es_dtmf_hook);
            switch_channel_set_private(switch_core_session_get_channel(s), es_privkey(s, st->id), NULL);
            es_fire_metrics(s, st);      /* final snapshot while ws stats are still available */
        }
        if (st->ws) {
            es_proto_send_stop(st->proto_ctx, st->ws);         /* best-effort postamble */
            es_ws_destroy(st->ws); st->ws = NULL;              /* joins ws thread first */
        }
        if (st->proto_ctx) { es_proto_destroy(st->proto_ctx); st->proto_ctx = NULL; }
        if (st->vad) switch_vad_destroy(&st->vad);
        if (st->marks) { void *m; while (switch_queue_trypop(st->marks, &m) == SWITCH_STATUS_SUCCESS) free(m); }
        if (st->play_buf) {
            switch_mutex_lock(st->play_mutex);
            switch_buffer_destroy(&st->play_buf);
            switch_mutex_unlock(st->play_mutex);
        }
        break;
    }

    default:
        break;
    }
    return SWITCH_TRUE;
}

/* --------------------------------------------------------------------------
 * Subcommand handlers (stubs)
 * -------------------------------------------------------------------------- */
static switch_status_t es_start(switch_core_session_t *session, int argc, char **argv, switch_stream_handle_t *stream)
{
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_memory_pool_t *pool = switch_core_session_get_pool(session);
    es_stream_t *st;
    const char *cid;
    int i;

    if (argc < 3) { stream->write_function(stream, "-ERR usage: %s\n", EARSHOT_SYNTAX); return SWITCH_STATUS_FALSE; }

    st = switch_core_alloc(pool, sizeof(*st));   /* pool alloc zeroes */
    st->session = session;
    switch_copy_string(st->url, argv[2], sizeof(st->url));
    st->codec = ES_CODEC_PCMU; st->rate = 8000; st->dir = ES_DIR_BOTH;   /* g711 8k telephony default */
    st->proto = ES_PROTO_NATIVE;
    st->ready = SWITCH_FALSE;
    st->vad_mode = 2; st->vad_voice_ms = 200; st->vad_silence_ms = 500;   /* telephony defaults */
    st->start_time = st->last_metrics = switch_micro_time_now();

    for (i = 3; i < argc; i++) es_apply_option(st, argv[i]);

    /* The option list is split on spaces, so an `auth=Bearer <token>` value would be
     * truncated to just "Bearer". EARSHOT_AUTH carries the full Authorization value
     * verbatim (spaces intact) — the reliable way to pass "Bearer <key>"/"Token <key>". */
    {
        const char *a = switch_channel_get_variable(channel, "EARSHOT_AUTH");
        if (a && *a) switch_copy_string(st->auth, a, sizeof st->auth);
    }

    /* correlation-first: default corr = SIP Call-ID (the two-key model) */
    if (!st->corr[0] || !strcasecmp(st->corr, "auto")) {
        cid = switch_channel_get_variable(channel, "sip_call_id");
        switch_copy_string(st->corr, cid ? cid : switch_core_session_get_uuid(session), sizeof st->corr);
    }

    /* fan-out: this stream's private key, and reject a duplicate id on the same channel */
    {
        const char *key = es_privkey(session, st->id);
        if (switch_channel_get_private(channel, key)) {
            stream->write_function(stream, "-ERR earshot stream '%s' already active\n", st->id[0] ? st->id : "default");
            return SWITCH_STATUS_FALSE;
        }
    }
    /* media-bug flags follow dir: forks (dir=in) are read-only so only the agent owns WRITE_REPLACE */
    {
        switch_media_bug_flag_t flags = 0;
        if (st->dir == ES_DIR_IN  || st->dir == ES_DIR_BOTH) flags |= SMBF_READ_STREAM;
        if (st->dir == ES_DIR_OUT || st->dir == ES_DIR_BOTH) flags |= SMBF_WRITE_REPLACE;
        st->bug_flags = flags;
    }

    /* protocol adapter (native/twilio/openai/deepgram): some protos pin the wire codec.
     * openai/deepgram may take a full session config from a channel variable. The adapter
     * resamples between the channel rate and the wire rate when they differ. */
    st->codec = es_proto_force_codec(st->proto, st->codec);
    {
        switch_codec_implementation_t read_impl;
        int chan_rate = 8000;
        if (switch_core_session_get_read_impl(session, &read_impl) == SWITCH_STATUS_SUCCESS
            && read_impl.actual_samples_per_second)
            chan_rate = read_impl.actual_samples_per_second;
        st->chan_rate = chan_rate;                     /* for the barge fade-out ramp */
        /* preload the welcome greeting at the channel rate — playback-capable streams only
         * (read-only dir=in forks never play out). Option path first, else EARSHOT_GREETING. */
        if (st->bug_flags & SMBF_WRITE_REPLACE) {
            if (!st->greeting_path[0]) {
                const char *g = switch_channel_get_variable(channel, "EARSHOT_GREETING");
                if (g) switch_copy_string(st->greeting_path, g, sizeof st->greeting_path);
            }
            if (st->greeting_path[0]) es_greeting_load(st, pool, st->greeting_path, chan_rate);
        }
        st->proto_ctx = es_proto_create(st->proto, st->codec, st->rate, chan_rate, st->corr,
                                        switch_core_session_get_uuid(session),
                                        switch_channel_get_variable(channel, "EARSHOT_SESSION_CONFIG"));

        /* module-side VAD runs on the channel-rate read stream */
        if (st->vad_on && (st->vad = switch_vad_init(chan_rate, 1))) {
            switch_vad_set_mode(st->vad, st->vad_mode);
            if (st->vad_voice_ms)   switch_vad_set_param(st->vad, "voice_ms",   st->vad_voice_ms);
            if (st->vad_silence_ms) switch_vad_set_param(st->vad, "silence_ms", st->vad_silence_ms);
            if (st->vad_thresh)     switch_vad_set_param(st->vad, "thresh",     st->vad_thresh);
        }
        /* a speech barge needs the module VAD; warn if the operator asked for one without vad=on */
        if ((st->barge == ES_BARGE_SPEECH || st->barge == ES_BARGE_ANY) && !st->vad)
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                "earshot: interruptible=%s needs vad=on for the speech barge; "
                "without it only DTMF (if enabled) interrupts\n",
                st->barge == ES_BARGE_ANY ? "any" : "speech");
    }
    if (!st->proto_ctx) { stream->write_function(stream, "-ERR proto init failed\n"); return SWITCH_STATUS_FALSE; }
    st->sink.user     = st;
    st->sink.on_audio = es_sink_audio;
    st->sink.on_clear = es_sink_clear;
    st->sink.on_mark  = es_sink_mark;
    st->sink.on_dtmf  = es_sink_dtmf;
    st->sink.on_command = es_sink_command;
    switch_copy_string(st->uuid, switch_core_session_get_uuid(session), sizeof st->uuid);
    switch_queue_create(&st->marks, 64, pool);

    /* playback buffer for agent audio (byte-granular so WRITE_REPLACE pulls exact frames);
     * read-only forks (dir=in) never play back, so they don't allocate one */
    switch_mutex_init(&st->play_mutex, SWITCH_MUTEX_NESTED, pool);
    if (st->bug_flags & SMBF_WRITE_REPLACE)
        switch_buffer_create_dynamic(&st->play_buf, 1024, 8192, 0);

    /* open the WebSocket (correlation headers injected on the handshake) */
    {
        es_ws_opts_t o;
        memset(&o, 0, sizeof o);
        o.url              = st->url;
        o.auth             = st->auth[0] ? st->auth : NULL;
        o.reconnect        = switch_true(switch_channel_get_variable(channel, "EARSHOT_NO_RECONNECT")) ? 0 : 1;
        o.user             = st;
        o.on_event         = es_on_ws_event;
        o.on_binary        = es_on_ws_binary;
        o.on_text          = es_on_ws_text;
        o.hdr_call_id      = st->corr;
        o.hdr_channel_uuid = switch_core_session_get_uuid(session);
        o.hdr_correlation  = st->corr;
        {   /* opaque caller context -> X-Earshot-Meta. Truncate at any CR/LF (so a value sourced
             * from caller-influenced data can't inject extra handshake headers) and cap the length
             * (an oversized header would overflow the lws handshake buffer and abort the connect). */
            const char *m = switch_channel_get_variable(channel, "EARSHOT_META");
            if (m && *m) {
                char *ms = switch_core_session_strdup(session, m), *c;
                for (c = ms; *c; c++) if (*c == '\r' || *c == '\n') { *c = '\0'; break; }
                if (c - ms > ES_META_MAX) {
                    ms[ES_META_MAX] = '\0';
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                        "earshot: EARSHOT_META truncated to %d bytes for the handshake header\n", ES_META_MAX);
                }
                o.hdr_meta = ms;
            }
        }
        o.hdr_extra_name   = es_proto_extra_header_name(st->proto);   /* optional per-proto header (currently none) */
        o.hdr_extra_value  = es_proto_extra_header_value(st->proto);
        o.subprotocol      = es_proto_subprotocol(st->proto);         /* e.g. openai -> "realtime" */
        o.insecure         = switch_true(switch_channel_get_variable(channel, "EARSHOT_TLS_NO_HOSTNAME_CHECK"));
        st->ws = es_ws_create(&o);
        if (st->ws) es_proto_set_ws(st->proto_ctx, st->ws);   /* enable receive-path pong (elevenlabs) */
        if (!st->ws || es_ws_start(st->ws) != 0) {
            stream->write_function(stream, "-ERR could not open websocket to %s\n", st->url);
            if (st->ws) { es_ws_destroy(st->ws); st->ws = NULL; }
            es_proto_destroy(st->proto_ctx); st->proto_ctx = NULL;
            if (st->play_buf) switch_buffer_destroy(&st->play_buf);
            return SWITCH_STATUS_FALSE;
        }
    }

    if (switch_core_media_bug_add(session, es_privkey(session, st->id), NULL, es_media_bug_cb, st, 0,
                                  st->bug_flags, &st->bug) != SWITCH_STATUS_SUCCESS) {
        stream->write_function(stream, "-ERR could not attach media bug\n");
        es_ws_destroy(st->ws); st->ws = NULL;
        es_proto_destroy(st->proto_ctx); st->proto_ctx = NULL;
        if (st->play_buf) switch_buffer_destroy(&st->play_buf);
        return SWITCH_STATUS_FALSE;
    }
    switch_channel_set_private(channel, es_privkey(session, st->id), st);
    /* register the caller-DTMF hook once per channel — on the default (unnamed) stream */
    if (!st->id[0]) { st->owns_dtmf_hook = SWITCH_TRUE; switch_core_event_hook_add_recv_dtmf(session, es_dtmf_hook); }
    es_fire_event(session, EARSHOT_EVENT_CONNECTED, "url", st->url);
    stream->write_function(stream, "+OK stream '%s' started\n", st->id[0] ? st->id : "default");
    return SWITCH_STATUS_SUCCESS;
}

static switch_status_t es_simple(switch_core_session_t *session, const char *verb, const char *id,
                                 switch_stream_handle_t *stream)
{
    switch_channel_t *channel = switch_core_session_get_channel(session);
    es_stream_t *st = (es_stream_t *) switch_channel_get_private(channel, es_privkey(session, id));
    if (!st) { stream->write_function(stream, "-ERR no active earshot stream '%s'\n", id && id[0] ? id : "default"); return SWITCH_STATUS_FALSE; }

    if (!strcasecmp(verb, "stop")) {
        switch_core_media_bug_remove(session, &st->bug);   /* triggers CLOSE -> teardown (clears private) */
    } else if (!strcasecmp(verb, "pause"))  { st->ready = SWITCH_FALSE; /* hold playback via the ready-gate */ }
    else if (!strcasecmp(verb, "resume")) { st->ready = SWITCH_TRUE; /* open ready-gate */ }
    else if (!strcasecmp(verb, "flush"))  {   /* barge-in: drop any queued agent audio now */
        if (st->play_buf) { switch_mutex_lock(st->play_mutex); switch_buffer_zero(st->play_buf); switch_mutex_unlock(st->play_mutex); }
    }
    else if (!strcasecmp(verb, "status")) {
        stream->write_function(stream,
            "{\"id\":\"%s\",\"proto\":\"%s\",\"corr\":\"%s\",\"ws_connected\":%d,\"ready\":%d,"
            "\"rx_frames\":%llu,\"play_buffered\":%u}\n",
            st->id[0] ? st->id : "default", es_proto_name(st->proto), st->corr,
            st->ws ? es_ws_connected(st->ws) : 0, st->ready,
            (unsigned long long) st->rx_frames,
            st->play_buf ? (unsigned) switch_buffer_inuse(st->play_buf) : 0);
        return SWITCH_STATUS_SUCCESS;
    }
    else if (!strcasecmp(verb, "metrics")) {
        es_ws_stats_t ws;
        es_ws_get_stats(st->ws, &ws);
        es_fire_metrics(session, st);      /* also emit as an event */
        stream->write_function(stream,
            "{\"proto\":\"%s\",\"corr\":\"%s\",\"tx_frames\":%llu,\"tx_bytes\":%llu,"
            "\"rx_frames\":%llu,\"rx_bytes\":%llu,\"play_drops\":%llu,\"commands\":%llu,"
            "\"speech_starts\":%llu,\"barges\":%llu,\"talking\":%d,\"dtmf\":%llu,\"masking\":%d,\"play_buffered\":%u,"
            "\"first_audio_ms\":%llu,\"response_ms\":%llu,\"response_ms_max\":%llu,\"turns\":%llu,"
            "\"ws_connected\":%d,\"ws_reconnects\":%u,\"ws_queue_drops\":%llu,\"ws_rtt_ms\":%ld}\n",
            es_proto_name(st->proto), st->corr,
            (unsigned long long) st->tx_frames, (unsigned long long) st->tx_bytes,
            (unsigned long long) st->rx_frames, (unsigned long long) st->rx_bytes,
            (unsigned long long) st->play_drops, (unsigned long long) st->commands,
            (unsigned long long) st->speech_starts, (unsigned long long) st->barges, st->talking,
            (unsigned long long) st->dtmf_count, st->masking,
            st->play_buf ? (unsigned) switch_buffer_inuse(st->play_buf) : 0,
            (unsigned long long) st->first_audio_ms, (unsigned long long) st->resp_ms_last,
            (unsigned long long) st->resp_ms_max, (unsigned long long) st->turns,
            ws.connected, ws.reconnects, (unsigned long long) ws.queue_drops, ws.rtt_ms);
        return SWITCH_STATUS_SUCCESS;
    }

    stream->write_function(stream, "+OK %s\n", verb);
    return SWITCH_STATUS_SUCCESS;
}

/* --------------------------------------------------------------------------
 * API + APP entry points
 * -------------------------------------------------------------------------- */
#define ES_MAX_ARGV 16

/* Return the substring of `s` after skipping `n` space-delimited tokens (for
 * verbs whose payload is free text, e.g. `send <json>`). Never returns NULL. */
static const char *es_after_tokens(const char *s, int n)
{
    while (n-- > 0 && s && *s) {
        while (*s == ' ') s++;
        while (*s && *s != ' ') s++;
    }
    while (s && *s == ' ') s++;
    return s ? s : "";
}

/* `send <text>`: write raw text to the agent socket. */
static switch_status_t es_send_text_cmd(switch_core_session_t *session, const char *id, const char *text,
                                        switch_stream_handle_t *stream)
{
    switch_channel_t *channel = switch_core_session_get_channel(session);
    es_stream_t *st = (es_stream_t *) switch_channel_get_private(channel, es_privkey(session, id));
    if (!st || !st->ws) { stream->write_function(stream, "-ERR no active earshot stream\n"); return SWITCH_STATUS_FALSE; }
    if (zstr(text))     { stream->write_function(stream, "-ERR send needs text\n"); return SWITCH_STATUS_FALSE; }
    es_ws_send_text(st->ws, text, strlen(text));
    stream->write_function(stream, "+OK send\n");
    return SWITCH_STATUS_SUCCESS;
}

/* `mask on|off`: toggle the PCI masking window (operator control, always allowed). */
static switch_status_t es_set_mask(switch_core_session_t *session, const char *id, switch_bool_t on,
                                   switch_stream_handle_t *stream)
{
    switch_channel_t *channel = switch_core_session_get_channel(session);
    es_stream_t *st = (es_stream_t *) switch_channel_get_private(channel, es_privkey(session, id));
    if (!st) { stream->write_function(stream, "-ERR no active earshot stream\n"); return SWITCH_STATUS_FALSE; }
    st->masking = on;
    switch_channel_set_variable(channel, "earshot_masking", on ? "true" : "false");
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "earshot: masking %s\n", on ? "ON" : "OFF");
    stream->write_function(stream, "+OK mask %s\n", on ? "on" : "off");
    return SWITCH_STATUS_SUCCESS;
}

/*
 * Compatibility shim for amigniter/mod_audio_stream's positional syntax:
 *   uuid_audio_stream <uuid> start <wss-url> <mix-type> <rate> [metadata]
 *   uuid_audio_stream <uuid> stop|pause|resume|graceful-shutdown
 *   uuid_audio_stream <uuid> send_text <text>
 * Translated to Earshot's named form (native proto, L16 at the requested rate,
 * which the adapter resamples to the channel rate). `raw` is the untokenized
 * command line, used to recover free-text `send_text` payloads.
 */
static switch_status_t es_compat(switch_core_session_t *session, int argc, char **argv,
                                 const char *raw, switch_stream_handle_t *stream)
{
    const char *verb = argc > 1 ? argv[1] : "";

    if (!strcasecmp(verb, "start")) {
        char *nargv[ES_MAX_ARGV] = { 0 };
        const char *url  = argc > 2 ? argv[2] : "";
        const char *rate = argc > 4 ? argv[4] : "8k";   /* mod_audio_stream <mix> (argv[3]) has no earshot equivalent */
        int hz = atoi(rate);
        int n = 0;
        if (strchr(rate, 'k') || strchr(rate, 'K')) hz *= 1000;
        if (hz <= 0) hz = 8000;
        nargv[n++] = argv[0];
        nargv[n++] = (char *) "start";
        nargv[n++] = (char *) url;
        nargv[n++] = (char *) "proto=native";
        nargv[n++] = (char *) "codec=l16";
        nargv[n++] = switch_core_session_sprintf(session, "rate=%d", hz);
        return es_start(session, n, nargv, stream);
    }
    if (!strcasecmp(verb, "send_text"))
        return es_send_text_cmd(session, "", es_after_tokens(raw, 2), stream);
    if (!strcasecmp(verb, "graceful-shutdown"))
        return es_simple(session, "stop", "", stream);
    return es_simple(session, verb, "", stream);   /* stop | pause | resume */
}

SWITCH_STANDARD_API(earshot_api_function)
{
    char *mycmd = NULL, *argv[ES_MAX_ARGV] = {0};
    int argc;
    switch_core_session_t *target = NULL;   /* the session named by argv[0] (macro owns `session`) */
    switch_status_t status = SWITCH_STATUS_FALSE;

    if (zstr(cmd) || !(mycmd = strdup(cmd))) { stream->write_function(stream, "-ERR usage: %s\n", EARSHOT_SYNTAX); return SWITCH_STATUS_SUCCESS; }
    argc = switch_separate_string(mycmd, ' ', argv, ES_MAX_ARGV);
    if (argc < 2) { stream->write_function(stream, "-ERR usage: %s\n", EARSHOT_SYNTAX); goto done; }

    if (!(target = switch_core_session_locate(argv[0]))) {
        stream->write_function(stream, "-ERR no such session %s\n", argv[0]);
        goto done;
    }

    if (!strcasecmp(argv[1], "start")) {
        status = es_start(target, argc, argv, stream);          /* start reads id= from options */
    } else {                                                    /* other verbs: optional id= after the verb */
        const char *id = "";
        int t = 2;
        if (argc > 2 && !strncasecmp(argv[2], "id=", 3)) { id = argv[2] + 3; t = 3; }
        if (!strcasecmp(argv[1], "send"))      status = es_send_text_cmd(target, id, es_after_tokens(cmd, t), stream);
        else if (!strcasecmp(argv[1], "mask")) status = es_set_mask(target, id, (switch_bool_t)(argc > t && switch_true(argv[t])), stream);
        else                                   status = es_simple(target, argv[1], id, stream);
    }

    switch_core_session_rwunlock(target);
done:
    (void) status; (void) session;   /* macro's `session` param is unused here */
    switch_safe_free(mycmd);
    return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_APP(earshot_app_function)
{
    /* Dialplan form: <action application="earshot" data="start ws://... codec=pcmu"/>
     * Operates on the current session: prepend its uuid and reuse the command path. */
    switch_stream_handle_t stream = { 0 };
    char *argv[ES_MAX_ARGV] = { 0 };
    char *full;
    int argc;

    SWITCH_STANDARD_STREAM(stream);
    full = switch_core_session_sprintf(session, "%s %s",
               switch_core_session_get_uuid(session), zstr(data) ? "" : data);
    argc = switch_separate_string(full, ' ', argv, ES_MAX_ARGV);
    if (argc >= 2 && !strcasecmp(argv[1], "start")) {
        es_start(session, argc, argv, &stream);
    } else if (argc >= 2) {
        const char *id = "";
        int d = 1;                                              /* tokens in `data` before free text */
        if (argc > 2 && !strncasecmp(argv[2], "id=", 3)) { id = argv[2] + 3; d = 2; }
        if (!strcasecmp(argv[1], "send"))      es_send_text_cmd(session, id, es_after_tokens(data, d), &stream);
        else if (!strcasecmp(argv[1], "mask")) es_set_mask(session, id, (switch_bool_t)(argc > (d + 1) && switch_true(argv[d + 1])), &stream);
        else                                   es_simple(session, argv[1], id, &stream);
    }
    switch_safe_free(stream.data);
}

/* ---- mod_audio_stream compatibility entry points ----------------------- */
SWITCH_STANDARD_API(uas_api_function)   /* uuid_audio_stream <uuid> <verb> ... */
{
    char *mycmd = NULL, *argv[ES_MAX_ARGV] = { 0 };
    int argc;
    switch_core_session_t *target = NULL;

    if (zstr(cmd) || !(mycmd = strdup(cmd))) { stream->write_function(stream, "-ERR usage: uuid_audio_stream <uuid> start <url> <mix> <rate>\n"); return SWITCH_STATUS_SUCCESS; }
    argc = switch_separate_string(mycmd, ' ', argv, ES_MAX_ARGV);
    if (argc < 2)                        { stream->write_function(stream, "-ERR usage: uuid_audio_stream <uuid> start <url> <mix> <rate>\n"); goto done; }
    if (!(target = switch_core_session_locate(argv[0]))) { stream->write_function(stream, "-ERR no such session %s\n", argv[0]); goto done; }
    es_compat(target, argc, argv, cmd, stream);
    switch_core_session_rwunlock(target);
done:
    (void) session;
    switch_safe_free(mycmd);
    return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_APP(audio_stream_app_function)   /* <action application="audio_stream" data="start <url> <mix> <rate>"/> */
{
    switch_stream_handle_t stream = { 0 };
    char *argv[ES_MAX_ARGV] = { 0 };
    char *full;
    int argc;

    SWITCH_STANDARD_STREAM(stream);
    full = switch_core_session_sprintf(session, "%s %s",
               switch_core_session_get_uuid(session), zstr(data) ? "" : data);
    argc = switch_separate_string(full, ' ', argv, ES_MAX_ARGV);
    if (argc >= 2) es_compat(session, argc, argv, full, &stream);
    switch_safe_free(stream.data);
}

/* --------------------------------------------------------------------------
 * Module lifecycle
 * -------------------------------------------------------------------------- */
SWITCH_MODULE_LOAD_FUNCTION(mod_earshot_load)
{
    switch_api_interface_t *api_interface;
    switch_application_interface_t *app_interface;

    *module_interface = switch_loadable_module_create_module_interface(pool, modname);

    /* Build the shared WS service-thread pool NOW, not lazily on the first call:
     * lazy init ran inside that call's session thread and cost it ~1s of caller
     * audio while the pool contexts were created (measured: first call after load
     * had first_audio_ms ~1s late and ~50 tx frames short). */
    if (es_ws_global_init() != 0) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mod_earshot: ws pool init failed\n");
        return SWITCH_STATUS_FALSE;
    }

    /* reserve the custom event subclasses so subclass-filtered subscribers receive them */
    switch_event_reserve_subclass(EARSHOT_EVENT_CONNECTED);
    switch_event_reserve_subclass(EARSHOT_EVENT_DISCONNECTED);
    switch_event_reserve_subclass(EARSHOT_EVENT_ERROR);
    switch_event_reserve_subclass(EARSHOT_EVENT_METRICS);
    switch_event_reserve_subclass(EARSHOT_EVENT_READY);
    switch_event_reserve_subclass(EARSHOT_EVENT_COMMAND);
    switch_event_reserve_subclass(EARSHOT_EVENT_SPEECH_START);
    switch_event_reserve_subclass(EARSHOT_EVENT_SPEECH_STOP);
    switch_event_reserve_subclass(EARSHOT_EVENT_DTMF);

    SWITCH_ADD_API(api_interface, "earshot", "Earshot audio-stream control", earshot_api_function, EARSHOT_SYNTAX);
    SWITCH_ADD_APP(app_interface, "earshot", "Earshot audio stream", "Stream channel audio to an AI agent over WebSocket",
                   earshot_app_function, EARSHOT_SYNTAX, SAF_NONE);

    /* drop-in compatibility with amigniter/mod_audio_stream's positional syntax.
     * Only register if those names are free — registering a duplicate collides with a
     * loaded mod_audio_stream and corrupts FreeSWITCH's interface tables (crash on unload). */
    {
        switch_application_interface_t *ex_app = switch_loadable_module_get_application_interface("audio_stream");
        switch_api_interface_t         *ex_api = switch_loadable_module_get_api_interface("uuid_audio_stream");
        if (ex_app || ex_api) {
            if (ex_app) UNPROTECT_INTERFACE(ex_app);
            if (ex_api) UNPROTECT_INTERFACE(ex_api);
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                "mod_earshot: audio_stream/uuid_audio_stream already registered (mod_audio_stream loaded?) "
                "- skipping the compat shim; unload mod_audio_stream first to enable it.\n");
        } else {
            SWITCH_ADD_API(api_interface, "uuid_audio_stream", "mod_audio_stream compat (uuid_audio_stream)",
                           uas_api_function, "<uuid> start <wss-url> <mix-type> <rate> [meta] | stop | pause | resume | send_text <text>");
            SWITCH_ADD_APP(app_interface, "audio_stream", "mod_audio_stream compat (audio_stream)",
                           "Stream channel audio to a WebSocket (mod_audio_stream-compatible)",
                           audio_stream_app_function, "start <wss-url> <mix-type> <rate> [meta]", SAF_NONE);
        }
    }

    switch_console_set_complete("add earshot ::console::list_uuid start");
    switch_console_set_complete("add earshot ::console::list_uuid stop");

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_earshot loaded\n");
    return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_earshot_shutdown)
{
    es_ws_global_shutdown();   /* join the shared WS service-thread pool before we're unmapped */
    switch_event_free_subclass(EARSHOT_EVENT_CONNECTED);
    switch_event_free_subclass(EARSHOT_EVENT_DISCONNECTED);
    switch_event_free_subclass(EARSHOT_EVENT_ERROR);
    switch_event_free_subclass(EARSHOT_EVENT_METRICS);
    switch_event_free_subclass(EARSHOT_EVENT_READY);
    switch_event_free_subclass(EARSHOT_EVENT_COMMAND);
    switch_event_free_subclass(EARSHOT_EVENT_SPEECH_START);
    switch_event_free_subclass(EARSHOT_EVENT_SPEECH_STOP);
    switch_event_free_subclass(EARSHOT_EVENT_DTMF);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_earshot shutdown\n");
    return SWITCH_STATUS_SUCCESS;
}
