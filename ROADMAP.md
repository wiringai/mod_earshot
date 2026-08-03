# Earshot Roadmap

Goal: the reliable, protocol-flexible FreeSWITCH↔AI audio bridge — matured privately,
then published. Tiers are ordered by adoption leverage, not difficulty.

Legend: ☐ todo · ◐ in progress · ☑ done

## Tier 0 — Foundation (make it real)

- ☐ Module skeleton: load/unload, `earshot` app+API, media-bug tap, session locate
- ☑ WebSocket transport (libwebsockets), ws + wss (`es_ws.c`) — **VALIDATED on a test environment**
- ☑ **FULL-DUPLEX VALIDATED on a real inbound SIP call (sipp → FreeSWITCH, test environment):**
  - one answered caller-facing leg, stereo-recorded (`record_session`, 5.8 s), both directions non-silent
    at once — this is the real proof, not a synthetic loopback:
  - send (caller→agent): sipp streamed a G.711 PCAP in; recorded READ max=16896 / rms=2007, and the
    agent received it over the WS (recv_max=16764, 304 frames) — earshot's read tap + encode + WS all work.
  - playback (agent→caller): `WRITE_REPLACE` injected the agent's tone into the caller path;
    recorded WRITE max=7932 / rms=5626.
  - `X-Call-ID` correlation header arrived at the agent (`Call-ID=1-123846@…`); FS never crashed.
  - harness: dedicated `earshot-test` sofia profile on :5090 (own context, `auth-calls=false`, RTP on the
    internal IP) so the call stays isolated on the host.
  - `earshot status` reports live stats: `{ws_connected, ready, rx_frames, play_buffered}`.
- ☑ (bugfix) never request `SMBF_WRITE_REPLACE` without setting a replace frame each callback — NULL-derefs FS
- ☑ playback buffer capped (~2 s) so it can't grow unbounded on a leg that isn't written to
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
- ◐ Test harness: ☑ mock WS agent (`test/mock_agent.py`); ☐ full audio round-trip (needs WS transport)

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

- ☑ **Protocol adapters** (`es_proto.c`): `native`, `twilio`, `openai`, `deepgram`, `elevenlabs`,
  `gemini`, `pipecat` — an agent written for any of these works against Earshot unmodified.
  - **All seven VALIDATED on real SIP calls** (in testing) against emulated vendor servers — clean full
    duplex (recorded READ=16896 / WRITE≈7932 each). Real-vendor interop still needs live keys.
    - `twilio`: `connected`+`start` framing, base64 μ-law media both ways, **mark echo-on-drain**.
    - `openai`: `session.update` handshake (g711_ulaw + server_vad), append out / delta in,
      `realtime` subprotocol, `speech_started` = barge-in.
    - `deepgram`: `Settings` handshake (mulaw/linear16 @ 8k), raw **binary** audio, `UserStartedSpeaking`.
    - `elevenlabs`: `user_audio_chunk` out / `{type:audio}` in (base64 ulaw_8000), **auto `ping`→`pong`
      keepalive** (validated), `interruption` = barge-in.
    - `gemini`: `setup` handshake (validated), `realtimeInput.mediaChunks` out / `serverContent` in,
      **asymmetric 16 kHz-in / 24 kHz-out resampled** transparently, `serverContent.interrupted`.
    - `pipecat`: binary **protobuf** `Frame{ audio: AudioRawFrame }` both ways (round-trip validated);
      wire codec is a pure-C unit `es_pb.c` with **unit tests** (`test/test_pb.c`, incl. adversarial input),
      `InterruptionFrame` = barge-in.
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
- ☑ **Module-side VAD + turn events** (works with any agent): FS-native `switch_vad` on the caller
  audio emits `earshot::speech_started` / `earshot::speech_stopped` + sets `earshot_talking`, with
  opt-in **speech-triggered barge-in** (`vad_barge=on`, flush playback the instant the caller speaks)
  and agent turn notify (`vad_notify=on`). Tunables `vad_mode` / `vad_voice_ms` / `vad_silence_ms`.
  **VALIDATED**: both events fired on real caller audio, `earshot_talking` flipped, `speech_starts`
  counted, agent received the turn JSON. Gives endpointing to agents that do none of their own.
- ◐ **Barge-in**: ☑ `flush` command, Twilio `clear`, and VAD `vad_barge` all clear buffered playback
  instantly; ☐ `earshot::playback` events
- ☑ **DTMF capture + PCI masking**: `recv_dtmf` hook → `earshot::dtmf` events + forward to the agent
  (`dtmf=on`); a masking window (`earshot <uuid> mask on|off`, or the control-channel `mask` action)
  **mutes caller→agent audio and redacts DTMF to the agent** during card entry while digits still reach
  FreeSWITCH for a secure collector. **VALIDATED on real RFC2833**: in-window digit suppressed from the
  agent (audit event still fired, redacted), audio froze to 0 frames while masked, both resumed after.
- ☐ Opus over WS (low-bitrate WAN links)
- ☐ App-level auth: `Bearer` token + mTLS client cert in the handshake

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
- ☐ Per-stream jitter buffer tuning; adaptive frame sizing
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

## Release plan

- **v0.1 (private):** Tier 0 + reconnect/ready-gate/μ-law from Tier 1. Dogfood on the
  audio test harness (not prod).
- **v0.2 (private):** rest of Tier 1 + Twilio adapter + correlation + barge-in.
- **v1.0 (public):** Tiers 0–2 solid, docs + pipecat quickstart + demo video, CI releases,
  metrics. Announce.
- **post-1.0:** Tier 3, community-driven from the issue tracker.

## Growth playbook

1. Mine the upstream module's open issues → a pre-validated backlog of real needs.
2. README + a 60-second demo (phone call → talking LLM) + a copy-paste pipecat quickstart.
3. Tagged releases + fast issue triage — responsiveness is the whole differentiator.
4. Cross-promote with the *Signal & Stream* book (Ch. 12 audio contract, Ch. 14 barge-in
   & latency document this module; the module gives the book a real companion repo).
