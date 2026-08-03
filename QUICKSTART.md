# Earshot quickstart recipes

Copy-paste recipes. Each is a dialplan snippet plus the agent side. Assumes `mod_earshot` is loaded
(`fs_cli -x "load mod_earshot"`) and your caller reaches the extension. Default wire codec is G.711
µ-law @ 8 kHz — no transcoding for telephony.

- [1. Echo (prove both directions)](#1-echo)
- [2. OpenAI Realtime](#2-openai-realtime)
- [3. Deepgram Voice Agent](#3-deepgram-voice-agent)
- [4. Pipecat](#4-pipecat)
- [5. Transcription fan-out (agent + live STT)](#5-transcription-fan-out)
- [6. PCI card-capture masking](#6-pci-masking)
- [7. Agent-driven call control](#7-call-control)
- [8. Watch the metrics](#8-metrics)

---

## 1. Echo

Hear yourself talk — the fastest proof that audio flows both ways.

```xml
<action application="answer"/>
<action application="earshot" data="start ws://127.0.0.1:9099/ws proto=native"/>
<action application="playback" data="silence_stream://-1"/>
```

```python
# pip install websockets ; python3 echo.py
import asyncio, websockets
async def handle(ws):
    async for msg in ws:
        await ws.send(msg)          # echo caller audio straight back
async def main():
    async with websockets.serve(handle, "0.0.0.0", 9099):
        await asyncio.Future()
asyncio.run(main())
```

## 2. OpenAI Realtime

Your OpenAI Realtime agent works unmodified — Earshot speaks its wire protocol (`session.update`,
`input_audio_buffer.append`, `response.audio.delta`) and adds the required `OpenAI-Beta` header.

```xml
<action application="set" data="EARSHOT_SESSION_CONFIG={"type":"session.update","session":{"instructions":"You are a helpful receptionist.","input_audio_format":"g711_ulaw","output_audio_format":"g711_ulaw","turn_detection":{"type":"server_vad"}}}"/>
<action application="answer"/>
<action application="earshot" data="start wss://api.openai.com/v1/realtime?model=gpt-4o-realtime-preview proto=openai auth=Bearer sk-... commands=true"/>
<action application="playback" data="silence_stream://-1"/>
```

- `auth=Bearer sk-...` becomes the `Authorization` header.
- `EARSHOT_SESSION_CONFIG` (a channel var) is sent verbatim as the first message — put your
  instructions/voice there. Omit it and Earshot sends a sane g711 + server-VAD default.
- Set `codec=l16 rate=24000` instead for OpenAI `pcm16` (Earshot resamples from the 8 kHz channel).

## 3. Deepgram Voice Agent

```xml
<action application="answer"/>
<action application="earshot" data="start wss://agent.deepgram.com/v1/agent/converse proto=deepgram auth=Token dg-..."/>
<action application="playback" data="silence_stream://-1"/>
```

Supply the full agent `Settings` (LLM/TTS/STT providers + keys) via `EARSHOT_SESSION_CONFIG`; Earshot
sends a mulaw/8k audio-format default otherwise.

## 4. Pipecat

Earshot speaks Pipecat's protobuf `Frame` wire format, so a `WebsocketServerTransport` with the
`ProtobufFrameSerializer` connects directly.

```xml
<action application="answer"/>
<action application="earshot" data="start ws://your-pipecat-host:8765/ws proto=pipecat"/>
<action application="playback" data="silence_stream://-1"/>
```

Pipecat audio is L16; Earshot defaults it to 16 kHz and resamples to/from the 8 kHz channel.

## 5. Transcription fan-out

Run the agent **and** a live transcription fork **and** a supervisor monitor on the same call. Forks
are `dir=in` (read-only — they never fight the agent for playback).

```xml
<action application="answer"/>
<action application="earshot" data="start ws://agent/ws proto=native"/>
<action application="earshot" data="start wss://api.deepgram.com/v1/listen id=transcribe dir=in proto=deepgram auth=Token dg-..."/>
<action application="earshot" data="start ws://supervisor/ws id=supervisor dir=in"/>
<action application="playback" data="silence_stream://-1"/>
```

Manage each independently: `earshot <uuid> status id=transcribe`, `earshot <uuid> stop id=supervisor`.

## 6. PCI masking

During card entry, mute the caller's audio and redact DTMF to the agent — while a secure collector
still gets the digits. The agent (with `commands=true`) can mask itself the moment it asks for a card:

```xml
<action application="earshot" data="start ws://agent/ws proto=native dtmf=on commands=true"/>
```

```jsonc
// agent, over the same WebSocket, when it needs the card:
{"type":"command","action":"mask","state":"on"}
// ...caller enters the card via DTMF — nothing reaches the agent or its logs...
{"type":"command","action":"mask","state":"off"}
```

Or from the operator side: `earshot <uuid> mask on` / `earshot <uuid> mask off`.

## 7. Call control

Turn the agent's tool calls into real telephony. Enable with `commands=true`; actions map to
whitelisted `uuid_*` APIs and every one is audited via `earshot::command`.

```jsonc
{"type":"command","action":"transfer","to":"2000","id":"c1"}       // -> uuid_transfer
{"type":"command","action":"send_dtmf","digits":"1"}               // navigate an IVR
{"type":"command","action":"record","state":"start","file":"/tmp/call.wav"}
{"type":"command","action":"hangup","cause":"NORMAL_CLEARING"}
// Earshot replies: {"type":"command_result","id":"c1","action":"transfer","ok":true}
```

Actions: `transfer · hangup · send_dtmf · play · stop_play · record · setvar · hold · bridge · park · mask`.

## 8. Metrics

```xml
<action application="earshot" data="start ws://agent/ws proto=native vad=on metrics=10"/>
```

Emits an `earshot::metrics` custom event every 10 s (subscribe over ESL: `event plain CUSTOM earshot::metrics`),
and `earshot <uuid> metrics` returns JSON on demand — including the latency KPIs
`first_audio_ms`, `response_ms`, `ws_rtt_ms`. See [FEATURES.md](FEATURES.md) for every field.

---

### Migrating from mod_audio_stream?

Your existing dialplans run unchanged — Earshot registers `uuid_audio_stream` / `audio_stream` when
those names are free. Unload `mod_audio_stream` first, then load `mod_earshot`; no dialplan edits.
