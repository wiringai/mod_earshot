# Earshot Architecture

A sketch of the internals, so contributors (and future-you) know where things live.

## The shape

```
   FreeSWITCH channel
        │  media bug (READ_STREAM: caller audio · WRITE_REPLACE: agent audio)
        ▼
   ┌──────────────────────────────────────────────────────────────┐
   │  mod_earshot  (mod_earshot.c)                                  │
   │                                                                │
   │  media bug READ ─► resample(rs_out) ─► es_proto_send_audio ───►│──► WebSocket ──► agent
   │   (caller→agent)   (chan→wire)          (es_proto.c adapter)   │      (es_ws.c)
   │        │                                                       │
   │      VAD (switch_vad) ─► speech events / vad_barge             │
   │        │                                                       │
   │  media bug WRITE ◄─ play_buf ◄─ es_emit_audio ◄─ es_proto ◄────│◄── agent audio / control
   │   (agent→caller)    (ready-gate,    (resample rs_in)           │
   │                      flush = barge-in)                         │
   │                                                                │
   │  recv_dtmf hook ─► earshot::dtmf / mask       control channel ─┤─► uuid_* API (transfer/hangup/…)
   └──────────────────────────────────────────────────────────────┘
        │  events: earshot::connected / ready / speech_* / dtmf / command / metrics
        ▼
   ESL / controller
```

## Threads & lifetime

- **Media thread (FreeSWITCH):** the media-bug callback runs on FS's media path. It must be *fast and
  non-blocking* — it reads a frame, runs VAD, and hands audio to the adapter (which only enqueues on
  the WS send queue). No socket I/O here.
- **WS service thread (per stream, `es_ws.c`):** owns one libwebsockets connection. All network I/O
  lives here: drains the send queue on `WRITEABLE`, delivers inbound frames/lifecycle via callbacks,
  reconnects with jittered backoff, and runs the RTT ping/pong probe. One per active stream.
- **Lifetime:** each stream's state hangs off the channel private (`earshot` for the default stream,
  `earshot_<id>` for fan-out forks). `stop`, hangup, and socket close converge on **one** teardown path
  (the media-bug `CLOSE`) guarded so it can't double-free or race the WS thread's `pthread_join`.

## Key components

- **Media bug** — `switch_core_media_bug_add`; flags follow `dir` (`SMBF_READ_STREAM` and/or
  `SMBF_WRITE_REPLACE`). Only a `dir=both`/`out` stream requests `WRITE_REPLACE`, so read-only forks
  never fight over playback.
- **`es_proto` (adapter)** — the only protocol-aware layer. Translates channel audio ↔ the wire shape
  for all seven protocols, holds per-direction resamplers, and drives inbound audio/clear/mark/dtmf/
  command to a small **sink** the module provides. Everything upstream is protocol-neutral.
- **`es_codec`** — L16 (passthrough) + G.711 µ-law/a-law (unit-tested ITU/Sun tables).
- **`es_pb`** — a tiny, pure-C protobuf codec for the Pipecat `Frame` wire format (unit-tested,
  including adversarial input). No FreeSWITCH dependency.
- **Resampler** — FreeSWITCH's bundled speex resampler (`switch_resample`), created per direction only
  when the channel and wire rates differ (e.g. Gemini 8k↔16k send / 24k↔8k receive).
- **`es_ws` (transport)** — libwebsockets client: ws/wss, handshake headers (auth, correlation,
  `OpenAI-Beta`), a mutex-guarded outbound queue with a drop-oldest cap, reconnect-with-jitter, and a
  WS ping/pong RTT probe.
- **VAD** — `switch_vad` on the caller's read stream → `earshot::speech_started/stopped`, the ready-gate,
  and speech-triggered barge-in; also the clock for per-turn response latency.
- **Control channel** — inbound `{"type":"command",…}` maps to whitelisted `uuid_*` APIs via
  `switch_api_execute` on the WS thread (the event-socket pattern), audited via `earshot::command`.
- **DTMF + masking** — a session `recv_dtmf` hook (registered once, on the default stream) forwards
  digits to the agent, or, during a masking window, mutes caller audio and redacts DTMF (PCI).
- **Metrics** — counters (frames/bytes, drops, speech, dtmf, latency KPIs) emitted as `earshot::metrics`
  events (periodic / on-close / on-demand JSON).

## Correlation

On handshake (unless `corr=<id>` is given) Earshot injects `X-Call-ID`, `X-Channel-UUID`,
`X-Correlation-ID`, so the agent joins the two-key model (SIP Call-ID ↔ channel UUID) the rest of the
observability stack uses. Twilio also carries these in `start.customParameters`.

## Why these choices

- **No socket I/O on the media thread** is the single most important rule — a slow or dead agent
  degrades gracefully (queue drops) instead of stalling audio for everyone.
- **Adapters at the edge** keep the hot path (tap → VAD → resample → send) identical regardless of
  protocol, so a new agent protocol is a translation unit, not a fork of the core.
- **One teardown path** is the antidote to the "stuck channel / double free" bugs that plague media
  modules under reconnect + hangup races.
- **A pure-C portable core** (`es_codec`, `es_pb`) unit-tests without FreeSWITCH, so correctness is
  checked on every CI run regardless of FS headers.
