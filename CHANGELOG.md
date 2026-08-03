# Changelog

All notable changes to Earshot (`mod_earshot`). Format follows
[Keep a Changelog](https://keepachangelog.com/); versions follow [SemVer](https://semver.org/).

## [Unreleased]

## [0.1.0] — 2026-08-01

First public pre-release. Every capability below is validated end-to-end on real SIP calls
(`sipp` + real audio) against emulated vendor servers on a test environment. Live-vendor
(OpenAI/Deepgram/…) interop still needs live keys.

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
- **Reliability** — auto-reconnect with jittered backoff; bounded outbound queue (drop-oldest).
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
