# Changelog

All notable changes to Earshot (`mod_earshot`). Format follows
[Keep a Changelog](https://keepachangelog.com/); versions follow [SemVer](https://semver.org/).

## [Unreleased]

### Fixed
- **Gemini adapter now works against current Gemini Live** (previously never worked). Gemini delivers
  its JSON — `setupComplete`, `serverContent` audio, interruptions — as **binary** WebSocket frames, which
  the adapter was dropping; they're now routed through the JSON parser (es_ws NUL-terminates the receive
  buffer for binary frames too — inert for raw-audio/protobuf consumers, which read by length). Also
  switched the outbound audio to the current `realtimeInput.audio` (the old `mediaChunks[]` is ignored by
  2.5+ models) and updated the default model (`gemini-2.0-flash-exp` is retired). **Live-validated**
  end-to-end (bidirectional audio on a real native-audio agent).
- **ElevenLabs adapter now works against real convai agents** (previously never connected). It now
  advertises the required **`convai`** WebSocket subprotocol (the server answers with it and rejects a
  mismatch), and it **respects `codec=`/`rate=`** instead of forcing µ-law — so `codec=l16 rate=16000`
  matches a `pcm_16000` agent while the µ-law default keeps existing telephony setups unchanged.
  **Live-validated** end-to-end (bidirectional audio on a real agent).

### Added
- **Cartesia adapter** (`proto=cartesia`) — Cartesia ink-whisper **STT** (transcription): PCM16 audio out
  (~100 ms chunks, shared coalescer with `assemblyai`), `{"type":"transcript",…}` in → `earshot::transcript`.
  Audio-in only (`dir=in` fork, no playback); the raw API key rides the URL as `access_token=` (with
  `model`/`encoding`/`sample_rate`/`cartesia_version`) — no auth header needed. **Live-validated** end-to-end.
- **AssemblyAI adapter** (`proto=assemblyai`) — AssemblyAI Universal-Streaming **STT** (transcription):
  PCM16 audio out (coalesced to ≥50 ms chunks — the vendor rejects smaller), `transcript` JSON in →
  a new **`earshot::transcript`** event (`text`, `final`, `corr`, `stream-id`). Audio-in only (use
  `dir=in` as a read-only fork; no playback); auth is the raw API key in the `Authorization` header via
  `EARSHOT_AUTH`. The transcript event is lightweight (uuid + corr, no full channel-variable copy) since
  partials fire many times per second. **Live-validated** end-to-end (accurate partial + final
  transcripts on a real call).
- **Vapi adapter** (`proto=vapi`) — Vapi's WebSocket transport: raw binary audio both ways
  (`pcm_s16le`/`mulaw`, set at REST `POST /call`), JSON control mapped to barge-in
  (`speech-update` role=user / `user-interrupted`). The per-call `websocketCallUrl` is the
  credential — no handshake, auth header, or subprotocol. **Live-validated** end-to-end on a real
  Vapi assistant (bidirectional audio + barge-in). Match `codec=`/`rate=` to the call's `audioFormat`.
- **Caller context + welcome greeting** — `EARSHOT_META` passes per-call context (customer id, tier,
  reason, …) to the agent verbatim as the `X-Earshot-Meta` handshake header, readable by any agent
  framework at connect regardless of `proto`. `greeting=<file>` (or `EARSHOT_GREETING`) plays a fixed
  audio prompt into the channel the moment the ready gate opens — ahead of the agent's first words,
  with no model round-trip — loaded once at `start` and resampled to the channel rate; pairs with
  `ready=connect` so it leads, and barge-in cuts it like any other playout.
- **Box-level mutual TLS (mTLS) + custom CA** — `EARSHOT_TLS_CLIENT_CERT` / `EARSHOT_TLS_CLIENT_KEY`
  present a client certificate on every agent connection; `EARSHOT_TLS_CA` verifies the agent against
  a private CA instead of the system trust store (**box-wide** — public-CA endpoints then fail unless
  they also chain to it). One client identity per box (libwebsockets binds client TLS material at the
  context level; applied to every pooled context at init). Configured via process environment, read
  at module load; the client key must be an unencrypted PEM. Validated end-to-end on a real SIP call
  (FreeSWITCH 1.11.1, libwebsockets 4.0.20) with live audio over the mutually-authenticated wss leg.
- **Barge-in policy engine** — `interruptible=none|dtmf|speech|any` (adds DTMF-triggered barge,
  suppressed during a PCI masking window); `ignore_backchannel` / `sensitivity=low|medium|high` /
  `barge_min_ms` require sustained caller speech so short backchannels don't cut the agent;
  `barge_fade_ms` fades the playout to silence over a ramp instead of a hard cut. Back-compat:
  `vad_barge=on` = `interruptible=speech`. New `barges` counter in `earshot::metrics`.

## [0.1.0] — 2026-08-05

First public release. The transport, full-duplex audio path, and playout are validated
end-to-end on real SIP calls with real audio — live OpenAI Realtime and Deepgram Voice
Agent sessions (the Deepgram one from a hardware SIP phone), plus a shared-pool
concurrency + soak load test. Adapter coverage is deliberately stated per vendor, not
blanket: `openai`, `deepgram`, and `native` are live-validated on a real call; `pipecat`
against Pipecat's official protobuf schema; `twilio` via a WebSocket echo round-trip;
`elevenlabs` and `gemini` are implemented but **not yet live-validated** (see the adapter
status table in the README).

### Added

- **Full-duplex audio bridge** — caller→agent (`READ_STREAM`) and agent→caller (`WRITE_REPLACE`)
  on one leg, with real, validated playback.
- **Seven protocol adapters** (`proto=`): `native`, `twilio` (Media Streams), `openai` (Realtime),
  `deepgram` (Voice Agent), `elevenlabs` (Conversational AI, incl. `ping`→`pong`), `gemini` (Live,
  16k-in/24k-out), `pipecat` (protobuf frames).
- **Codecs + resampling** — G.711 µ-law/a-law + L16; transparent per-direction resampling (8/16/24 kHz).
- **Module-side VAD + turn events** — `earshot::speech_started/stopped`, ready-gate, speech-triggered
  barge-in (`vad_barge`), agent turn-notify (`vad_notify`).
- **Barge-in** — `flush`, protocol `clear`/`interruption`/`speech_started`, and VAD-driven.
- **Control channel** (opt-in `commands=true`) — agent-driven `transfer`/`hangup`/`send_dtmf`/`play`/
  `record`/`setvar`/`hold`/`bridge`/`park`/`mask`, whitelisted to `uuid_*` APIs, audited via
  `earshot::command`.
- **DTMF capture + PCI masking** — `earshot::dtmf` + forward to agent; a masking window mutes caller
  audio and redacts DTMF to the agent during card entry while digits still reach a secure collector.
- **Multi-stream fan-out** — many named streams per channel (`id=`); `dir=in` read-only forks
  (transcription / supervisor); per-stream lifecycle and metrics.
- **Latency KPIs** — `first_audio_ms`, per-turn `response_ms`/`response_ms_max`+`turns`, and
  `ws_rtt_ms` (WebSocket ping/pong probe).
- **Scales with sessions, not threads** — all streams share a small pool of libwebsockets
  service contexts (sized to CPU cores), not a thread + context per stream. Adds ~0 WebSocket
  threads and ~1 MB per session (vs ~1 dedicated thread + ~17 MB before), so a small box holds
  thousands of sessions where thread-per-stream hit a RAM wall near ~420. Validated: 150
  concurrent streams, a 12-minute soak with flat memory/fds/threads, mass-teardown + RST-storm
  with no crash, module unload under live load, and a ThreadSanitizer-clean fuzz run.
- **Reliability** — auto-reconnect with jittered backoff (reset only after a link proves stable),
  a **liveness timeout** that drops a hung/half-open peer and reconnects, and a bounded outbound
  queue (drop-oldest). Agent playout is buffered to hold a full burst-delivered response in order.
- **Observability** — `earshot::metrics` events (periodic / on-close / on-demand JSON); reserved
  custom subclasses; correlation by SIP Call-ID + channel UUID on the handshake.
- **mod_audio_stream compat** — `uuid_audio_stream` / `audio_stream` positional syntax, registered
  only when those names are free.
- **Packaging** — CMake build; unit tests for the portable core (`es_codec`, `es_pb`,
  incl. adversarial protobuf input); CPack `.deb`; a reproducible `Dockerfile`; CI that gates on the
  FreeSWITCH-free unit tests.

### Security

- The Pipecat protobuf parser bounds-checks against remaining bytes (no pointer-arithmetic overflow),
  found and fixed in peer review; covered by `test/test_pb.c`.
- The control channel is **off by default**, opt-in per stream, and whitelisted to call-control APIs.

[Unreleased]: https://github.com/wiringai/mod_earshot/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/wiringai/mod_earshot/releases/tag/v0.1.0
