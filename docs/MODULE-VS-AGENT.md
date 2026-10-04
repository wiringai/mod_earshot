# Module side or agent side? VAD, barge-in and latency

Earshot can detect caller speech, interrupt the agent and measure latency inside
FreeSWITCH. Agent frameworks such as pipecat, LiveKit Agents or the OpenAI Realtime
API do some of the same things inside the agent. This page explains what each side
actually does, why Earshot has these features at all, and which switch to set for
which kind of agent. Nothing here is on by default: a stream started without
`vad=` or `metrics=` behaves as a plain audio bridge.

## What Earshot's VAD is, and is not

`vad=on` runs FreeSWITCH's built-in `switch_vad` on the caller's audio in the media
tap. It is an **energy VAD with hangover**: speech held for `vad_voice_ms` fires
`speech_started`, silence held for `vad_silence_ms` fires `speech_stopped`. That is
**endpointing**, not turn prediction. There is no model that decides whether the
caller has finished a thought; a pause longer than the silence budget is the end of
the turn. Where the docs say "turn", read "VAD endpoint".

Three things hang off it:

| Switch | What happens | Who needs it |
|---|---|---|
| `vad=on` | `earshot::speech_started` / `speech_stopped` events, `earshot_talking` variable | Anyone who wants a "caller is speaking" signal on the event socket |
| `vad_notify=on` | the same transitions sent to the agent as JSON | Agents with no endpointing of their own |
| `interruptible=speech` (`vad_barge=on`) | queued agent playback is flushed the moment the caller speaks | Agents with no interruption logic of their own |

The per-turn response latency in `earshot::metrics` also needs the VAD, because it
starts its clock at `speech_stopped`.

## Why it is in the module at all

Most agents on the other end of a WebSocket are not a full voice framework. A raw
STT → LLM → TTS loop, a Twilio-protocol server, a transcription sink or an in-house
prototype has no VAD, no endpointing and no idea when to stop talking. For those
agents the module is the only place turn boundaries can come from, and module-side
barge-in has one property no agent can match: it cuts playback **within one 20 ms
frame** of detected speech, with no network round trip. The price is a crude
detector, which is why `barge_min_ms` (sustained speech before a barge) and
`barge_fade_ms` (fade instead of a hard cut) exist.

## When to leave it off

If the agent already runs its own VAD and interruption logic, turn Earshot's off.
Pipecat with Silero VAD and a turn model, the OpenAI Realtime API with server VAD,
Deepgram Voice Agent, Vapi and ElevenLabs all detect speech on their side and tell
Earshot to interrupt (a `clear` / `flush` on the control channel, or the vendor's
own barge signal, which the protocol adapters translate). Running two detectors
means two opinions about when the caller spoke, and the cruder, faster one wins on
timing: Earshot would cut the agent's audio on a cough that Silero would have
ignored. One detector per call. For these agents the right stream options are no
`vad=`, barge-in driven by the agent, which is also the default.

## Latency: the two sides measure different things

Agent-side metrics (pipecat's `MetricsFrame`s, your own observers) measure inside
the agent process: LLM time to first token, TTS time to first byte, per-processor
time. They are the right tool for finding **which stage** is slow.

They cannot see the wire. Nothing inside the agent knows when its audio actually
reached FreeSWITCH, how long the WebSocket connect took, whether frames were
dropped, or that a cold-started container held the handshake for a minute while
the caller heard ringback. Earshot measures from the edge, which is what the
caller experiences:

- **time-to-first-audio**: stream start → first agent audio frame arrives, including
  connect, handshake and any queuing in front of the agent;
- **per-turn response time**: caller stops → agent audio starts (needs `vad=on`);
- **WebSocket RTT**, frame counts, queue drops, reconnects.

These are not a replacement for agent-side metrics; they are the other half. The
agent tells you why a turn was slow, the module tells you whether the caller felt
it. Enable `metrics=<seconds>` even when the VAD is off: time-to-first-audio, RTT
and drops need nothing else.

## Recommended settings by agent type

| Agent | `vad=` | Barge-in | `metrics=` |
|---|---|---|---|
| pipecat, LiveKit Agents, any framework with its own VAD | off | agent sends `clear`/`flush` | on |
| OpenAI Realtime, Deepgram Voice Agent, Vapi, ElevenLabs, Gemini Live | off | vendor signal, translated by the adapter | on |
| Raw STT/LLM/TTS loop, Twilio-protocol server, prototype | `vad=on vad_notify=on` | `interruptible=speech barge_min_ms=…` | on |
| Transcription or supervisor fork (`dir=in`) | `vad=on` if you want speech events | n/a | optional |
