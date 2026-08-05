# Why Earshot

Earshot is the reliable, protocol-flexible FreeSWITCH ↔ AI-agent audio bridge. This is the short
version of what makes it different from the alternatives — the pitch for the open-source launch.
(For the full capability list see [FEATURES.md](FEATURES.md); for what's next, [ROADMAP.md](ROADMAP.md).)

The core call path below is **validated on real SIP calls** (`sipp` + real audio). Protocol adapters
vary by vendor: `native`, `openai`, `deepgram`, and `pipecat` are validated against the live
service/schema; `elevenlabs` and `gemini` ship adapter-complete and mock-tested in CI (see the status
table in [README](README.md)).

---

## The one that matters: playback actually works

Every FreeSWITCH voice-AI write-up hits the same wall — the incumbent streaming module's playback
(application → caller) "simply didn't work reliably," forcing `uuid_broadcast` hacks and other
workarounds. Earshot's `WRITE_REPLACE` path is validated with real audio, full duplex, on one leg.
**This is the wedge**: if you've fought half-working agent audio on FreeSWITCH, Earshot is the fix.

## Distinguished features

1. **Seven wire protocols, your agent unmodified.** `native`, `twilio` (Media Streams), `openai`
   (Realtime), `deepgram` (Voice Agent), `elevenlabs`, `gemini` (Live), `pipecat` (protobuf). Swap
   vendors by changing one word (`proto=`). Correlation, base64/JSON/protobuf framing, handshakes
   (OpenAI `realtime` subprotocol, ElevenLabs ping→pong, Gemini setup), and per-vendor barge-in signals are
   handled for you.

2. **Turn-taking that works with *any* agent.** Module-side VAD (`switch_vad`) emits
   `speech_started` / `speech_stopped`, opens a "ready gate" so callers never answer into silence,
   and does **speech-triggered barge-in** — even for an agent that does no endpointing itself.

3. **The agent can drive the call.** An opt-in, whitelisted control channel turns LLM tool calls into
   real telephony: `transfer`, `hangup`, `send_dtmf`, `play`, `record`, `hold`, `bridge`, `setvar` —
   audited via `earshot::command`. A dumb audio pipe becomes an AI-controllable endpoint.

4. **PCI/PII masking a SaaS charges dearly for.** A masking window mutes caller audio and redacts DTMF
   to the agent during card entry, while digits still reach a secure collector. Self-hosted, on your
   box, no per-minute compliance surcharge.

5. **One tap, many sinks.** Multi-stream fan-out: run the agent + a live transcription fork + a
   supervisor monitor + compliance capture on the same call at once (`dir=in` read-only forks).

6. **Latency you can see.** Built-in KPIs in every metrics event: time-to-first-audio, per-turn
   response latency, and WebSocket RTT — the numbers voice teams actually optimize.

7. **Any sample rate.** Transparent resampling (8k ↔ 16k ↔ 24k, incl. asymmetric like Gemini's
   16k-in/24k-out) via FreeSWITCH's bundled resampler — no extra dependency.

8. **Built to stay up.** Auto-reconnect with jittered backoff, a bounded outbound queue (buffers a
   blip, drops a long outage instead of replaying stale audio), and one clean teardown path.

9. **Observable and traceable.** `earshot::metrics` events (periodic / on-close / on-demand JSON),
   lifecycle + DTMF + command + speech events over the FreeSWITCH event socket, and correlation by
   SIP Call-ID + channel UUID on the handshake.

10. **Drop-in migration.** A `uuid_audio_stream` / `audio_stream` compat shim runs existing
    `mod_audio_stream` dialplans unmodified (and refuses to register if that module is still loaded).

11. **MIT, tested, uncapped.** Portable core (codec, protobuf, backpressure queue) is **unit-tested**
    off-box — including adversarial protobuf input caught in peer review — and the core call path is
    exercised end-to-end with `sipp` + real audio.

## What Earshot is *not*

A thin, reliable FreeSWITCH-side tap — not an agent framework, STT/TTS engine, media server/SFU, or
IVR engine. It feeds pipecat / LiveKit / your own agent; it doesn't replace them. Staying sharp is the
point.

## Positioning in one line

> The self-hosted answer to Twilio ConversationRelay's feature set — matched, then extended with the
> things only an on-prem module can do: your own models, PCI masking, fan-out, and agent-driven call
> control — on the FreeSWITCH stack you already run.
