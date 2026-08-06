# Earshot — Features

Earshot (`mod_earshot`) is the reliable, protocol-flexible FreeSWITCH ↔ AI-agent audio
bridge: it taps a channel's audio, streams it to an agent over a WebSocket, and plays the
agent's audio back to the caller — full duplex, on one leg, with the reliability the stock
tooling never delivered.

> Status legend: **✓ shipped** · ◐ partial · ☐ planned
> The core call path — full-duplex audio, VAD/turn-taking, barge-in, DTMF/PCI masking, fan-out, and
> the control channel — is validated end-to-end on real SIP calls (`sipp` + real audio). Protocol
> adapters vary by vendor: `native`, `openai`, `deepgram`, and `pipecat` are validated against the
> live service/schema; `twilio` is framing/echo-tested; `elevenlabs` and `gemini` are adapter-complete
> and mock-tested in CI (not yet run against the live vendor). See the table in [README](README.md).

---

## At a glance

| Area | Capability |
|---|---|
| Transport | Full-duplex WebSocket (ws/wss), own service thread, never blocks the media path |
| Protocols | `native`, `twilio`, `openai`, `deepgram`, `elevenlabs`, `gemini`, `pipecat` |
| Audio | G.711 µ-law/a-law + L16; transparent resampling 8 k ↔ 16 k ↔ 24 k |
| Turn-taking | Module-side VAD + turn events, ready-gate, barge-in (`flush` / `clear`), Twilio mark echo |
| Fan-out | Many named streams per channel (`id=`); `dir=in` read-only forks (transcription + supervisor) |
| Call control | Agent-driven control channel (transfer, hangup, DTMF, play, record, …) |
| DTMF & PCI | Caller DTMF capture + forward; maskable window mutes audio + redacts digits to the agent |
| Reliability | Auto-reconnect w/ jittered backoff, bounded outbound queue |
| Observability | `earshot::metrics` events + on-demand JSON + lifecycle events |
| Correlation | SIP Call-ID + channel UUID on the handshake and in the agent's start payload |
| Migration | Drop-in `uuid_audio_stream` / `audio_stream` compat (mod_audio_stream syntax) |

---

## 1. Full-duplex audio bridge  ✓

One media-bug tap carries both directions on a single caller leg:

- **caller → agent** via `SMBF_READ_STREAM`
- **agent → caller** via `SMBF_WRITE_REPLACE`

The playback direction is the headline: every other FreeSWITCH streaming module has
long-standing bugs where agent audio decodes but never reaches the caller. Earshot's
`WRITE_REPLACE` path is validated with real audio in both directions simultaneously.

```xml
<action application="answer"/>
<action application="earshot" data="start ws://agent.internal:9099/ws codec=pcmu"/>
```

## 2. Protocol adapters  ✓

An agent written for any of these speaks to Earshot unmodified — the adapter handles framing,
base64, and the session handshake.

| `proto=` | Wire format | Barge-in signal |
|---|---|---|
| `native` | raw binary frames + small JSON control (`playAudio` / `clear`) | `clear` |
| `twilio` | Twilio Media Streams (`connected`/`start`/`media`/`mark`), base64 µ-law | `clear` |
| `openai` | OpenAI Realtime (`session.update`, `input_audio_buffer.append`, `response.output_audio.delta`) | `speech_started` |
| `deepgram` | Deepgram Voice Agent (`Settings`, raw binary audio) | `UserStartedSpeaking` |
| `elevenlabs` | ElevenLabs Conversational AI (`user_audio_chunk` / `{type:audio}`, base64 µ-law), auto `ping`→`pong` | `interruption` |
| `gemini` | Gemini Live (`setup`, `realtimeInput.mediaChunks` 16k / `serverContent` 24k PCM) | `serverContent.interrupted` |
| `pipecat` | Pipecat protobuf `Frame{ audio: AudioRawFrame }` (binary, L16) | `InterruptionFrame` |

- Twilio **mark echo-on-drain** round-trips (agents that wait on marks work).
- OpenAI negotiates the `realtime` WebSocket subprotocol automatically (auth via `EARSHOT_AUTH`).
- ElevenLabs' `ping` keepalive is answered with `pong` automatically (no dropped sessions).
- Gemini's asymmetric rates (16 kHz in / 24 kHz out) are resampled transparently to the channel.
- Pipecat frames are hand-encoded protobuf — the codec is a pure-C unit (`es_pb`) with its own tests.
- `openai`/`deepgram`/`gemini`/`elevenlabs` session config (voice/model/keys/prompt) defaults to an
  audio-format handshake, or supply a full JSON via the `EARSHOT_SESSION_CONFIG` channel variable.

## 3. Codecs & resampling  ✓

- G.711 **µ-law** / **a-law** and **L16** (unit-tested against the canonical ITU/Sun algorithms).
- **Transparent resampling** (FreeSWITCH's bundled speex resampler, no extra dependency) between
  the channel rate and the wire rate — so 16 kHz / 24 kHz agents (e.g. OpenAI `pcm16` @ 24 k) work
  even on an 8 kHz telephony channel. Validated at 8↔16 k and 8↔24 k with amplitude preserved.

```
start ... codec=l16 rate=24000     # pcm16 agent; earshot resamples from the 8k channel
```

## 4. Ready-gate  ✓

Designs out "answered into silence": hold playback until media is confirmed flowing, then fire
`earshot::ready` + set the `earshot_ready` channel variable so the dialplan can react.

```
start ... ready=firstframe    # default: open on the agent's first audio frame
          ready=connect       # open as soon as the WebSocket connects
          ready=manual        # open only on an explicit `earshot <uuid> resume`
```

## 5. Barge-in  ✓

Turn detection stays live during playback; when the caller (or the agent's protocol) signals an
interruption, buffered agent audio is dropped instantly.

- `earshot <uuid> flush` — clear queued playback now.
- Twilio `clear`, OpenAI `speech_started`, Deepgram `UserStartedSpeaking` all map to the same flush.

## 6. Turn detection — module-side VAD  ✓

Energy VAD (FreeSWITCH's native `switch_vad`, hangover-aware) runs on the caller's audio in the
media tap, so **any** agent gets turn boundaries — even one that does no endpointing of its own.
On each transition Earshot fires an event, sets `earshot_talking`, and can act on it:

- **`earshot::speech_started` / `earshot::speech_stopped`** events (+ `earshot_talking=true|false` var).
- **Auto barge-in** (`vad_barge=on`): the instant the caller speaks, the agent's queued playback is
  flushed — barge-in that works regardless of the agent's protocol.
- **Turn notify** (`vad_notify=on`): send `{"type":"speech_started"}` / `{"type":"speech_stopped"}`
  to the agent, giving it free endpointing.
- `speech_starts` + `talking` are surfaced in metrics.

```
start ... vad=on vad_barge=on           # endpointing + interrupt-on-speech
          vad_voice_ms=200              # speech held this long -> started (default 200)
          vad_silence_ms=500            # silence this long -> stopped (endpoint budget, default 500)
          vad_mode=2                    # -1 native energy, 0..3 fvad aggressiveness (default 2)
```

**Validated**: real caller audio → `speech_started` + `speech_stopped` events fired, `earshot_talking`
flipped, `speech_starts` counted, and the agent received the turn notifications over the WebSocket.

## 7. Control channel — agent drives the call  ✓  *(opt-in)*

The agent sends `{"type":"command","action":…}` over the same WebSocket; Earshot maps each
**whitelisted** action to the equivalent thread-safe `uuid_*` FreeSWITCH API, executes it, fires an
`earshot::command` audit event, and echoes a `command_result`. Turns an LLM's tool calls into real
telephony actions. **Off by default** — enable per stream with `commands=true`.

| action | effect |
|---|---|
| `transfer` | `uuid_transfer` — move the call to another extension/context |
| `hangup` | `uuid_kill` — end the call (optional `cause`) |
| `send_dtmf` | `uuid_send_dtmf` — send digits (e.g. navigate an IVR) |
| `play` / `stop_play` | `uuid_broadcast` / `uuid_break` — play or stop a prompt |
| `record` | `uuid_record` — start/stop recording |
| `setvar` | `uuid_setvar` — set a channel variable |
| `hold` | `uuid_hold` — hold / unhold |
| `bridge` | `uuid_bridge` — bridge to another UUID |
| `park` | `uuid_park` |

```
start ... commands=true
agent → {"type":"command","action":"transfer","to":"9000","id":"c1"}
agent ← {"type":"command_result","id":"c1","action":"transfer","ok":true}
```

Unknown actions are reported `ok:false,"error":"unsupported"`; when the gate is closed,
`ok:false,"error":"disabled"`. Every command is audited via `earshot::command`.

## 8. DTMF capture & PCI masking  ✓

Capture the caller's DTMF and forward it to the agent, with an enterprise-grade **masking window**
for card entry — a self-hosted capability a managed SaaS makes you pay dearly for.

- **Capture** (`dtmf=on`): a session `recv_dtmf` hook turns caller digits into `earshot::dtmf` events
  and forwards them to the agent (native `{"type":"dtmf","digit":"5"}` / Twilio `dtmf` event).
- **Masking window** — `earshot <uuid> mask on|off` (operator) or `{"action":"mask","state":"on"}`
  (agent, via the control channel). While masked:
  - caller → agent **audio is muted** (spoken card numbers + in-band DTMF never reach the agent),
  - DTMF is **suppressed** from the agent; an `earshot::dtmf` audit event fires with the **digit redacted**,
  - digits still flow to FreeSWITCH, so a secure collector (`play_and_get_digits`) captures the payment.
- The agent (LLM) can mask itself the moment it asks for a card, then unmask when done.

```
earshot <uuid> mask on     # card entry: nothing leaves the box
earshot <uuid> mask off
```

**Validated on real RFC2833 calls**: digits pressed outside the window reached the agent; a digit
pressed *inside* the window did **not** reach the agent (audit event still fired, redacted); and
caller→agent audio froze completely (0 frames) while masked, then resumed.

## 9. Observability  ✓

- **`earshot::metrics` events** — periodic (`metrics=<seconds>`), a final snapshot on close, and
  on-demand (`earshot <uuid> metrics` returns JSON). Fields: `tx/rx-frames`, `tx/rx-bytes`,
  `play-drops`, `commands`, `play-buffered`, `ws-connected`, `ws-reconnects`, `ws-queue-drops`,
  `duration-ms`, `speech-starts`, `talking`, `dtmf`, `masking`.
- **Latency KPIs** — the numbers voice teams optimize, in the same metrics: `first-audio-ms`
  (stream start → agent's first audio), `response-ms` / `response-ms-max` (caller stops speaking →
  agent's first audio, per turn — uses the VAD) + `turns`, and `ws-rtt-ms` (a WebSocket ping/pong
  every 5 s). Validated live: first-audio ≈ 65 ms and RTT = 1 ms against a localhost agent.
- **Lifecycle events**: `earshot::connected`, `earshot::ready`, `earshot::command`, `earshot::dtmf`,
  `earshot::speech_started`, `earshot::speech_stopped` (all subclasses reserved, so ESL subscribers receive them).
- **`earshot <uuid> status`** — quick liveness JSON.

```
start ... metrics=10          # emit earshot::metrics every 10s
```

## 10. Reliability  ✓

- Auto-reconnect with **exponential backoff + jitter** (thundering-herd guard).
- **Bounded outbound queue** (drop-oldest): buffers audio across a brief blip, but a long outage
  drops rather than growing unbounded or replaying stale audio.
- Single clean teardown path; no crash if the agent hangs up first.

```
EARSHOT_NO_RECONNECT=true     # channel var to disable auto-reconnect (backpressure is automatic drop-oldest)
```

## 11. Correlation-first  ✓

Trace one call across LB / FreeSWITCH / controller / agent by two keys:

- Handshake headers `X-Call-ID`, `X-Channel-UUID`, `X-Correlation-ID`.
- Twilio `start.customParameters` carries `earshot_call_id` + `earshot_channel_uuid`.
- Defaults `corr` to the SIP Call-ID; override with `corr=<id>`.

## 12. mod_audio_stream compatibility  ✓

Drop-in for [amigniter/mod_audio_stream](https://github.com/amigniter/mod_audio_stream)'s positional
syntax so existing dialplans run against Earshot unchanged — native L16 at the requested rate,
resampled to the channel. Registers **only when those names are free**, so it never collides with a
still-loaded `mod_audio_stream`.

```xml
<action application="audio_stream" data="start wss://agent/ws mono 16k"/>
<!-- uuid_audio_stream <uuid> start|stop|pause|resume|send_text ... -->
```

## 13. Multi-stream fan-out  ✓

One caller, many sinks. Start several named streams on the same channel — a bidirectional **agent**
plus read-only **forks** (`dir=in`) for live transcription, a supervisor monitor, compliance capture,
analytics — all tapping the caller audio at once. Only the agent (`dir=both`) owns `WRITE_REPLACE`,
so the forks never fight over playback.

```xml
<action application="earshot" data="start ws://agent/ws"/>                          <!-- agent (default) -->
<action application="earshot" data="start ws://stt/ws  id=transcribe dir=in proto=deepgram"/>
<action application="earshot" data="start ws://mon/ws  id=supervisor dir=in"/>
```

Every verb targets a stream by id: `earshot <uuid> status id=transcribe`, `earshot <uuid> stop id=supervisor`.
Each stream has its own protocol, codec, metrics, and lifecycle; all share the call's correlation id.

**Validated**: three streams on one channel all received the caller audio simultaneously; the agent
played back while the forks did not; stopping one fork left the others running.

## 14. Security  ◐

- `wss://` (TLS) with server-cert verification against the system trust store; optional
  `auth=<token>` → `Authorization` header (use the `EARSHOT_AUTH` channel var for a value with a
  space, e.g. `Bearer <key>` — the option list is split on spaces).
- **Box-level mTLS + custom CA** ✓ *(validated end-to-end on a real SIP call)* —
  `EARSHOT_TLS_CLIENT_CERT` + `EARSHOT_TLS_CLIENT_KEY` present a client certificate on every agent
  connection; `EARSHOT_TLS_CA` verifies the agent against a private CA — note it **replaces the system
  trust store box-wide**, so don't set it on a box that also talks to public-CA vendors. One client
  identity per box (unencrypted-PEM key).
- Control channel is **opt-in** (`commands=true`) and whitelisted to call-control APIs only.
- **PCI/PII masking** shipped (§8): mute audio + redact DTMF to the agent during card entry.
- ☐ Planned: per-stream (multi-identity) mTLS, per-action command scoping, OAuth refresh, consent-gated recording.

---

## Command & option reference

**Options (`start …`)**: `id=<name>` · `dir=in|out|both` · `codec=l16|pcmu|pcma` · `rate=8000|16000|24000`
· `proto=native|twilio|openai|deepgram|elevenlabs|gemini|pipecat`
· `ready=firstframe|connect|manual` · `vad=on` · `vad_barge=on` · `vad_notify=on`
· `vad_mode=-1..3` · `vad_voice_ms=<n>` · `vad_silence_ms=<n>` · `dtmf=on` · `mask=on` · `commands=true`
· `metrics=<seconds>` · `corr=auto|<id>` · `auth=<token>` (or the `EARSHOT_AUTH` var for a value with a space)

**Verbs** (all accept an optional `id=<name>` after the verb to target a fan-out stream):
`start` · `stop` · `pause` · `resume` · `flush` · `send <text>` · `mask on|off` · `status` · `metrics`

**Channel variables**: `EARSHOT_SESSION_CONFIG` (openai/deepgram/gemini/elevenlabs) · `EARSHOT_AUTH`
· `EARSHOT_NO_RECONNECT` · `EARSHOT_TLS_NO_HOSTNAME_CHECK` · (set by earshot) `earshot_ready`
· `earshot_talking` · `earshot_masking`

**Events**: `earshot::connected` · `earshot::ready` · `earshot::metrics` · `earshot::command` · `earshot::dtmf`
· `earshot::speech_started` · `earshot::speech_stopped`

See [ROADMAP.md](ROADMAP.md) for what's next.
