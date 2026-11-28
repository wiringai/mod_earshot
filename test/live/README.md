# Live test rig

Real calls through `mod_earshot` on a real FreeSWITCH, with no carrier, no agent keys
and no SignalWire token. One command:

```
test/live/run.sh              # every scenario, about 12 minutes
test/live/run.sh basic twilio # only these
test/live/run.sh --list
test/live/run.sh --asan       # the module built with AddressSanitizer
```

It needs Docker. The first run pulls a 2 GB FreeSWITCH image.

## How it works

One container runs FreeSWITCH 1.10 and the test. FreeSWITCH calls itself over SIP:
the calling leg plays a 300 Hz tone (the caller), the called leg is parked and carries
the stream. `live_test.py` is the agent (a WebSocket server), the operator (the event
socket) and the judge. The agent answers with a 440 Hz tone, and the calling leg is
recorded, so both directions are checked by frequency and not by "packets flowed".

The module is built in the container from this checkout, against the distribution's
libwebsockets (4.1.6 on Debian 12).

## What it covers

| Scenario | What it proves |
|---|---|
| `basic` | Audio both ways, handshake headers, the events, DTMF, `send`, and a `stop` the agent sees as close code 1000 |
| `hangup` | Either party hanging up closes the stream with 1000 and fires a final metrics event |
| `stop_mid_playback` | A stop with 18 s of agent audio queued returns at once and the caller stops hearing it |
| `commands` | Each allow-list refusal, each grant, `ok:false` when FreeSWITCH fails a command, where a relative recording lands |
| `unreachable` | No false `connected`, `reconnect_max=`, `reconnect_deadline=`, `connect_timeout=`, `earshot_failed` in a dialplan |
| `reconnect` | The agent drops the connection; the stream returns, says `reconnected`, and carries no backlog |
| `liveness` | A silent agent is dropped after `liveness=`; one that only answers pings is kept |
| `twilio` | `connected`, `start`, `media`, then `stop` as the last message before the close |
| `app` | The dialplan application, not only the API |
| `pause_fanout` | `pause` and `resume`, and two streams on one call |
| `verb_race` | 60 rounds of operator commands and caller digits racing a stop or a hangup |
| `agent_race` | 30 rounds of an agent flooding commands into a stop or a hangup |
| `churn` | 150 short calls ten at a time: memory against the same calls without a stream, threads, close codes |
| `unload` | Unloading the module with a stream up does not take FreeSWITCH down |

After every scenario the rig checks that FreeSWITCH is still the same process and no
channel is left.

## What it does not cover

- TLS (`wss://`) and client certificates: see `test/mtls`.
- Real vendor endpoints. The agent here speaks the native and Twilio dialects only.
- Load. Ten calls at a time is a race test, not a capacity figure.
- Other FreeSWITCH or libwebsockets versions than the image's.
