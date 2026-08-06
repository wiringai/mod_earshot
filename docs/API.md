# Earshot API

`mod_earshot` registers a dialplan **app** and an **API** named `earshot` (plus a
`uuid_audio_stream`/`audio_stream` compat shim), a set of `earshot::` custom events, and a
few `EARSHOT_*` channel variables. Options are **named** (`key=value`) — no positional order
to memorize, and new options never shift an existing one.

- **App** (dialplan, current channel): `<action application="earshot" data="<verb> …"/>`
- **API** (fs_cli / ESL, explicit uuid **first**): `earshot <uuid> <verb> …`

## Verbs

### `start <url> [key=value …]`
Begin streaming the channel's audio to `url` (`ws://` or `wss://`).

| Option | Values | Default | Notes |
|---|---|---|---|
| `id` | `<name>` | *(default stream)* | fan-out: name this stream so many can run on one channel |
| `proto` | `native` `twilio` `openai` `deepgram` `elevenlabs` `gemini` `pipecat` | `native` | wire adapter (see [Protocol adapters](#protocol-adapters)) |
| `codec` | `pcmu` `pcma` `l16` | `pcmu` | g711 = 8-bit telephony (½ the bytes); some protos pin the codec |
| `rate` | `8000` `16000` `24000` | `8000` | wire rate; resampled to/from the channel rate |
| `dir` | `in` `out` `both` | `both` | `in` = caller→agent only (read-only **fork**, no playback); `both` = bidirectional agent |
| `ready` | `firstframe` `connect` `manual` | `firstframe` | when playback opens (see [Ready gate](#ready-gate)) |
| `vad` | `on` | off | module-side VAD → `speech_started`/`speech_stopped` |
| `vad_barge` | `on` | off | back-compat alias for `interruptible=speech` |
| `interruptible` | `none` `dtmf` `speech` `any` | `none` (or `speech` if `vad_barge=on`) | what interrupts the agent's playback |
| `ignore_backchannel` | `on` | off | require *sustained* speech before a speech-barge (drops short "yeah/okay") |
| `sensitivity` | `low` `medium` `high` | — | preset for the sustained-speech gate (600 / 300 / 150 ms) |
| `barge_min_ms` | `<n>` | `0` | ms of sustained caller speech before a speech-barge (`0` = immediate) |
| `barge_fade_ms` | `<n>` | `0` | fade playback out over N ms instead of a hard cut |
| `vad_notify` | `on` | off | also send `{"type":"speech_started"}` to the agent |
| `vad_mode` / `vad_voice_ms` / `vad_silence_ms` | ints | `2` / `200` / `500` | VAD aggressiveness + endpointing budget |
| `dtmf` | `on` | off | capture caller DTMF → `earshot::dtmf` + forward to the agent |
| `mask` | `on` | off | start in a PCI masking window (mute audio + redact DTMF to the agent) |
| `commands` | `true` | false | **opt-in** control channel (agent can drive the call) |
| `metrics` | `<seconds>` | `0` | emit `earshot::metrics` every N seconds |
| `corr` | `auto` \| `<id>` | `auto` | correlation id; `auto` = SIP Call-ID |
| `auth` | `<token>` (no spaces) | — | `Authorization` header. A value **containing a space** (`Bearer <key>`, `Token <key>`) must be passed via the **`EARSHOT_AUTH`** channel variable instead — the option list is split on spaces, so `auth=Bearer …` would truncate to `Bearer` |
| `greeting` | `<file>` (no spaces) | — | welcome audio played into the channel the moment the [ready gate](#ready-gate) opens, ahead of the agent's first words (see [Welcome greeting](#welcome-greeting)). A path **with spaces** goes via the **`EARSHOT_GREETING`** channel variable |

Caller context (customer id, tier, call reason, …) is passed to the agent at connect via the **`EARSHOT_META`** channel variable — see [Caller context](#caller-context).

### `stop` · `pause` · `resume`
Tear down / suspend / resume the stream. Channel hangup also tears down cleanly.

### `flush` — barge-in
Immediately drop queued agent audio (also driven by protocol clear signals and `vad_barge`).

### `send <text|json>`
Write a raw message to the agent socket.

### `mask on|off`
Toggle the PCI masking window (operator control; the agent can also toggle it via the control channel).

### `status` · `metrics`
Return per-stream JSON. `metrics` also fires an `earshot::metrics` event. Fields include
`proto`, `corr`, `id`, `tx/rx_frames`, `tx/rx_bytes`, `play_drops`, `commands`, `speech_starts`,
`talking`, `dtmf`, `masking`, `ws_connected`, `ws_reconnects`, `ws_queue_drops`, and the latency
KPIs `first_audio_ms`, `response_ms` / `response_ms_max`, `turns`, `ws_rtt_ms`.

**Targeting a fan-out stream:** every non-`start` verb accepts an `id=<name>` immediately after the
verb, e.g. `earshot <uuid> status id=transcribe`, `earshot <uuid> stop id=supervisor`.

## Control channel (`commands=true`)

The agent sends `{"type":"command","action":…}` over the same socket; Earshot maps each whitelisted
action to a thread-safe `uuid_*` API, audits it via `earshot::command`, and replies `command_result`.

```jsonc
{"type":"command","action":"transfer","to":"2000","id":"c1"}   // -> uuid_transfer
{"type":"command","action":"send_dtmf","digits":"1"}
{"type":"command","action":"mask","state":"on"}                // PCI masking
```
Actions: `transfer · hangup · send_dtmf · play · stop_play · record · setvar · hold · bridge · park · mask`.
Off by default; unknown/disabled → `{"ok":false,"error":…}`.

## Events (subclass `earshot::…`)

| Event | Key headers | Fires when |
|---|---|---|
| `earshot::connected` | `url` | WS handshake completes |
| `earshot::ready` | `corr` | playback gate opens (also sets `earshot_ready=true`) |
| `earshot::speech_started` / `earshot::speech_stopped` | `corr` | VAD turn boundaries (sets `earshot_talking`) |
| `earshot::dtmf` | `digit` or `masked` | caller DTMF (`masked=true`, digit redacted, during a mask window) |
| `earshot::command` | `action`, `api`, `ok`, `result` | a control-channel command ran |
| `earshot::metrics` | counters + latency KPIs (`first-audio-ms`, `ws-rtt-ms`, …) | periodic / on-close / on-demand |

All subclasses are reserved, so ESL subscribers receive them: `event plain CUSTOM earshot::metrics`.

## Ready gate

Playback into the channel is held until media is confirmed flowing, so an agent's first words never
land in silence. `ready=firstframe` (default) opens on the agent's first audio frame; `ready=connect`
on the WS handshake; `ready=manual` waits for `earshot <uuid> resume`. Opening fires `earshot::ready`
and sets the `earshot_ready` channel variable.

## Welcome greeting

`greeting=<file>` (or the `EARSHOT_GREETING` channel variable for paths with spaces) plays an audio
file to the caller **the instant the ready gate opens**, ahead of the agent's first words — a
module-owned prompt that never lands in silence and needs no round-trip to the model. The file is
loaded once at `start` and resampled to the channel rate (any format FreeSWITCH can open; bounded to
15 s). It is emitted into the playout *before* any agent audio — whichever thread opens the gate
emits it first — so it leads in **every** `ready` mode (`firstframe`, `connect`, `manual`), with the
agent's reply queued behind it. Use `ready=connect` when you want the greeting to play as soon as the
socket connects rather than waiting on the agent's first frame. Barge-in (`flush` / `vad_barge`) cuts
it like any other playout. Read-only forks (`dir=in`) never play back and ignore it.

For a **dynamic** greeting, prefer the agent's own (e.g. Deepgram's `greeting` in
`EARSHOT_SESSION_CONFIG`) or pre-render the text to a file — `greeting=` is a fixed audio prompt, not
TTS.

## Caller context

Set the **`EARSHOT_META`** channel variable to pass per-call context to the agent at setup — a
customer id, account tier, call reason, campaign, anything. Earshot sends it verbatim as the
**`X-Earshot-Meta`** header on the WebSocket handshake (alongside `X-Call-ID` / `X-Channel-UUID` /
`X-Correlation-ID`), so it's readable by any agent framework at connect, regardless of `proto`.
Keep it compact and single-line (it's an HTTP header) — JSON is the natural shape:

```xml
<action application="set" data="EARSHOT_META={&quot;customer_id&quot;:&quot;C-8842&quot;,&quot;tier&quot;:&quot;gold&quot;,&quot;reason&quot;:&quot;billing&quot;}"/>
```

This is transport-level context for the handshake; proto-specific in-band session config (voices,
models, prompts) still goes through `EARSHOT_SESSION_CONFIG`.

## Protocol adapters

`proto=` selects how audio + control map onto the wire, so an existing agent works unchanged.
Full framing details in [FEATURES.md](../FEATURES.md#2-protocol-adapters--); in brief:

- **`native`** — raw binary frames + `{"type":"playAudio"|"clear"}` JSON control.
- **`twilio`** — Twilio Media Streams (`connected`/`start`/`media`/`mark`, base64 µ-law, mark echo).
- **`openai`** — OpenAI Realtime (`session.update`, `input_audio_buffer.append`, `response.output_audio.delta`; `realtime` subprotocol, auth via `EARSHOT_AUTH`).
- **`deepgram`** — Deepgram Voice Agent (`Settings`, raw binary audio, `UserStartedSpeaking`).
- **`elevenlabs`** — ElevenLabs Conversational AI (`user_audio_chunk` / `{type:audio}`, auto `ping`→`pong`).
- **`gemini`** — Gemini Live (`setup`, `realtimeInput.mediaChunks` 16k / `serverContent` 24k; resampled).
- **`pipecat`** — Pipecat protobuf `Frame{ audio: AudioRawFrame }` (binary L16).

For `openai`/`deepgram`/`gemini`/`elevenlabs`, provide the full session config (voice/model/keys) via
the `EARSHOT_SESSION_CONFIG` channel variable; otherwise Earshot sends an audio-format default.

## Channel variables

| Variable | Purpose |
|---|---|
| `EARSHOT_SESSION_CONFIG` | verbatim first message for openai/deepgram/gemini/elevenlabs |
| `EARSHOT_META` | opaque caller context → `X-Earshot-Meta` handshake header (see [Caller context](#caller-context)) |
| `EARSHOT_GREETING` | welcome-audio file path (alternative to `greeting=` for paths with spaces) |
| `EARSHOT_NO_RECONNECT` | disable auto-reconnect (default: reconnect with jittered backoff) |
| `EARSHOT_TLS_NO_HOSTNAME_CHECK` | skip wss cert/hostname checks (dev only) |
| `earshot_ready` / `earshot_talking` / `earshot_masking` | **set by** Earshot for dialplan logic |

## Box-level TLS (environment)

Mutual TLS and custom-CA verification are configured **per box** (not per stream), via process
environment variables read once at module load and applied to every agent connection:

| Env var | Purpose |
|---|---|
| `EARSHOT_TLS_CLIENT_CERT` | PEM client certificate to present (enables mTLS) |
| `EARSHOT_TLS_CLIENT_KEY` | matching private key (unencrypted PEM) |
| `EARSHOT_TLS_CA` | verify the agent against this private CA **instead of** the system trust store — applies to **every** connection on the box, so public-CA endpoints (OpenAI, Deepgram) will fail; set only when all agents chain to this CA |

One client identity per box — libwebsockets binds client TLS material at the shared-context level, so
distinct per-stream certificates are not supported.

## Compatibility

`uuid_audio_stream` / `audio_stream` accept mod_audio_stream's positional syntax
(`start <url> <mix> <rate>`) and run existing dialplans unchanged — registered only when those names
are free (won't collide with a loaded `mod_audio_stream`). See [FEATURES.md](../FEATURES.md).
