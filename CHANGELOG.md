# Changelog

All notable changes to Earshot (`mod_earshot`). Format follows
[Keep a Changelog](https://keepachangelog.com/); versions follow [SemVer](https://semver.org/).

## [Unreleased]

### Added
- **Box-level mutual TLS (mTLS) + custom CA** — `EARSHOT_TLS_CLIENT_CERT` / `EARSHOT_TLS_CLIENT_KEY`
  present a client certificate on every agent connection; `EARSHOT_TLS_CA` verifies the agent against
  a private CA instead of the system trust store (**box-wide** — public-CA endpoints then fail unless
  they also chain to it). One client identity per box (libwebsockets binds client TLS material at the
  context level; applied to every pooled context at init). Configured via process environment, read
  at module load; the client key must be an unencrypted PEM.

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
