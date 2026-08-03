# Earshot — `mod_earshot`

> A FreeSWITCH module that streams live call audio to your AI agent over a WebSocket —
> and plays the agent's voice back — full duplex, with seven ready-made protocol adapters,
> module-side turn detection, agent-driven call control, PCI masking, multi-stream fan-out,
> and built-in latency metrics.

**Status:** pre-release, MIT. Every capability below is validated end-to-end on real SIP calls
(`sipp` + real audio) on a test environment. Not yet wired into production.

Earshot is an independent, from-scratch implementation, **inspired by** the excellent
[`amigniter/mod_audio_stream`](https://github.com/amigniter/mod_audio_stream) (MIT) — credit in
[NOTICE](NOTICE) — but rethought for teams running AI voice at scale.

---

## Why Earshot

Every FreeSWITCH voice-AI write-up hits the same wall: the incumbent module's **playback**
(agent → caller) doesn't work reliably, forcing `uuid_broadcast` hacks. Earshot's `WRITE_REPLACE`
path is validated with real audio, full duplex, on one leg. That's the wedge — everything else builds
on it. The full pitch is in **[WHY.md](WHY.md)**; the complete capability list is in **[FEATURES.md](FEATURES.md)**.

- **7 wire protocols, agent unmodified** — `native`, `twilio`, `openai` (Realtime),
  `deepgram` (Voice Agent), `elevenlabs`, `gemini` (Live), `pipecat`. Swap vendors with one word.
- **Turn-taking for any agent** — module-side VAD → `speech_started`/`speech_stopped`, a ready-gate
  (no "answered into silence"), and speech-triggered barge-in.
- **Agent drives the call** — opt-in, whitelisted control channel: transfer, hangup, DTMF, play,
  record, hold, bridge, setvar.
- **PCI/PII masking** — mute audio + redact DTMF to the agent during card entry.
- **Multi-stream fan-out** — agent + live transcription + supervisor on one call.
- **Latency KPIs** — time-to-first-audio, per-turn response time, WebSocket RTT.
- **Codecs + resampling** — G.711 µ-law/a-law + L16, transparent 8k/16k/24k.
- **Reliable + observable** — reconnect w/ jitter, bounded queue, `earshot::metrics` events,
  correlation by SIP Call-ID + channel UUID.
- **Drop-in migration** — `uuid_audio_stream` / `audio_stream` compat runs mod_audio_stream dialplans unchanged.

---

## 60-second quickstart

**1. Build & install** (needs FreeSWITCH dev headers + `libwebsockets-dev`):

```bash
cmake -S . -B build && cmake --build build
sudo cmake --install build        # -> /usr/lib/freeswitch/mod/mod_earshot.so
fs_cli -x "load mod_earshot"
```

**2. Point a call at your agent** — dialplan:

```xml
<extension name="ai-agent">
  <condition field="destination_number" expression="^5000$">
    <action application="answer"/>
    <action application="earshot" data="start ws://127.0.0.1:9099/ws proto=native vad=on vad_barge=on"/>
    <action application="playback" data="silence_stream://-1"/>   <!-- keep the leg up -->
  </condition>
</extension>
```

**3. Any WebSocket agent works.** A minimal echo agent (hear yourself talk):

```python
# pip install websockets
import asyncio, websockets
async def handle(ws):
    async for msg in ws:            # binary caller audio (µ-law 8k by default)
        await ws.send(msg)          # ...echo it straight back to the caller
async def main():
    async with websockets.serve(handle, "0.0.0.0", 9099):
        await asyncio.Future()
asyncio.run(main())
```

Call `5000` and you'll hear yourself — proving both directions. Swap the echo for your LLM,
or point `proto=` at OpenAI/Deepgram/ElevenLabs/Gemini/Pipecat and run your existing agent unchanged.
More recipes (pipecat, OpenAI Realtime, transcription fan-out, PCI masking) in **[QUICKSTART.md](QUICKSTART.md)**.

---

## The command surface

Dialplan **app** (operates on the current channel):

```xml
<action application="earshot" data="start ws://agent/ws proto=openai commands=true metrics=10"/>
```

**API** (fs_cli / ESL — `<uuid>` first, then the verb):

```
earshot <uuid> start ws://agent/ws proto=native [id=<name>] [dir=in|out|both] ...
earshot <uuid> flush                 # barge-in: clear queued agent audio
earshot <uuid> send  '{"type":"..."}'
earshot <uuid> mask  on|off          # PCI: mute audio + redact DTMF to the agent
earshot <uuid> status | metrics      # per-stream JSON (add id=<name> for a fan-out fork)
earshot <uuid> stop
```

Fan-out — many streams on one channel:

```xml
<action application="earshot" data="start ws://agent/ws"/>
<action application="earshot" data="start ws://stt/ws id=transcribe dir=in proto=deepgram"/>
<action application="earshot" data="start ws://mon/ws id=supervisor dir=in"/>
```

Full option/verb/event reference: **[FEATURES.md](FEATURES.md)** · API detail: **[docs/API.md](docs/API.md)** ·
internals: **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)**.

---

## Build, package, test

```bash
cmake -S . -B build && cmake --build build   # module (needs FS headers) + unit tests
ctest --test-dir build                       # codec / queue / protobuf unit tests (no FS needed)
cpack --config build/CPackConfig.cmake -G DEB   # -> mod-earshot_*.deb  (when FS headers are present)
docker build -t earshot-build .              # reproducible build image (see Dockerfile)
```

The portable core (codec, protobuf) unit-tests without FreeSWITCH, so CI stays
green on any runner; the module itself compiles where FS dev headers are available. See
[`.github/workflows/ci.yml`](.github/workflows/ci.yml).

## Naming

`Earshot` / `mod_earshot` is a working name (evokes "within earshot"). It's a single token across the
source and CMake — see the note at the top of `src/mod_earshot.c` to rename.

## License

MIT — see [LICENSE](LICENSE). Attribution and provenance in [NOTICE](NOTICE).
Contributions welcome — [CONTRIBUTING.md](CONTRIBUTING.md).
