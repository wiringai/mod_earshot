# Earshot Roadmap

Goal: the reliable, protocol-flexible FreeSWITCH↔AI audio bridge. Tiers are ordered by
adoption leverage, not difficulty; ☑ items ship in 0.1.0, ☐/◐ are what's next.

Legend: ☐ todo · ◐ in progress · ☑ done · ✗ considered, rejected

## Tier 0 — Foundation (make it real)

- ☑ Module skeleton: load/unload, `earshot` app+API, media-bug tap, session locate
- ☑ WebSocket transport (libwebsockets), ws + wss (`es_ws.c`) — **VALIDATED**
- ☑ **FULL-DUPLEX VALIDATED on a real inbound SIP call (sipp → FreeSWITCH):**
  - one answered caller-facing leg, stereo-recorded (`record_session`, 5.8 s), both directions non-silent
    at once — this is the real proof, not a synthetic loopback:
  - send (caller→agent): sipp streamed a G.711 PCAP in; recorded READ max=16896 / rms=2007, and the
    agent received it over the WS (recv_max=16764, 304 frames) — earshot's read tap + encode + WS all work.
  - playback (agent→caller): `WRITE_REPLACE` injected the agent's tone into the caller path;
    recorded WRITE max=7932 / rms=5626.
  - `X-Call-ID` correlation header arrived at the agent (`Call-ID=1-123846@…`); FS never crashed.
  - harness: a dedicated `earshot-test` sofia profile on :5090 (own context, `auth-calls=false`) so the
    call runs fully isolated from any other routing on the box.
  - `earshot status` reports live stats: `{ws_connected, ready, rx_frames, play_buffered}`.
- ☑ (bugfix) never request `SMBF_WRITE_REPLACE` without setting a replace frame each callback — NULL-derefs FS
- ☑ playback buffer bounded (sized to hold a full agent response, which realtime vendors burst
  faster than realtime) so it plays a whole reply in order yet can't grow unbounded
- Note: an earlier synthetic loopback could only carry one direction per leg (b-leg = read/send,
  a-leg = written/playback); a real SIP caller leg carries both, which is what the sipp run above exercises.
- ☑ **Codec layer** — L16 + G.711 μ-law/a-law (`es_codec.c`), **unit-tested** (`test/test_codec.c`)
- ☑ **Backpressure** — the bounded, drop-oldest outbound queue lives in the WS transport (`es_ws.c`);
  buffers a brief reconnect, drops on a long outage. (A standalone `es_queue` utility was removed as
  unused — the transport owns its own queue.)
- ◐ Named-argument parser (`key=value`) + channel-variable fallbacks (in `mod_earshot.c`)
- ◐ `earshot::` events (connected/disconnected/error/message/playback) — plumbing in place
- ☑ **CMake** (module + CTest for the portable core) · ◐ CI matrix (FS 1.10/1.11 · Debian/Ubuntu)
- ☐ Prebuilt `.deb` + Docker build image as CI release artifacts
- ☑ Test harness: mock WS agent (`test/mock_agent.py`) + full audio round-trip validated on real SIP calls

## Tier 1 — Reliability (why people switch)

- ☑ Reconnect with exponential backoff **+ jitter** (`es_ws.c`); the outbound queue is now
  **bounded (drop-oldest)** — buffers audio across a brief reconnect, drops on a long outage so
  it can't grow unbounded or replay stale audio.
- ◐ Backpressure policy — queue done + unit-tested; ☑ outbound WS queue bounded; ☐ wire `jitter=drop|block` into the media path
- ☑ **Ready gate**: `ready=firstframe|connect|manual` (default firstframe) holds playback until the
  agent's first audio frame; fires `earshot::ready` + sets `earshot_ready=true` so the dialplan can
  design out "answered into silence".
- ☑ Native **G.711 μ-law/a-law** — codec done + unit-tested.
- ☑ **Clean resampling** (FS bundled speex resampler, no new dep): the adapter resamples between the
  channel rate and the wire rate when they differ, so 16 kHz/24 kHz agents work (e.g. OpenAI pcm16 @ 24k).
  **VALIDATED on real calls**: 8k↔16k and 8k↔24k both directions, amplitude preserved (WRITE max 7998 ≈
  source 8000, rms 5631 ≈ clean-tone 5657), byte counts exactly 4×/6× — no distortion.
  Wire codec/rate via `codec=l16 rate=16000|24000`; default stays g711 8k.
- ☐ Graceful teardown races (stop while playing / during reconnect) — no leaks, no stuck channels
- ◐ Structured logging with the channel UUID + Call-ID (mark/ready lifecycle logs in place)

## Tier 2 — The adoption unlock

- ☑ **Protocol adapters** (`es_proto.c`): `native`, `twilio`, `openai`, `deepgram`, `vapi`, `elevenlabs`,
  `gemini`, `pipecat`, `assemblyai`, `cartesia` — an agent written for any of these works against Earshot unmodified.
  - **`openai`, `deepgram`, `vapi`, `assemblyai`, `cartesia`, `elevenlabs`, and `gemini` are live-validated against the real vendors** on real SIP calls;
    `native`, `twilio`, and `pipecat` are validated by full-duplex
    round-trip against a local echo agent. Every adapter has now met its real vendor
    (see the README status table). Framing detail per adapter:
    - `twilio`: `connected`+`start` framing, base64 μ-law media both ways, **mark echo-on-drain**.
    - `openai`: `session.update` handshake (g711_ulaw + server_vad), append out / delta in,
      `realtime` subprotocol, `speech_started` = barge-in.
    - `deepgram`: `Settings` handshake (mulaw/linear16 @ 8k), raw **binary** audio, `UserStartedSpeaking`.
    - `vapi`: WebSocket transport, raw **binary** audio both ways (pcm_s16le/mulaw set at REST
      `POST /call`), JSON control; `speech-update`(role user) / `user-interrupted` = barge-in. The
      per-call `websocketCallUrl` is the credential (no handshake/auth header). Live-validated
      end-to-end (bidirectional audio + barge on a real assistant).
    - `elevenlabs`: convai — **`convai` subprotocol** (required), `user_audio_chunk` out / `{type:audio}` in,
      **auto `ping`→`pong` keepalive**, `interruption` = barge-in. µ-law default, or `codec=l16 rate=16000`
      to match a `pcm_16000` agent. **Live-validated** end-to-end (bidirectional audio on a real convai agent).
    - `gemini`: Live API — `setup`, `realtimeInput.audio` out / `serverContent` in (Gemini sends **JSON as
      binary frames**, routed through the text parser), **asymmetric 16 kHz-in / 24 kHz-out resampled**
      transparently, `serverContent.interrupted` = barge-in. Model via `EARSHOT_SESSION_CONFIG` (names churn).
      **Live-validated** end-to-end (bidirectional audio on a real native-audio agent).
    - `pipecat`: binary **protobuf** `Frame{ audio: AudioRawFrame }` both ways (round-trip validated);
      wire codec is a pure-C unit `es_pb.c` with **unit tests** (`test/test_pb.c`, incl. adversarial input),
      `InterruptionFrame` = barge-in.
    - `assemblyai`: Universal-Streaming **STT** (transcription, no playback) — PCM16 out coalesced to
      ≥50 ms chunks (vendor rejects <50 ms), `transcript` JSON in → the new `earshot::transcript` event
      (`text`/`final`); `dir=in` fork, raw API key in `Authorization` via `EARSHOT_AUTH`. **Live-validated**
      end-to-end (accurate partial + final transcripts on a real call).
    - `cartesia`: ink-whisper **STT** (transcription, no playback) — PCM16 out (~100 ms chunks, shared
      coalescer), `{type:transcript}` in → `earshot::transcript`; `dir=in` fork, raw API key rides the URL
      as `access_token=` (no auth header). **Live-validated** end-to-end (accurate finals on a real call).
  - Session config (voice/model/keys/prompt) defaults to an audio-only handshake, or the caller supplies
    a full JSON via the `EARSHOT_SESSION_CONFIG` channel variable (sent verbatim).
- ☑ **Control channel** (`commands=true`, opt-in): the agent sends `{"type":"command","action":…}`
  and Earshot runs the whitelisted `uuid_*` API (transfer / hangup / send_dtmf / play / stop_play /
  record / setvar / hold / bridge / park), fires an `earshot::command` audit event, and echoes a
  `command_result`. Turns LLM tool calls into real telephony actions. **VALIDATED**: `setvar` executed
  mid-call (`uuid_getvar` confirmed the side effect) + `ok:true` echoed + event fired; refused with
  `error:disabled` when the gate is off. ☐ next: per-action scoping (`commands=transfer,hangup`).
- ☑ **Correlation-first handshake**: `X-Call-ID`/`X-Channel-UUID`/`X-Correlation-ID` headers on the
  WS handshake, and (Twilio) `earshot_call_id`+`earshot_channel_uuid` in `start.customParameters`.
- ☑ **Caller context + welcome greeting**: `EARSHOT_META` → `X-Earshot-Meta` handshake header (opaque
  per-call context to the agent at connect, any `proto`); `greeting=<file>` / `EARSHOT_GREETING` plays
  a fixed audio prompt into the channel the moment the ready gate opens, ahead of the agent's first
  words. (☐ dynamic/TTS greeting stays the agent's job or a pre-rendered file.)
- ☑ **Module-side VAD + turn events** (works with any agent): FS-native `switch_vad` on the caller
  audio emits `earshot::speech_started` / `earshot::speech_stopped` + sets `earshot_talking`, with
  opt-in **speech-triggered barge-in** (`vad_barge=on`, flush playback the instant the caller speaks)
  and agent turn notify (`vad_notify=on`). Tunables `vad_mode` / `vad_voice_ms` / `vad_silence_ms`.
  **VALIDATED**: both events fired on real caller audio, `earshot_talking` flipped, `speech_starts`
  counted, agent received the turn JSON. Gives endpointing to agents that do none of their own.
- ☑ **Barge-in policy engine**: `interruptible=none|dtmf|speech|any` (adds DTMF-triggered barge,
  suppressed during a masking window); `ignore_backchannel` / `sensitivity=low|medium|high` /
  `barge_min_ms` require sustained caller speech so short backchannels don't cut the agent;
  `barge_fade_ms` fades the playout to silence instead of a hard cut. `flush`, Twilio `clear`, OpenAI
  `speech_started`, Deepgram `UserStartedSpeaking`, and module VAD all drive it; `barges` counter in
  metrics. Back-compat: `vad_barge=on` = `interruptible=speech`. ☐ `earshot::playback` events
- ☑ **DTMF capture + PCI masking**: `recv_dtmf` hook → `earshot::dtmf` events + forward to the agent
  (`dtmf=on`); a masking window (`earshot <uuid> mask on|off`, or the control-channel `mask` action)
  **mutes caller→agent audio and redacts DTMF to the agent** during card entry while digits still reach
  FreeSWITCH for a secure collector. **VALIDATED on real RFC2833**: in-window digit suppressed from the
  agent (audit event still fired, redacted), audio froze to 0 frames while masked, both resumed after.
- ☐ Opus over WS (low-bitrate WAN links)
- ☑ App-level auth: full `Authorization` header (e.g. `Bearer <key>`, Deepgram `Token <key>`) via the
  `EARSHOT_AUTH` channel variable, sent verbatim on the WS handshake — validated live against OpenAI +
  Deepgram.
- ☑ **Box-level mTLS + custom CA** — **VALIDATED end-to-end on a real SIP call** (FreeSWITCH 1.11.1,
  libwebsockets 4.0.20): client cert presented + accepted, `EARSHOT_TLS_CA` enforced, live audio over
  the mutually-authenticated wss leg; also standalone-verified across all guard cases.
  `EARSHOT_TLS_CLIENT_CERT` / `EARSHOT_TLS_CLIENT_KEY` present a client certificate on every agent
  connection; `EARSHOT_TLS_CA` verifies the agent against a private CA — which **replaces the system
  trust store box-wide**, so public-CA endpoints (OpenAI/Deepgram) then fail unless they also chain to
  it. One client identity per box (lws binds client TLS material at context level, applied to every
  pooled context at init). (☐ per-stream *multi-identity* mTLS still needs context-partitioning.)
- ✗ **permessage-deflate** — considered and deliberately omitted (a non-goal, not a gap). WebSocket
  compression (RFC 7692) shrinks text/JSON well, which is why general-purpose clients like
  `mod_audio_stream` turn it on by default. It is the wrong fit for a real-time **voice** bridge, for
  four reasons:
  1. **Audio doesn't compress** — G.711 is already a codec and L16 is high-entropy PCM, so deflate
     saves ~0% on a payload where bandwidth was never the bottleneck.
  2. **It adds latency** — a compress/decompress step on every 20 ms frame, on the exact metric
     (time-to-first-audio, per-turn response) that is the product.
  3. **CPU per frame** — ~50 frames/sec/call of pointless compression lowers per-box concurrency.
  4. **Memory per connection** — deflate keeps a ~32 KB sliding window *per connection*, which at
     thousands of sessions undercuts the shared pool's ~1 MB/session efficiency.

  Net: all cost, no benefit for audio — so "on by default" (as upstream ships it) is a mis-default
  here, not an advantage. It would only ever be added **opt-in, off by default**, purely to satisfy an
  endpoint that requires or negotiates it.

## Tier 3 — Scale & observability

- ☑ **`earshot::metrics` events** — periodic (`metrics=<seconds>` option), final-on-close, and on-demand
  (`earshot <uuid> metrics` returns JSON): tx/rx frames+bytes, play-drops, play-buffered,
  ws-connected / ws-reconnects / ws-queue-drops, duration. Custom subclasses are reserved so ESL
  subscribers receive them. **VALIDATED**: 6 metrics events fired over one call + live API JSON;
  `earshot::connected` / `earshot::ready` / `earshot::metrics` all delivered over the event socket.
- ☑ **Latency KPIs** (the AI scoreboard): `first-audio-ms` (start → agent's first audio), per-turn
  `response-ms` / `response-ms-max` + `turns` (caller stops → agent responds, via the VAD), and
  `ws-rtt-ms` (a WebSocket ping/pong probe every 5 s, timed on the pong). **VALIDATED live**:
  first-audio ≈ 65 ms, ws-rtt = 1 ms (localhost), turn timer fires on each VAD turn-end.
  (☐ a Prometheus textfile exporter / OTel spans are downstream consumers of these events.)
- ☑ **Multi-stream fan-out** — many named streams per channel (`id=<name>`); `dir=in` read-only forks
  (transcription / supervisor / compliance) tap the caller audio alongside the bidirectional agent, and
  only the agent owns `WRITE_REPLACE` so forks never fight over playback. Every verb targets a stream by
  `id=`. **VALIDATED**: 3 streams on one channel all received caller audio; agent played back, forks
  didn't; per-stream stop worked. (☐ next: `track=both` caller+agent mix for the supervisor; mask-all-streams.)
- ☑ **Shared lws service thread / event loop (was the #1 concurrency ceiling).** DONE. All streams
  now share a small pool of `lws_context`s sized to CPU cores, each driven by one service thread with
  rate-limited housekeeping and per-frame writable arming (`EVENT_WAIT_CANCELLED`). Per session: ~0
  added WS threads and ~1 MB (vs ~1 thread + ~17 MB before). **VALIDATED** on an 8 GB box vs the old
  thread-per-stream build (same box, same load): 150 concurrent streams (RSS +139 MB vs +2.4 GB, ~17×
  less), 12-minute soak flat, mass-teardown + RST-storm no-crash, unload-under-load clean, dead-peer
  liveness detection, ThreadSanitizer-clean fuzz, and audio cadence/quality byte-equal to the old
  transport on a real SIP call. Reviewed via self + three adversarial peer passes.
- ☐ Per-stream **adaptive** jitter buffer (a fixed prebuffer was tried and reverted — its re-cushioning
  inserted audible artifacts; the win needs an adaptive, underrun-driven design); adaptive frame sizing
- ☐ Optional simultaneous recording fork
- ☑ **`uuid_audio_stream` / `audio_stream` compat shim** — mod_audio_stream's positional syntax
  (`start <url> <mix> <rate>`, stop/pause/resume/send_text) translated to Earshot (native L16 at the
  requested rate, resampled to the channel rate). Registers **only when those names are free**, so it
  never collides with a loaded mod_audio_stream (a duplicate registration corrupts FS's interface tables
  and crashes on unload). **VALIDATED**: after unloading mod_audio_stream, an unmodified
  `audio_stream data="start <url> mono 16k"` dialplan runs full-duplex against Earshot.

## Non-goals (for now)

- Not a media server or SFU (that's LiveKit's job) — Earshot is the FreeSWITCH-side tap.
- Not an agent framework — it feeds pipecat / your own WS server, doesn't replace them.

## Releases

- **0.1.0 — first public release.** Tiers 0–2 solid (full-duplex bridge, seven protocol
  adapters, control channel, VAD/barge-in, DTMF + PCI masking) plus the Tier 3 shared
  service-loop transport, metrics, latency KPIs, and multi-stream fan-out. `openai`,
  `deepgram`, and `native` are live-validated on real SIP calls; the remaining adapters
  are implemented and mock/echo-tested (see the README status table).
- **Next:** the ☐ items above, community-driven from the issue tracker — Opus, per-action
  command scoping, `track=both` supervisor mix, and live-validation of the Gemini and
  ElevenLabs adapters.
