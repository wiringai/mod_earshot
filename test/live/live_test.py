#!/usr/bin/env python3
"""Live calls through mod_earshot on a real FreeSWITCH.

Runs inside the rig container (see run.sh). FreeSWITCH calls itself over SIP: the
calling leg plays a 300 Hz tone, the called leg is parked and carries the stream.
This process is the agent (a WebSocket server), the operator (the event socket)
and the judge. Nothing here is mocked on the FreeSWITCH side.

    python3 live_test.py                 every scenario
    python3 live_test.py basic twilio    only these
    python3 live_test.py --list

Exit status: 0 when every check passed, 1 otherwise, 2 when the rig is not usable.
"""
import argparse
import asyncio
import audioop
import base64
import hashlib
import json
import math
import os
import random
import sys
import time
import urllib.parse
import uuid
import wave

import websockets

FS_PASSWORD = os.environ.get("FS_PASSWORD", "JambonzR0ck$")
FS_PID_FILE = "/usr/local/freeswitch/run/freeswitch.pid"
FS_LOG = "/usr/local/freeswitch/log/freeswitch.log"
AGENT_PORT = 9100      # the agent
BLACKHOLE_PORT = 9101  # accepts the TCP connection, never answers the handshake
MUTE_PORT = 9102       # completes the handshake, then never sends or answers anything
DEAD_PORT = 9199       # nothing listens here
CALLER_HZ = 300
AGENT_HZ = 440

EVENTS = ("CHANNEL_HANGUP_COMPLETE CUSTOM earshot::connected earshot::reconnected "
          "earshot::disconnected earshot::error earshot::metrics earshot::ready "
          "earshot::command earshot::dtmf")


# ----------------------------------------------------------------------------
# the event socket
# ----------------------------------------------------------------------------
class ESL:
    def __init__(self):
        self.events = []
        self._replies = asyncio.Queue()
        self._lock = asyncio.Lock()
        self._new = asyncio.Condition()
        self.reader = self.writer = self._task = None

    async def connect(self, subscribe=True):
        self.reader, self.writer = await asyncio.open_connection("127.0.0.1", 8021)
        await self._read_message()                       # auth/request
        self.writer.write(f"auth {FS_PASSWORD}\n\n".encode())
        hdr, _ = await self._read_message()
        if "+OK" not in hdr.get("Reply-Text", ""):
            raise RuntimeError("event socket refused the password")
        self._task = asyncio.create_task(self._pump())
        if subscribe:
            await self._command(f"event plain {EVENTS}")
        return self

    async def _read_message(self):
        hdr = {}
        while True:
            line = await self.reader.readline()
            if not line:
                raise ConnectionError("event socket closed")
            line = line.decode(errors="replace").rstrip("\n")
            if not line:
                if hdr:
                    break
                continue
            k, _, v = line.partition(": ")
            hdr[k] = v
        body = b""
        if "Content-Length" in hdr:
            body = await self.reader.readexactly(int(hdr["Content-Length"]))
        return hdr, body.decode(errors="replace")

    async def _pump(self):
        try:
            while True:
                hdr, body = await self._read_message()
                ctype = hdr.get("Content-Type", "")
                if ctype == "text/event-plain":
                    ev = {}
                    for line in body.split("\n"):
                        if not line:
                            break
                        k, _, v = line.partition(": ")
                        ev[k] = urllib.parse.unquote(v)
                    ev["_t"] = time.monotonic()
                    self.events.append(ev)
                    async with self._new:
                        self._new.notify_all()
                elif ctype in ("api/response", "command/reply"):
                    self._replies.put_nowait(body if ctype == "api/response" else hdr.get("Reply-Text", ""))
        except (ConnectionError, asyncio.IncompleteReadError, asyncio.CancelledError):
            self._replies.put_nowait("-ERR event socket closed")

    async def _command(self, line, timeout=30):
        async with self._lock:
            self.writer.write((line + "\n\n").encode())
            await self.writer.drain()
            return (await asyncio.wait_for(self._replies.get(), timeout)).strip()

    async def api(self, cmd, timeout=30):
        return await self._command("api " + cmd, timeout)

    def mark(self):
        return len(self.events)

    def find(self, since, name=None, **want):
        """Events after `since` with this name (Event-Subclass or Event-Name) and these headers."""
        out = []
        for ev in self.events[since:]:
            if name and name not in (ev.get("Event-Subclass"), ev.get("Event-Name")):
                continue
            if all(ev.get(k) == v for k, v in want.items()):
                out.append(ev)
        return out

    async def wait(self, since, name=None, timeout=10, count=1, **want):
        deadline = time.monotonic() + timeout
        while True:
            got = self.find(since, name, **want)
            if len(got) >= count:
                return got
            left = deadline - time.monotonic()
            if left <= 0:
                return got
            async with self._new:
                try:
                    await asyncio.wait_for(self._new.wait(), left)
                except asyncio.TimeoutError:
                    pass

    async def close(self):
        if self._task:
            self._task.cancel()
        if self.writer:
            self.writer.close()


# ----------------------------------------------------------------------------
# the agent
# ----------------------------------------------------------------------------
def tone(hz, ms, rate=8000, phase=0):
    n = rate * ms // 1000
    pcm = b"".join(int(9000 * math.sin(2 * math.pi * hz * (phase + i) / rate)).to_bytes(2, "little", signed=True)
                   for i in range(n))
    return pcm, phase + n


def dominant_hz(pcm, rate=8000):
    """Frequency of a clean tone from its zero crossings; 0 for silence."""
    n = len(pcm) // 2
    if n < rate // 10 or audioop.rms(pcm, 2) < 300:
        return 0
    crossings, prev = 0, 0
    for i in range(n):
        s = int.from_bytes(pcm[2 * i:2 * i + 2], "little", signed=True)
        if (s >= 0) != (prev >= 0):
            crossings += 1
        prev = s
    return crossings * rate / (2.0 * n)


class Conn:
    """What the agent saw on one WebSocket connection."""

    def __init__(self, ws, path):
        self.ws, self.path = ws, path
        self.headers = {k.lower(): v for k, v in ws.request_headers.raw_items()}
        self.opened = time.monotonic()
        self.texts = []          # (monotonic time, parsed json or raw str)
        self.frames = 0
        self.frame_times = []
        self.pcm = bytearray()   # caller audio, decoded to 16-bit
        self.close_code = None
        self.closed_at = None
        self.closed = asyncio.Event()

    def json_texts(self, **want):
        return [m for _, m in self.texts if isinstance(m, dict) and all(m.get(k) == v for k, v in want.items())]

    async def wait_text(self, timeout=5, **want):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            got = self.json_texts(**want)
            if got:
                return got[0]
            await asyncio.sleep(0.02)
        return None

    async def wait_closed(self, timeout=5):
        try:
            await asyncio.wait_for(self.closed.wait(), timeout)
            return True
        except asyncio.TimeoutError:
            return False

    def heard_hz(self, last_ms=1000):
        return dominant_hz(bytes(self.pcm[-16 * last_ms:]))


class Agent:
    def __init__(self):
        self.conns = []
        self.behaviours = {}     # path prefix -> async fn(conn)
        self._new = asyncio.Condition()

    def on(self, prefix, fn):
        self.behaviours[prefix] = fn

    async def start(self):
        self.server = await websockets.serve(self._handle, "127.0.0.1", AGENT_PORT, max_size=None,
                                             ping_interval=None)

    async def _handle(self, ws, path=None):
        path = path or ws.path
        conn = Conn(ws, path)
        self.conns.append(conn)
        async with self._new:
            self._new.notify_all()
        task = None
        for prefix, fn in self.behaviours.items():
            if path.startswith(prefix):
                task = asyncio.create_task(fn(conn))
                break
        twilio = "twilio" in path
        try:
            async for msg in ws:
                now = time.monotonic()
                if isinstance(msg, (bytes, bytearray)):
                    conn.frames += 1
                    conn.frame_times.append(now)
                    conn.pcm += audioop.ulaw2lin(bytes(msg), 2)
                    continue
                try:
                    obj = json.loads(msg)
                except ValueError:
                    obj = msg
                conn.texts.append((now, obj))
                if twilio and isinstance(obj, dict) and obj.get("event") == "media":
                    conn.frames += 1
                    conn.frame_times.append(now)
                    conn.pcm += audioop.ulaw2lin(base64.b64decode(obj["media"]["payload"]), 2)
        except websockets.ConnectionClosed:
            pass
        finally:
            conn.close_code = ws.close_code
            conn.closed_at = time.monotonic()
            conn.closed.set()
            if task:
                task.cancel()

    async def wait_conn(self, path, timeout=5, nth=1):
        deadline = time.monotonic() + timeout
        while True:
            got = [c for c in self.conns if c.path == path]
            if len(got) >= nth:
                return got[nth - 1]
            left = deadline - time.monotonic()
            if left <= 0:
                return None
            async with self._new:
                try:
                    await asyncio.wait_for(self._new.wait(), left)
                except asyncio.TimeoutError:
                    pass


async def speak(conn, ms=2000, hz=AGENT_HZ, realtime=True):
    """The agent talks: `ms` of a tone as mu-law, in 20 ms frames."""
    phase = 0
    t0 = time.monotonic()
    try:
        for i in range(ms // 20):
            pcm, phase = tone(hz, 20, phase=phase)
            await conn.ws.send(audioop.lin2ulaw(pcm, 2))
            if realtime:      # paced against the clock, 100 ms ahead: a sleep per frame runs slow
                await asyncio.sleep(max(0.0, t0 + (i + 1) * 0.02 - 0.1 - time.monotonic()))
    except websockets.ConnectionClosed:
        pass


async def blackhole(reader, writer):
    """Accept the TCP connection and say nothing, for ever."""
    try:
        while await reader.read(4096):
            pass
    except ConnectionError:
        pass


class MuteServer:
    """Completes the WebSocket handshake, then never sends or answers anything (no pong)."""

    def __init__(self):
        self.sessions = []   # [opened, closed or None]

    async def handle(self, reader, writer):
        rec = [time.monotonic(), None]
        try:
            request = await reader.readuntil(b"\r\n\r\n")
            key = [ln.split(b":", 1)[1].strip() for ln in request.split(b"\r\n")
                   if ln.lower().startswith(b"sec-websocket-key")][0]
            accept = base64.b64encode(hashlib.sha1(key + b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest())
            writer.write(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                         b"Sec-WebSocket-Accept: " + accept + b"\r\n\r\n")
            await writer.drain()
            self.sessions.append(rec)
            while await reader.read(4096):
                pass
        except (ConnectionError, asyncio.IncompleteReadError, IndexError):
            pass
        finally:
            rec[1] = time.monotonic()
            writer.close()


# ----------------------------------------------------------------------------
# the rig
# ----------------------------------------------------------------------------
class Rig:
    def __init__(self):
        self.esl = ESL()
        self.agent = Agent()
        self.mute = MuteServer()
        self.results = []   # (scenario, check, passed, detail)
        self.scenario = ""
        self.ip = ""
        self.pid = 0
        self.seq = 0

    async def start(self):
        await self.esl.connect()
        await self.agent.start()
        await asyncio.start_server(blackhole, "127.0.0.1", BLACKHOLE_PORT)
        await asyncio.start_server(self.mute.handle, "127.0.0.1", MUTE_PORT)
        self.ip = (await self.esl.api("global_getvar local_ip_v4")).strip()
        self.pid = self.fs_pid()
        if "true" not in await self.esl.api("module_exists mod_earshot"):
            raise RuntimeError("mod_earshot is not loaded")

    @staticmethod
    def fs_pid():
        try:
            pid = int(open(FS_PID_FILE).read().strip())
            os.kill(pid, 0)
            return pid
        except (OSError, ValueError):
            return 0

    def rss_kb(self):
        try:
            for line in open(f"/proc/{self.pid}/status"):
                if line.startswith("VmRSS"):
                    return int(line.split()[1])
        except OSError:
            pass
        return 0

    def threads(self):
        try:
            return len(os.listdir(f"/proc/{self.pid}/task"))
        except OSError:
            return 0

    def check(self, name, passed, detail=""):
        self.results.append((self.scenario, name, bool(passed), detail))
        mark = "\033[32mPASS\033[0m" if passed else "\033[31mFAIL\033[0m"
        print(f"  {mark} {name}" + (f"  [{detail}]" if detail and not passed else ""), flush=True)
        return bool(passed)

    def note(self, text):
        print(f"       {text}", flush=True)

    async def call(self, dest="park", record=False):
        """Place a call to ourselves. Returns (calling leg, called leg, recording path)."""
        self.seq += 1
        a = str(uuid.uuid4())
        cid = f"7{self.seq:06d}"
        rec = f"/tmp/rig-{cid}.wav" if record else None
        vars_ = f"origination_uuid={a},origination_caller_id_number={cid},RECORD_READ_ONLY=true"
        r = await self.esl.api(f"originate {{{vars_}}}sofia/drachtio_mrf/{dest}@{self.ip}:5080 "
                               f"&playback(tone_stream://%(60000,0,{CALLER_HZ});loops=-1)")
        if not r.startswith("+OK"):
            raise RuntimeError(f"originate failed: {r}")
        b = None
        for _ in range(50):
            rows = json.loads(await self.esl.api("show channels as json")).get("rows", [])
            hit = [x for x in rows if x.get("cid_num") == cid and x.get("direction") == "inbound"]
            if hit:
                b = hit[0]["uuid"]
                break
            await asyncio.sleep(0.05)
        if not b:
            raise RuntimeError("the called leg never appeared")
        if record:
            await self.esl.api(f"uuid_record {a} start {rec}")
        return a, b, rec

    async def hangup(self, *uuids):
        for u in uuids:
            await self.esl.api(f"uuid_kill {u}")

    async def channels(self):
        r = await self.esl.api("show channels count")
        try:
            return int(r.split()[0])
        except (ValueError, IndexError):
            return -1

    async def settle(self):
        """After a scenario: everything hung up, FreeSWITCH still the same process."""
        await self.esl.api("hupall")
        for _ in range(60):
            if await self.channels() == 0:
                break
            await asyncio.sleep(0.1)
        n = await self.channels()
        alive = self.fs_pid() == self.pid and self.pid != 0
        self.check("FreeSWITCH is still the same process and no channel is left", alive and n == 0,
                   f"pid {self.pid}->{self.fs_pid()}, channels {n}")
        return alive

    async def getvar(self, u, name):
        return (await self.esl.api(f"uuid_getvar {u} {name}")).strip()


def url(path):
    return f"ws://127.0.0.1:{AGENT_PORT}{path}"


def heard_agent(rec):
    """Longest stretch, in ms, of the agent's tone in what the calling leg received."""
    try:
        w = wave.open(rec, "rb")
        pcm = w.readframes(w.getnframes())
        rate = w.getframerate()
        w.close()
    except (OSError, wave.Error, EOFError):
        return -1
    best = run = 0
    step = rate // 10 * 2
    for i in range(0, len(pcm) - step + 1, step):
        hz = dominant_hz(pcm[i:i + step], rate)
        run = run + 100 if abs(hz - AGENT_HZ) < 25 else 0
        best = max(best, run)
    return best


# ----------------------------------------------------------------------------
# scenarios
# ----------------------------------------------------------------------------
async def s_basic(rig):
    """One ordinary call: both directions of audio, the events, a stop that closes cleanly."""
    agent, esl = rig.agent, rig.esl
    agent.on("/basic", lambda c: speak(c, 2500))
    a, b, rec = await rig.call(record=True)
    m = esl.mark()
    r = await esl.api(f"earshot {b} start {url('/basic')} dtmf=on metrics=1")
    rig.check("start is accepted", r.startswith("+OK"), r)
    conn = await agent.wait_conn("/basic")
    if not rig.check("the agent receives a WebSocket connection", conn is not None):
        return
    rig.check("the handshake names the call (X-Channel-UUID is the called leg)",
              conn.headers.get("x-channel-uuid") == b and bool(conn.headers.get("x-call-id")),
              str({k: v for k, v in conn.headers.items() if k.startswith("x-")}))
    ev = await esl.wait(m, "earshot::connected", **{"Unique-ID": b})
    rig.check("earshot::connected fires, after the agent's handshake",
              len(ev) == 1 and ev[0]["_t"] >= conn.opened, f"{len(ev)} event(s)")
    await asyncio.sleep(3.2)
    fps = conn.frames / max(time.monotonic() - conn.opened, 0.1)
    rig.check("the agent receives the caller's audio at about 50 frames a second", 42 <= fps <= 58, f"{fps:.1f} fps")
    hz = conn.heard_hz()
    rig.check(f"what the agent hears is the caller's {CALLER_HZ} Hz tone", abs(hz - CALLER_HZ) < 20, f"{hz:.0f} Hz")
    st = await esl.api(f"earshot {b} status")
    rig.check("status reports a connected stream", '"ws_connected":1' in st, st)
    rig.check("earshot::ready fired when the agent first spoke", len(esl.find(m, "earshot::ready")) == 1)
    rig.check("periodic earshot::metrics events arrive", len(esl.find(m, "earshot::metrics")) >= 2,
              f"{len(esl.find(m, 'earshot::metrics'))}")
    await esl.api(f"uuid_send_dtmf {a} 5")
    ev = await esl.wait(m, "earshot::dtmf", timeout=3, digit="5")
    rig.check("a digit the caller presses raises earshot::dtmf", len(ev) == 1)
    rig.check("and reaches the agent", await conn.wait_text(2, type="dtmf") is not None
              or any("5" in json.dumps(t) for _, t in conn.texts), str(conn.texts[-2:]))
    r = await esl.api(f"earshot {b} send hello-agent")
    rig.check("send delivers a text message to the agent",
              r.startswith("+OK") and any(t == "hello-agent" for _, t in await _texts_after(conn, 1)), r)
    t0 = time.monotonic()
    r = await esl.api(f"earshot {b} stop")
    took = time.monotonic() - t0
    rig.check("stop is accepted and returns promptly", r.startswith("+OK") and took < 1.0, f"{r} in {took:.2f}s")
    await conn.wait_closed(3)
    rig.check("the agent sees a normal close (code 1000), not a dropped socket", conn.close_code == 1000,
              f"close code {conn.close_code}")
    r = await esl.api(f"earshot {b} stop")
    rig.check("a second stop is refused, not crashed on", r.startswith("-ERR"), r)
    rig.check("the call itself is still up after the stream stopped", "true" in await esl.api(f"uuid_exists {b}"))
    await esl.api(f"uuid_record {a} stop {rec}")
    ms = heard_agent(rec)
    rig.check(f"the caller heard the agent's {AGENT_HZ} Hz tone for about 2.5 s", 2000 <= ms <= 3000, f"{ms} ms")
    await rig.settle()


async def _texts_after(conn, wait):
    await asyncio.sleep(wait)
    return conn.texts


async def s_hangup(rig):
    """The caller hangs up with the stream running."""
    agent, esl = rig.agent, rig.esl
    for who in ("called leg", "calling leg"):
        path = "/hangup-" + who.split()[0]
        a, b, _ = await rig.call()
        m = esl.mark()
        await esl.api(f"earshot {b} start {url(path)}")
        conn = await agent.wait_conn(path)
        if not rig.check(f"[{who} hangs up] the agent is connected", conn is not None):
            continue
        await asyncio.sleep(1)
        await rig.hangup(b if who == "called leg" else a)
        await conn.wait_closed(3)
        rig.check(f"[{who} hangs up] the agent sees a normal close (1000)", conn.close_code == 1000,
                  f"close code {conn.close_code}")
        ev = await esl.wait(m, "earshot::metrics", timeout=2, **{"Unique-ID": b})
        rig.check(f"[{who} hangs up] a final earshot::metrics event is fired", len(ev) >= 1)
    await rig.settle()


async def s_stop_mid_playback(rig):
    """Stop while the agent is talking and has seconds of audio queued."""
    agent, esl = rig.agent, rig.esl
    agent.on("/midplay", lambda c: speak(c, 20000, realtime=False))   # 20 s of audio at once
    a, b, rec = await rig.call(record=True)
    await esl.api(f"earshot {b} start {url('/midplay')}")
    conn = await agent.wait_conn("/midplay")
    if not rig.check("the agent is connected and talking", conn is not None):
        return
    await asyncio.sleep(1.5)
    st = await esl.api(f"earshot {b} status")
    queued = json.loads(st).get("play_buffered", 0) if st.startswith("{") else 0
    rig.check("seconds of agent audio are queued for the caller", queued > 16000 * 5, st)
    t0 = time.monotonic()
    r = await esl.api(f"earshot {b} stop")
    took = time.monotonic() - t0
    rig.check("stop returns promptly with audio still queued", r.startswith("+OK") and took < 1.0, f"{took:.2f}s")
    await conn.wait_closed(3)
    rig.check("the agent sees a normal close (1000)", conn.close_code == 1000, f"close code {conn.close_code}")
    await asyncio.sleep(1.5)
    await esl.api(f"uuid_record {a} stop {rec}")
    ms = heard_agent(rec)
    rig.check("the caller stops hearing the agent when the stream stops (about 1.5 s heard, not 20)",
              1000 <= ms <= 2200, f"{ms} ms")
    rig.check("the call is still up", "true" in await esl.api(f"uuid_exists {b}"))
    await rig.settle()


async def s_verb_race(rig, rounds=60):
    """Operator commands, a caller's digits and a stop or hangup all at once, many times."""
    esl = rig.esl
    extra = [await ESL().connect(subscribe=False) for _ in range(4)]
    stopper = await ESL().connect(subscribe=False)
    verbs = ["status", "metrics", "flush", "send x", "pause", "resume", "mask on", "mask off"]
    allowed = ("+OK", "{", "-ERR earshot stream", "-ERR no active earshot stream", "-ERR no such session")
    odd = []
    a = b = None
    agent = rig.agent
    agent.on("/race", lambda c: speak(c, 4000))
    for i in range(rounds):
        if not b:
            a, b, _ = await rig.call()
        r = await esl.api(f"earshot {b} start {url('/race')} dtmf=on commands=true")
        if not r.startswith("+OK"):
            odd.append(f"start: {r}")
            await rig.hangup(a)
            b = None
            continue
        running = True

        async def hammer(conn_, k):
            while running:
                v = verbs[(k + random.randrange(len(verbs))) % len(verbs)]
                rr = await conn_.api(f"earshot {b} {v}")
                if not rr.startswith(allowed):
                    odd.append(f"{v}: {rr[:80]}")

        async def digits():
            while running:
                await extra[0].api(f"uuid_send_dtmf {a} 1")
                await asyncio.sleep(0.03)

        tasks = [asyncio.create_task(hammer(c, k)) for k, c in enumerate(extra)]
        tasks.append(asyncio.create_task(digits()))
        await asyncio.sleep(random.uniform(0.08, 0.4))
        if i % 3 == 2:                     # every third round the call ends instead of the stream
            await stopper.api(f"uuid_kill {b}")
            ended = True
        else:
            await stopper.api(f"earshot {b} stop")
            ended = False
        await asyncio.sleep(0.05)
        running = False
        await asyncio.gather(*tasks, return_exceptions=True)
        if ended:
            await rig.hangup(a)
            b = None
        if rig.fs_pid() != rig.pid:
            break
    for c in extra + [stopper]:
        await c.close()
    rig.check(f"{rounds} rounds of commands and digits racing a stop or a hangup: FreeSWITCH survives",
              rig.fs_pid() == rig.pid, f"pid {rig.pid}->{rig.fs_pid()}")
    rig.check("every command got a sane answer (OK, data, or 'closing' / 'no active stream')", not odd,
              "; ".join(odd[:5]))
    still = [c for c in agent.conns if c.path == "/race" and c.close_code not in (1000, None)]
    rig.check("every one of those streams closed with code 1000 at the agent", not still,
              f"{len(still)} closed otherwise: {sorted(set(c.close_code for c in still))}")
    await rig.settle()


async def s_agent_race(rig, rounds=30):
    """The agent floods call-control commands while the stream is being stopped."""
    esl, agent = rig.esl, rig.agent

    async def flood(conn):
        n = 0
        try:
            while True:
                n += 1
                act = ("stop_play", "park", "mask", "send_dtmf", "nonsense")[n % 5]
                await conn.ws.send(json.dumps({"type": "command", "action": act, "id": f"c{n}",
                                               "digits": "1", "state": "off"}))
                await asyncio.sleep(0.002)
        except websockets.ConnectionClosed:
            pass

    agent.on("/flood", flood)
    a, b, _ = await rig.call()
    results = 0
    for i in range(rounds):
        if "true" not in await esl.api(f"uuid_exists {b}"):
            a, b, _ = await rig.call()
        await esl.api(f"earshot {b} start {url('/flood')} commands=true")
        await asyncio.sleep(random.uniform(0.05, 0.3))
        if i % 3 == 2:
            await rig.hangup(b, a)
        else:
            await esl.api(f"earshot {b} stop")
        if rig.fs_pid() != rig.pid:
            break
    await asyncio.sleep(0.5)
    for c in agent.conns:
        if c.path == "/flood":
            results += len(c.json_texts(type="command_result"))
    rig.check(f"{rounds} rounds of an agent flooding commands into a stop or a hangup: FreeSWITCH survives",
              rig.fs_pid() == rig.pid)
    rig.check("the agent's commands were being answered while the stream was up", results > rounds, f"{results} results")
    await rig.settle()


async def _ask(conn, cmd, cid, timeout=4):
    cmd = dict(cmd, type="command", id=cid)
    await conn.ws.send(json.dumps(cmd))
    return await conn.wait_text(timeout, type="command_result", id=cid)


async def s_commands(rig):
    """What an agent's call-control commands may and may not do, with and without the allow-lists."""
    esl, agent = rig.esl, rig.agent
    for stale in ("/usr/local/freeswitch/recordings/rig-relative.wav",
                  "/usr/local/freeswitch/sounds/en/us/callie/rig-relative.wav", "/tmp/rigrec/ok.wav"):
        if os.path.exists(stale):
            os.remove(stale)
    a, b, _ = await rig.call()
    m = esl.mark()
    await esl.api(f"earshot {b} start {url('/cmd-default')} commands=true")
    conn = await agent.wait_conn("/cmd-default")
    if not rig.check("the agent is connected with commands on", conn is not None):
        return

    def refused(res, word):
        return res is not None and res.get("ok") is False and word in res.get("error", "")

    res = await _ask(conn, {"action": "transfer", "to": "landed", "dialplan": "inline"}, "t-inline")
    rig.check("transfer through a dialplan that is not listed is refused", refused(res, "dialplan"), str(res))
    res = await _ask(conn, {"action": "record", "file": "/tmp/rig-abs.wav"}, "r-abs")
    rig.check("record to an absolute path is refused", refused(res, "record path"), str(res))
    rig.check("and no file was created", not os.path.exists("/tmp/rig-abs.wav"))
    res = await _ask(conn, {"action": "play", "file": "http://127.0.0.1:1/a.wav"}, "p-http")
    rig.check("play of a URL is refused", refused(res, "scheme"), str(res))
    res = await _ask(conn, {"action": "record", "file": "shout://127.0.0.1/a.mp3"}, "r-shout")
    rig.check("record to a URL is refused", refused(res, "record path"), str(res))
    res = await _ask(conn, {"action": "setvar", "name": "foo", "value": "bar"}, "sv")
    rig.check("setvar of a variable that is not listed is refused", refused(res, "setvars"), str(res))
    res = await _ask(conn, {"action": "self_destruct"}, "unk")
    rig.check("an unknown action is refused", refused(res, "unsupported"), str(res))
    res = await _ask(conn, {"action": "bridge", "peer_uuid": "00000000-0000-0000-0000-00000000dead"}, "br")
    rig.check("a command FreeSWITCH itself fails (-ERR) is reported as ok:false, with the reason",
              res is not None and res.get("ok") is False and res.get("error"), str(res))
    ev = await esl.wait(m, "earshot::command", timeout=2, action="bridge")
    rig.check("and its earshot::command audit event says ok=false", len(ev) == 1 and ev[0].get("ok") == "false",
              str([e.get("ok") for e in ev]))
    res = await _ask(conn, {"action": "send_dtmf", "digits": "1"}, "dt")
    rig.check("an allowed command (send_dtmf) succeeds", res is not None and res.get("ok") is True, str(res))
    res = await _ask(conn, {"action": "record", "file": "rig-relative.wav"}, "r-rel")
    rig.check("record to a relative path succeeds", res is not None and res.get("ok") is True, str(res))
    await asyncio.sleep(0.5)
    rig.check("the file is under FreeSWITCH's recordings directory, not among its prompts",
              os.path.exists("/usr/local/freeswitch/recordings/rig-relative.wav")
              and not os.path.exists("/usr/local/freeswitch/sounds/en/us/callie/rig-relative.wav"))
    res = await _ask(conn, {"action": "record", "state": "stop", "file": "rig-relative.wav"}, "r-rel-stop")
    rig.check("and the same relative name stops it", res is not None and res.get("ok") is True, str(res))
    res = await _ask(conn, {"action": "transfer", "to": "landed", "dialplan": "XML", "context": "mrf"}, "t-ok", 2)
    await asyncio.sleep(0.5)
    rig.check("transfer through the XML dialplan lands the call on the target extension",
              await rig.getvar(b, "earshot_test_landed") == "true", f"result {res}")
    await rig.hangup(a, b)

    # the same refusals lifted by the operator's lists
    os.makedirs("/tmp/rigrec", exist_ok=True)
    a, b, _ = await rig.call()
    await esl.api(f"earshot {b} start {url('/cmd-lists')} commands=true record_dir=/tmp/rigrec "
                  f"dialplans=XML,enum,inline media_schemes=http")
    conn = await agent.wait_conn("/cmd-lists")
    if rig.check("the agent is connected with the allow-lists set", conn is not None):
        res = await _ask(conn, {"action": "record", "file": "/tmp/rigrec/ok.wav"}, "r-in")
        rig.check("record_dir= allows an absolute path under it", res is not None and res.get("ok") is True, str(res))
        await asyncio.sleep(0.4)
        rig.check("and the recording exists there", os.path.exists("/tmp/rigrec/ok.wav"))
        res = await _ask(conn, {"action": "record", "file": "/tmp/elsewhere/no.wav"}, "r-out")
        rig.check("but still refuses one outside it", refused(res, "record path"), str(res))
        res = await _ask(conn, {"action": "record", "file": "/tmp/rigrec/../escape.wav"}, "r-trav")
        rig.check("and a path that climbs out with ..", res is not None and res.get("ok") is False, str(res))
        res = await _ask(conn, {"action": "play", "file": "http://127.0.0.1:1/a.wav"}, "p-http2")
        rig.check("media_schemes=http lets a play URL through to FreeSWITCH",
                  res is not None and "scheme" not in res.get("error", ""), str(res))
        res = await _ask(conn, {"action": "transfer", "to": "landed", "dialplan": "inline"}, "t-inl", 2)
        rig.check("the inline dialplan stays refused even when the operator lists it", refused(res, "dialplan"), str(res))
        res = await _ask(conn, {"action": "transfer", "to": "landed", "dialplan": "enum"}, "t-enum", 2)
        rig.check("dialplans= lets a transfer through a listed dialplan reach FreeSWITCH",
                  res is None or "dialplan" not in res.get("error", ""), str(res))
    await rig.hangup(a, b)

    a, b, _ = await rig.call()
    await esl.api(f"earshot {b} start {url('/cmd-scoped')} commands=transfer,hangup")
    conn = await agent.wait_conn("/cmd-scoped")
    if rig.check("the agent is connected with commands=transfer,hangup", conn is not None):
        res = await _ask(conn, {"action": "send_dtmf", "digits": "1"}, "dt2")
        rig.check("an action outside the granted list is refused as disabled", refused(res, "disabled"), str(res))
        m = esl.mark()
        await conn.ws.send(json.dumps({"type": "command", "action": "hangup", "id": "hu"}))
        ev = await esl.wait(m, "CHANNEL_HANGUP_COMPLETE", timeout=3, **{"Unique-ID": b})
        rig.check("a granted hangup ends the call", len(ev) == 1)
        await conn.wait_closed(3)
        rig.check("and the agent then sees a normal close (1000)", conn.close_code == 1000, f"{conn.close_code}")
    await rig.settle()


async def s_unreachable(rig):
    """An agent that is not there: no false 'connected', a bounded number of tries, a way out."""
    esl = rig.esl
    a, b, _ = await rig.call()
    m = esl.mark()
    t0 = time.monotonic()
    r = await esl.api(f"earshot {b} start ws://127.0.0.1:{DEAD_PORT}/x reconnect_max=3")
    rig.check("start is accepted (the connection is made in the background)", r.startswith("+OK"), r)
    ev = await esl.wait(m, "earshot::error", timeout=20, reason="gave_up")
    took = time.monotonic() - t0
    rig.check("after reconnect_max=3 failed attempts the stream gives up: earshot::error reason=gave_up",
              len(ev) == 1, f"{len(ev)} after {took:.1f}s")
    rig.note(f"gave up after {took:.1f} s")
    errors = [e for e in esl.find(m, "earshot::error") if e.get("reason") != "gave_up"]
    rig.check("each failed attempt raised its own earshot::error first", len(errors) == 3, f"{len(errors)} errors")
    rig.check("earshot::connected never fired for an agent that was never there",
              not esl.find(m, "earshot::connected"))
    rig.check("the channel variable earshot_failed is true", await rig.getvar(b, "earshot_failed") == "true",
              await rig.getvar(b, "earshot_failed"))
    await asyncio.sleep(3)
    rig.check("it gives up once: no further events after that", len(esl.find(m, "earshot::error", reason="gave_up")) == 1
              and len([e for e in esl.find(m, "earshot::error")]) == 4, f"{len(esl.find(m, 'earshot::error'))} errors")
    r = await esl.api(f"earshot {b} stop")
    rig.check("the given-up stream can still be stopped", r.startswith("+OK"), r)
    await rig.hangup(a)

    a, b, _ = await rig.call()
    m = esl.mark()
    t0 = time.monotonic()
    await esl.api(f"earshot {b} start ws://127.0.0.1:{DEAD_PORT}/x reconnect_deadline=4")
    ev = await esl.wait(m, "earshot::error", timeout=15, reason="gave_up")
    took = time.monotonic() - t0
    rig.check("reconnect_deadline=4 gives up after about 4 s", len(ev) == 1 and 3.5 <= took <= 7.5, f"{took:.1f}s")
    await rig.hangup(a)

    a, b, _ = await rig.call()
    m = esl.mark()
    t0 = time.monotonic()
    await esl.api(f"earshot {b} start ws://127.0.0.1:{BLACKHOLE_PORT}/x connect_timeout=2 reconnect_max=1")
    ev = await esl.wait(m, "earshot::error", timeout=10, reason="connect timeout")
    took = time.monotonic() - t0
    rig.check("an agent that accepts the connection and never answers is abandoned at connect_timeout=2",
              len(ev) == 1 and 1.5 <= took <= 3.5, f"{len(ev)} event(s) at {took:.1f}s")
    ev = await esl.wait(m, "earshot::error", timeout=5, reason="gave_up")
    rig.check("and then given up on (reconnect_max=1)", len(ev) == 1)
    t0 = time.monotonic()
    await rig.hangup(b, a)
    rig.check("hanging up a call whose stream never connected is immediate", time.monotonic() - t0 < 1.0)

    a, b, _ = await rig.call()
    await esl.api(f"earshot {b} start ws://127.0.0.1:{BLACKHOLE_PORT}/x")
    await asyncio.sleep(1)
    t0 = time.monotonic()
    r = await esl.api(f"earshot {b} stop")
    rig.check("stop during a connection attempt that is still waiting returns promptly",
              r.startswith("+OK") and time.monotonic() - t0 < 1.0, f"{r} in {time.monotonic() - t0:.2f}s")
    await rig.hangup(a, b)

    # the dialplan can act on it
    a, b, _ = await rig.call("fallback")
    await asyncio.sleep(7.5)
    rig.check("a dialplan can read earshot_failed after the stream gives up and fall back",
              await rig.getvar(b, "earshot_test_fallback") == "true", await rig.getvar(b, "earshot_test_fallback"))
    await rig.settle()


async def s_reconnect(rig):
    """The agent drops the connection mid-call; the stream comes back and says so."""
    esl, agent = rig.esl, rig.agent
    state = {"n": 0}

    async def drop_first(conn):
        state["n"] += 1
        if state["n"] == 1:
            await asyncio.sleep(1.0)
            await conn.ws.close(code=1011)

    agent.on("/drop", drop_first)
    a, b, _ = await rig.call()
    m = esl.mark()
    await esl.api(f"earshot {b} start {url('/drop')}")
    first = await agent.wait_conn("/drop")
    second = await agent.wait_conn("/drop", timeout=8, nth=2)
    if not rig.check("after the agent drops the connection, the stream connects again", second is not None):
        await rig.settle()
        return
    rig.note(f"back {second.opened - first.closed_at:.2f} s after the drop")
    await asyncio.sleep(1.5)
    rig.check("earshot::connected fired once, for the first connection", len(esl.find(m, "earshot::connected")) == 1)
    rig.check("earshot::reconnected fired once, for the second", len(esl.find(m, "earshot::reconnected")) == 1)
    rig.check("caller audio flows on the new connection", second.frames > 40 and abs(second.heard_hz() - CALLER_HZ) < 20,
              f"{second.frames} frames, {second.heard_hz():.0f} Hz")
    rig.check("the new connection starts with live audio, not a backlog from the gap",
              second.frames < (time.monotonic() - second.opened) * 50 + 15,
              f"{second.frames} frames in {time.monotonic() - second.opened:.1f}s")
    await esl.api(f"earshot {b} stop")
    await second.wait_closed(3)
    rig.check("stop after a reconnect still closes with 1000", second.close_code == 1000, f"{second.close_code}")
    await rig.settle()


async def s_liveness(rig):
    """An agent that goes silent is dropped after the liveness window; a healthy one is not."""
    esl, agent = rig.esl, rig.agent
    a, b, _ = await rig.call()
    m = esl.mark()
    n0 = len(rig.mute.sessions)
    await esl.api(f"earshot {b} start ws://127.0.0.1:{MUTE_PORT}/x liveness=3 ping=1")
    await asyncio.sleep(8)
    sess = rig.mute.sessions[n0:]
    first = sess[0] if sess else None
    held = (first[1] - first[0]) if first and first[1] else -1
    rig.check("an agent that answers nothing, not even a ping, is dropped after liveness=3",
              2.5 <= held <= 5.0, f"held {held:.1f}s")
    rig.check("and the stream tries again", len(sess) >= 2, f"{len(sess)} connection(s)")
    await rig.hangup(a, b)

    a, b, _ = await rig.call()
    await esl.api(f"earshot {b} start {url('/quiet')} liveness=3 ping=1")
    conn = await agent.wait_conn("/quiet")
    await asyncio.sleep(8)
    rig.check("an agent that sends no audio but answers pings stays connected past the window",
              conn is not None and not conn.closed.is_set())
    rig.check("with no reconnect", len([c for c in agent.conns if c.path == "/quiet"]) == 1)
    await rig.settle()


async def s_twilio(rig):
    """The Twilio dialect: connected, start, media, and a `stop` message before the close."""
    esl, agent = rig.esl, rig.agent
    for how in ("stop", "hangup"):
        path = f"/twilio-{how}"
        a, b, _ = await rig.call()
        await esl.api(f"earshot {b} start {url(path)} proto=twilio")
        conn = await agent.wait_conn(path)
        if not rig.check(f"[{how}] the agent is connected", conn is not None):
            continue
        await asyncio.sleep(1.5)
        events = [m.get("event") for _, m in conn.texts if isinstance(m, dict)]
        rig.check(f"[{how}] the stream opens with connected, then start, then media",
                  events[:3] == ["connected", "start", "media"], str(events[:4]))
        start = conn.json_texts(event="start")
        sid = start[0].get("streamSid") if start else None
        rig.check(f"[{how}] start carries a streamSid and the mu-law 8 kHz format",
                  bool(sid) and start[0].get("start", {}).get("mediaFormat", {}).get("encoding") == "audio/x-mulaw",
                  str(start[:1])[:200])
        rig.check(f"[{how}] media carries the caller's tone", abs(conn.heard_hz() - CALLER_HZ) < 20, f"{conn.heard_hz():.0f} Hz")
        if how == "stop":
            await esl.api(f"earshot {b} stop")
        else:
            await rig.hangup(b)
        await conn.wait_closed(3)
        last = conn.texts[-1][1] if conn.texts else None
        rig.check(f"[{how}] the last message the agent receives is Twilio's `stop`, for the same stream",
                  isinstance(last, dict) and last.get("event") == "stop" and last.get("streamSid") == sid, str(last)[:160])
        rig.check(f"[{how}] then a normal close (1000)", conn.close_code == 1000, f"{conn.close_code}")
        await rig.hangup(a)
    await rig.settle()


async def s_adapters(rig):
    """Vendor dialects against stand-in servers: ElevenLabs, Gemini, Deepgram, Twilio's numbering."""
    esl, agent = rig.esl, rig.agent

    def tone_b64(ms, rate):
        pcm, _ = tone(AGENT_HZ, ms, rate=rate)
        return base64.b64encode(pcm).decode()

    # ElevenLabs: the agent is set to 16 kHz PCM; the stream is started with the defaults.
    async def eleven(conn):
        await conn.ws.send(json.dumps({"type": "conversation_initiation_metadata",
                                       "conversation_initiation_metadata_event": {
                                           "conversation_id": "conv_rig", "agent_output_audio_format": "pcm_16000",
                                           "user_input_audio_format": "pcm_16000"}}))
        await asyncio.sleep(0.3)
        for i in range(25):                                   # 2.5 s, 100 ms a message, ahead of real time
            await conn.ws.send(json.dumps({"type": "audio", "audio_event": {"audio_base_64": tone_b64(100, 16000),
                                                                           "event_id": i + 1}}))
            await asyncio.sleep(0.08)
        await conn.ws.send(json.dumps({"type": "ping", "ping_event": {"event_id": 77}}))

    agent.on("/eleven", eleven)
    a, b, rec = await rig.call(record=True)
    await esl.api(f"earshot {b} start {url('/eleven')} proto=elevenlabs")
    conn = await agent.wait_conn("/eleven")
    if rig.check("[elevenlabs] the agent is connected", conn is not None):
        await asyncio.sleep(4)
        await esl.api(f"uuid_record {a} stop {rec}")
        ms = heard_agent(rec)
        rig.check("[elevenlabs] an agent that announces pcm_16000 is heard as its 440 Hz tone, not as noise",
                  2000 <= ms <= 3000, f"{ms} ms of tone")
        rig.check("[elevenlabs] the caller's audio arrives as user_audio_chunk messages",
                  len([m for _, m in conn.texts if isinstance(m, dict) and "user_audio_chunk" in m]) > 100)
        rig.check("[elevenlabs] the agent's ping is answered with a pong for the same event",
                  await conn.wait_text(2, type="pong", event_id=77) is not None)
        log = open(FS_LOG, errors="replace").read()
        rig.check("[elevenlabs] the log says the agent expects another caller-audio format, and how to fix it",
                  "expects caller audio as 'pcm_16000'" in log and "codec=l16 rate=16000" in log)
    await rig.hangup(a, b)

    # Gemini: the server takes its time over setupComplete.
    async def gemini(conn):
        await asyncio.sleep(1.5)
        conn.setup_sent = time.monotonic()
        await conn.ws.send(json.dumps({"setupComplete": {}}).encode())     # Gemini sends JSON in binary frames
        await asyncio.sleep(0.3)
        for _ in range(20):
            msg = {"serverContent": {"modelTurn": {"parts": [
                {"inlineData": {"mimeType": "audio/pcm;rate=24000", "data": tone_b64(100, 24000)}}]}}}
            await conn.ws.send(json.dumps(msg).encode())
            await asyncio.sleep(0.08)

    agent.on("/gemini", gemini)
    a, b, rec = await rig.call(record=True)
    await esl.api(f"earshot {b} start {url('/gemini')} proto=gemini")
    conn = await agent.wait_conn("/gemini")
    if rig.check("[gemini] the agent is connected", conn is not None):
        await asyncio.sleep(4.5)
        await esl.api(f"uuid_record {a} stop {rec}")
        first = conn.texts[0][1] if conn.texts else {}
        rig.check("[gemini] the first message is setup, naming the current Live model",
                  isinstance(first, dict) and first.get("setup", {}).get("model") == "models/gemini-3.8-live", str(first)[:120])
        audio = [t for t, m in conn.texts if isinstance(m, dict) and "realtimeInput" in m]
        rig.check("[gemini] no caller audio is sent before setupComplete, and it flows after",
                  len(audio) > 50 and audio[0] >= conn.setup_sent, f"{len(audio)} messages, first "
                  f"{(audio[0] - conn.setup_sent) if audio else 0:+.2f}s from setupComplete")
        ms = heard_agent(rec)
        rig.check("[gemini] the 24 kHz reply is heard by the caller as its 440 Hz tone", 1500 <= ms <= 2500, f"{ms} ms")
    await rig.hangup(a, b)

    # Deepgram: a pause must not let the session go silent.
    a, b, _ = await rig.call()
    await esl.api(f"earshot {b} start {url('/deepgram')} proto=deepgram")
    conn = await agent.wait_conn("/deepgram")
    if rig.check("[deepgram] the agent is connected", conn is not None):
        await asyncio.sleep(1)
        first = conn.texts[0][1] if conn.texts else {}
        rig.check("[deepgram] the first message is Settings", isinstance(first, dict) and first.get("type") == "Settings")
        rig.check("[deepgram] with audio flowing there is no KeepAlive", not conn.json_texts(type="KeepAlive"))
        await esl.api(f"earshot {b} pause")
        await asyncio.sleep(17.5)
        n = len(conn.json_texts(type="KeepAlive"))
        rig.check("[deepgram] paused for 17.5 s, the agent received two KeepAlives (one every 8 s)", n == 2, f"{n}")
        await esl.api(f"earshot {b} resume")
        await asyncio.sleep(9)
        n = len(conn.json_texts(type="KeepAlive"))
        rig.check("[deepgram] and none once audio flows again", n == 2 and not conn.closed.is_set(), f"{n}")
    await rig.hangup(a, b)

    # Twilio: every message Twilio numbers is numbered, in order.
    async def twilio(conn):
        await asyncio.sleep(0.5)
        pcm, _ = tone(AGENT_HZ, 200)
        await conn.ws.send(json.dumps({"event": "media", "streamSid": "x",
                                       "media": {"payload": base64.b64encode(audioop.lin2ulaw(pcm, 2)).decode()}}))
        await conn.ws.send(json.dumps({"event": "mark", "streamSid": "x", "mark": {"name": "rig-mark"}}))

    agent.on("/twilio-seq", twilio)
    a, b, _ = await rig.call()
    await esl.api(f"earshot {b} start {url('/twilio-seq')} proto=twilio dtmf=on")
    conn = await agent.wait_conn("/twilio-seq")
    if rig.check("[twilio] the agent is connected", conn is not None):
        await asyncio.sleep(1.2)
        await esl.api(f"uuid_send_dtmf {a} 7")
        await asyncio.sleep(1)
        mark = conn.json_texts(event="mark")
        dtmf = conn.json_texts(event="dtmf")
        rig.check("[twilio] the mark is echoed once its audio has played, with a sequenceNumber",
                  len(mark) == 1 and mark[0].get("mark", {}).get("name") == "rig-mark" and "sequenceNumber" in mark[0], str(mark)[:160])
        rig.check("[twilio] the caller's digit arrives as dtmf on the inbound track, with a sequenceNumber",
                  len(dtmf) == 1 and dtmf[0].get("dtmf") == {"track": "inbound", "digit": "7"} and "sequenceNumber" in dtmf[0], str(dtmf)[:160])
        seqs = [int(m["sequenceNumber"]) for _, m in conn.texts if isinstance(m, dict) and "sequenceNumber" in m]
        rig.check("[twilio] sequence numbers run 1, 2, 3, ... with no gap or repeat across media, mark and dtmf",
                  seqs == list(range(1, len(seqs) + 1)) and len(seqs) > 50, f"{len(seqs)} numbered messages")
    await rig.settle()


async def s_app(rig):
    """The stream started from the dialplan application instead of the API."""
    esl, agent = rig.esl, rig.agent
    n0 = len([c for c in agent.conns if c.path == "/app"])
    a, b, _ = await rig.call("app")
    conn = await agent.wait_conn("/app", nth=n0 + 1)
    if rig.check("a dialplan `earshot start` connects the agent", conn is not None):
        await asyncio.sleep(1.2)
        rig.check("with caller audio", conn.frames > 30, f"{conn.frames} frames")
        res = await _ask(conn, {"action": "send_dtmf", "digits": "2"}, "app-dt")
        rig.check("and its options applied (commands=true)", res is not None and res.get("ok") is True, str(res))
        await rig.hangup(a)
        await conn.wait_closed(3)
        rig.check("the caller hanging up closes it with 1000", conn.close_code == 1000, f"{conn.close_code}")
    await rig.settle()


async def s_pause_fanout(rig):
    """Pause and resume, and two streams on one call."""
    esl, agent = rig.esl, rig.agent
    a, b, _ = await rig.call()
    await esl.api(f"earshot {b} start {url('/main')}")
    r = await esl.api(f"earshot {b} start {url('/fork')} id=fork dir=in")
    rig.check("a second, named stream starts on the same call", r.startswith("+OK"), r)
    main, fork = await agent.wait_conn("/main"), await agent.wait_conn("/fork")
    if not rig.check("both agents are connected", main is not None and fork is not None):
        await rig.settle()
        return
    await asyncio.sleep(1)
    await esl.api(f"earshot {b} pause")
    await asyncio.sleep(0.2)
    n_main, n_fork = main.frames, fork.frames
    await asyncio.sleep(1.5)
    rig.check("pause stops caller audio to the paused stream", main.frames - n_main <= 2, f"+{main.frames - n_main}")
    rig.check("and leaves the other stream running", fork.frames - n_fork > 50, f"+{fork.frames - n_fork}")
    await esl.api(f"earshot {b} resume")
    n_main = main.frames
    await asyncio.sleep(1)
    rig.check("resume starts it again", main.frames - n_main > 35, f"+{main.frames - n_main}")
    await esl.api(f"earshot {b} stop id=fork")
    await fork.wait_closed(3)
    rig.check("stopping the named stream closes only that one (1000)",
              fork.close_code == 1000 and not main.closed.is_set(), f"fork {fork.close_code}")
    await rig.hangup(b)
    await main.wait_closed(3)
    rig.check("hangup closes the remaining stream (1000)", main.close_code == 1000, f"{main.close_code}")
    await rig.hangup(a)
    await rig.settle()


async def s_churn(rig, calls=150, at_once=10):
    """Many short calls: nothing accumulates."""
    esl, agent = rig.esl, rig.agent
    agent.on("/churn", lambda c: speak(c, 600))

    async def one(i, stream):
        try:
            a, b, _ = await rig.call()
            if stream:
                await esl.api(f"earshot {b} start {url('/churn')} commands=true dtmf=on metrics=1")
            await asyncio.sleep(random.uniform(0.6, 1.2))
            if stream and i % 2:
                await esl.api(f"earshot {b} stop")
            await rig.hangup(b if i % 4 < 2 else a)
            await rig.hangup(a, b)
            return True
        except Exception as e:      # noqa: BLE001 - a failed call is a result, not a crash of the test
            return str(e)

    async def batch(n, stream):
        sem = asyncio.Semaphore(at_once)

        async def guarded(i):
            async with sem:
                return await one(i, stream)
        res = await asyncio.gather(*[guarded(i) for i in range(n)])
        await asyncio.sleep(3)
        return res

    # FreeSWITCH's own memory grows for its first several hundred calls and then levels
    # off, stream or no stream. So: warm up, then measure the same number of calls without
    # a stream, with one, and without again; the stream must not cost more than they do.
    await batch(calls * 2, True)
    rss0, thr0 = rig.rss_kb(), rig.threads()
    await batch(calls, False)
    rss1 = rig.rss_kb()
    n0 = len([c for c in agent.conns if c.path == "/churn"])
    res = await batch(calls, True)
    rss2, thr2 = rig.rss_kb(), rig.threads()
    await batch(calls, False)
    rss3 = rig.rss_kb()
    bad = [r for r in res if r is not True]
    conns = [c for c in agent.conns if c.path == "/churn"][n0:]
    rig.check(f"{calls} short calls, {at_once} at a time, all placed and streamed", not bad and len(conns) == calls,
              f"{len(bad)} failed, {len(conns)} connections; {bad[:2]}")
    codes = sorted(set(c.close_code for c in conns))
    rig.check("every stream closed with 1000", codes == [1000], str(codes))
    rig.check("the thread count is back where it was", thr2 <= thr0 + 2, f"{thr0} -> {thr2}")
    plain = max(rss1 - rss0, rss3 - rss2) / calls
    streamed = (rss2 - rss1) / calls
    rig.check("calls with a stream grow memory no more than calls without one (within 10 kB a call)",
              streamed <= plain + 10, f"{streamed:+.1f} kB a call with, {plain:+.1f} without")
    rig.note(f"resident memory per call: {(rss1 - rss0) / calls:+.1f} kB without a stream, {streamed:+.1f} with, "
             f"{(rss3 - rss2) / calls:+.1f} without again; threads {thr0} -> {thr2}")
    await rig.settle()


async def s_unload(rig):
    """Unloading the module with a stream up must not take FreeSWITCH down. Run last."""
    esl, agent = rig.esl, rig.agent
    a, b, _ = await rig.call()
    await esl.api(f"earshot {b} start {url('/unload')}")
    conn = await agent.wait_conn("/unload")
    await asyncio.sleep(0.5)
    r = await esl.api("unload mod_earshot", timeout=40)
    rig.note(f"unload said: {r[:100]}")
    await asyncio.sleep(1)
    rig.check("FreeSWITCH survives an unload with a stream up", rig.fs_pid() == rig.pid)
    await rig.hangup(a, b)
    await asyncio.sleep(0.5)
    if "true" not in await esl.api("module_exists mod_earshot"):
        await esl.api("load mod_earshot")
    a, b, _ = await rig.call()
    r = await esl.api(f"earshot {b} start {url('/after-unload')}")
    conn = await agent.wait_conn("/after-unload")
    rig.check("and streams work again afterwards", r.startswith("+OK") and conn is not None, r)
    await rig.settle()


SCENARIOS = [
    ("basic", s_basic), ("hangup", s_hangup), ("stop_mid_playback", s_stop_mid_playback),
    ("commands", s_commands), ("unreachable", s_unreachable), ("reconnect", s_reconnect),
    ("liveness", s_liveness), ("twilio", s_twilio), ("adapters", s_adapters), ("app", s_app), ("pause_fanout", s_pause_fanout),
    ("verb_race", s_verb_race), ("agent_race", s_agent_race), ("churn", s_churn), ("unload", s_unload),
]


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("only", nargs="*")
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()
    if args.list:
        for name, fn in SCENARIOS:
            print(f"{name:20} {fn.__doc__.strip().splitlines()[0]}")
        return 0
    unknown = [n for n in args.only if n not in dict(SCENARIOS)]
    if unknown:
        print(f"unknown scenario(s): {unknown}")
        return 2
    rig = Rig()
    try:
        await rig.start()
    except Exception as e:      # noqa: BLE001
        print(f"FATAL: the rig is not usable: {e}")
        return 2
    for name, fn in SCENARIOS:
        if args.only and name not in args.only:
            continue
        rig.scenario = name
        print(f"\n== {name}: {fn.__doc__.strip().splitlines()[0]}", flush=True)
        try:
            await fn(rig)
        except Exception as e:  # noqa: BLE001
            rig.check("the scenario ran to its end", False, f"{type(e).__name__}: {e}")
            if rig.fs_pid() == rig.pid:
                try:
                    await rig.esl.api("hupall")
                except Exception:   # noqa: BLE001
                    pass
        if rig.fs_pid() != rig.pid:
            print("\nFATAL: FreeSWITCH died or restarted; stopping here.")
            break
    passed = sum(1 for r in rig.results if r[2])
    failed = [r for r in rig.results if not r[2]]
    print(f"\n=== RESULT: {passed} passed, {len(failed)} failed ===")
    for sc, name, _, detail in failed:
        print(f"  FAIL {sc}: {name}  [{detail}]")
    return 1 if failed or rig.fs_pid() != rig.pid else 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
