#!/usr/bin/env python3
"""
mock_agent — a stand-in AI agent for developing/testing mod_earshot.

Emulates the *server* side of each wire protocol (choose with --proto), so the
module's framing can be validated over a real SIP call without vendor keys:

  native   : binary audio in/out (+ {"type":"playAudio",...} / {"type":"clear"})
  twilio   : Twilio Media Streams — connected/start/media/mark JSON, base64 mu-law
  openai   : OpenAI Realtime — session.update, input_audio_buffer.append, response.audio.delta
  deepgram : Deepgram Voice Agent — Settings, raw binary audio
  elevenlabs: ElevenLabs Conversational AI — user_audio_chunk in, {type:audio} out, ping->pong
  gemini   : Gemini Live — setup, realtimeInput.mediaChunks in, serverContent.modelTurn out (24k)
  pipecat  : Pipecat protobuf Frame{ audio: AudioRawFrame } both ways

Modes:  --echo   --speak (send a 440 Hz tone)   --command '<json>'   --silent (default)

Requires:  pip install websockets
Run:       python3 -u test/mock_agent.py --port 9099 --proto elevenlabs --speak
"""
import argparse, asyncio, audioop, base64, json, math, time

try:
    import websockets
except ImportError:
    raise SystemExit("mock_agent needs the 'websockets' package: pip install websockets")


def tone_l16(phase, rate, freq=440):
    """One 20 ms frame of a 440 Hz tone at `rate`, as raw 16-bit LE PCM bytes."""
    fpb = max(1, rate // 50)
    pcm = bytearray()
    for i in range(fpb):
        v = int(8000 * math.sin(2 * math.pi * freq * (phase + i) / rate))
        pcm += int(v).to_bytes(2, "little", signed=True)
    return bytes(pcm), phase + fpb


def measure(recv_max, raw, fmt):
    try:
        lin = raw if fmt == "l16" else audioop.ulaw2lin(raw, 2)
        return max(recv_max, audioop.max(lin, 2))
    except Exception:
        return recv_max


# ---- minimal protobuf (Pipecat Frame{ audio: AudioRawFrame }) ----
def pb_varint(v):
    out = bytearray()
    while True:
        b = v & 0x7f; v >>= 7
        out.append(b | 0x80 if v else b)
        if not v:
            return bytes(out)


def pb_read_varint(d, i):
    v = shift = 0
    while i < len(d):
        b = d[i]; i += 1; v |= (b & 0x7f) << shift
        if not (b & 0x80):
            return v, i
        shift += 7
    return None, i


def pb_encode_audio(audio, rate):
    inner = (bytes([(3 << 3) | 2]) + pb_varint(len(audio)) + audio +
             bytes([(4 << 3) | 0]) + pb_varint(rate) +
             bytes([(5 << 3) | 0]) + pb_varint(1))
    return bytes([(2 << 3) | 2]) + pb_varint(len(inner)) + inner


def pb_decode_audio(d):
    i = 0
    while i < len(d):
        tag, i = pb_read_varint(d, i)
        if tag is None:
            return None
        field, wt = tag >> 3, tag & 7
        if wt == 2:
            flen, i = pb_read_varint(d, i)
            if flen is None or i + flen > len(d):
                return None
            if field == 2:
                inner, j = d[i:i + flen], 0
                while j < len(inner):
                    itag, j = pb_read_varint(inner, j)
                    if itag is None:
                        break
                    ifield, iwt = itag >> 3, itag & 7
                    if iwt == 2:
                        ilen, j = pb_read_varint(inner, j)
                        if ilen is None or j + ilen > len(inner):
                            break
                        if ifield == 3:
                            return bytes(inner[j:j + ilen])
                        j += ilen
                    elif iwt == 0:
                        _, j = pb_read_varint(inner, j)
                    else:
                        break
            i += flen
        elif wt == 0:
            _, i = pb_read_varint(d, i)
        else:
            return None
    return None


async def speak(ws, args, ctx):
    """Continuously send a 440 Hz tone (agent -> caller), framed for the protocol."""
    phase = frames = 0
    try:
        while True:
            if args.proto in ("twilio", "openai", "elevenlabs"):        # base64 mu-law @ 8k
                l16, phase = tone_l16(phase, 8000)
                b64 = base64.b64encode(audioop.lin2ulaw(l16, 2)).decode()
                if args.proto == "twilio":
                    await ws.send(json.dumps({"event": "media", "streamSid": ctx.get("sid") or "MZmock",
                                              "media": {"payload": b64}}))
                elif args.proto == "openai":
                    await ws.send(json.dumps({"type": "response.audio.delta", "delta": b64}))
                else:  # elevenlabs
                    await ws.send(json.dumps({"type": "audio", "audio_event": {"audio_base_64": b64}}))
            elif args.proto == "gemini":                                # L16 @ 24k, base64
                l16, phase = tone_l16(phase, 24000)
                b64 = base64.b64encode(l16).decode()
                await ws.send(json.dumps({"serverContent": {"modelTurn": {"parts": [
                    {"inlineData": {"mimeType": "audio/pcm;rate=24000", "data": b64}}]}}}))
            elif args.proto == "pipecat":                               # protobuf L16 @ 16k
                l16, phase = tone_l16(phase, 16000)
                await ws.send(pb_encode_audio(l16, 16000))
            else:                                                       # native / deepgram: raw binary
                l16, phase = tone_l16(phase, args.rate)
                await ws.send(l16 if args.fmt == "l16" else audioop.lin2ulaw(l16, 2))
            frames += 1
            if args.command and frames == 100:
                await ws.send(args.command); print(f"    -> sent command: {args.command}")
            if args.proto == "elevenlabs" and frames == 50:             # exercise ping->pong keepalive
                await ws.send(json.dumps({"type": "ping", "ping_event": {"event_id": 42, "ping_ms": 0}}))
                print("    -> sent ping event_id=42 (expect pong back)")
            if args.proto == "twilio" and frames == 100:
                await ws.send(json.dumps({"event": "mark", "streamSid": ctx.get("sid") or "MZmock",
                                          "mark": {"name": "utterance-1"}}))
                print("    -> sent mark 'utterance-1'")
            if args.proto == "twilio" and frames >= 130:
                print("    -> end of utterance burst"); return
            await asyncio.sleep(0.02)
    except Exception:
        pass


async def handle(ws, args):
    peer = getattr(ws, "remote_address", ("?", 0))
    frames = bytes_in = recv_max = 0
    t0 = time.time()
    hdrs = getattr(ws, "request_headers", {})
    corr = hdrs.get("X-Call-ID") or hdrs.get("X-Correlation-ID") or "-"
    print(f"[+] connected {peer}  proto={args.proto} fmt={args.fmt}/{args.rate}  Call-ID={corr}")

    ctx, speaker = {}, None

    async def maybe_speak():
        nonlocal speaker
        if args.speak and not speaker:
            speaker = asyncio.create_task(speak(ws, args, ctx))

    if args.proto in ("native", "elevenlabs", "pipecat"):
        await maybe_speak()

    try:
        async for msg in ws:
            if isinstance(msg, (bytes, bytearray)):
                if args.proto == "pipecat":                             # protobuf audio (caller->agent)
                    raw = pb_decode_audio(bytes(msg))
                    if raw:
                        frames += 1; bytes_in += len(raw); recv_max = measure(recv_max, raw, "l16")
                else:                                                   # native / deepgram binary
                    frames += 1; bytes_in += len(msg); recv_max = measure(recv_max, bytes(msg), args.fmt)
                    if args.echo:
                        await ws.send(msg)
                continue
            try:
                obj = json.loads(msg)
            except json.JSONDecodeError:
                print(f"    text: {msg[:120]}"); continue
            typ = obj.get("event") or obj.get("type")

            # --- twilio ---
            if typ == "connected":
                print("    twilio: connected")
            elif typ == "start":
                start = obj.get("start", {}); ctx["sid"] = obj.get("streamSid") or start.get("streamSid")
                print(f"    twilio: start streamSid={ctx['sid']} params={start.get('customParameters', {})}")
                await maybe_speak()
            elif typ == "media":
                payload = (obj.get("media") or {}).get("payload") or obj.get("payload", "")
                if payload:
                    raw = base64.b64decode(payload); frames += 1; bytes_in += len(raw)
                    recv_max = measure(recv_max, raw, "ulaw")
            elif typ == "mark":
                print(f"    <- MARK echoed back: {json.dumps(obj.get('mark', obj))[:100]}")
            elif typ == "command_result":
                print(f"    <- COMMAND RESULT: {json.dumps(obj)[:160]}")
            # --- openai ---
            elif typ == "session.update":
                print(f"    openai: session.update {json.dumps(obj.get('session', {}))[:100]}")
                await maybe_speak()
            elif typ == "input_audio_buffer.append":
                payload = obj.get("audio", "")
                if payload:
                    raw = base64.b64decode(payload); frames += 1; bytes_in += len(raw)
                    recv_max = measure(recv_max, raw, "ulaw" if args.fmt != "l16" else "l16")
            # --- deepgram ---
            elif typ == "Settings":
                print(f"    deepgram: Settings audio={obj.get('audio')}")
                await maybe_speak()
            # --- elevenlabs ---
            elif "user_audio_chunk" in obj:
                raw = base64.b64decode(obj["user_audio_chunk"]); frames += 1; bytes_in += len(raw)
                recv_max = measure(recv_max, raw, "ulaw")
            elif typ == "pong":
                print(f"    <- PONG received: event_id={obj.get('event_id')}")
            # --- gemini ---
            elif "setup" in obj:
                print(f"    gemini: setup {json.dumps(obj['setup'])[:100]}")
                await ws.send(json.dumps({"setupComplete": {}}))
                await maybe_speak()
            elif "realtimeInput" in obj:
                for ch in obj["realtimeInput"].get("mediaChunks", []):
                    raw = base64.b64decode(ch.get("data", "")); frames += 1; bytes_in += len(raw)
                    recv_max = measure(recv_max, raw, "l16")
            else:
                print(f"    control: {typ} {json.dumps(obj)[:120]}")

            if frames and frames % 250 == 0:
                print(f"    {frames} frames, {bytes_in} bytes, recv_max={recv_max}")
    except websockets.ConnectionClosed:
        pass
    finally:
        if speaker:
            speaker.cancel()
        dur = max(time.time() - t0, 1e-6)
        print(f"[-] closed {peer}  {frames} frames / {bytes_in} bytes in {dur:.1f}s  recv_max={recv_max}")


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=9099)
    ap.add_argument("--proto", default="native",
                    choices=["native", "twilio", "openai", "deepgram", "elevenlabs", "gemini", "pipecat"])
    ap.add_argument("--fmt", choices=["ulaw", "l16"], default="ulaw", help="wire format for binary protos")
    ap.add_argument("--rate", type=int, default=8000, help="wire sample rate for binary protos")
    ap.add_argument("--echo", action="store_true", help="loop received audio back to the caller")
    ap.add_argument("--speak", action="store_true", help="send a generated tone (agent->caller)")
    ap.add_argument("--command", default=None, help="a control-channel JSON to send mid-call")
    args = ap.parse_args()
    print(f"mock_agent listening on ws://{args.host}:{args.port}  (proto={args.proto} speak={args.speak})")
    async with websockets.serve(lambda ws, *_: handle(ws, args), args.host, args.port, max_size=None):
        await asyncio.Future()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
