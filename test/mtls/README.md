# mTLS / custom-CA end-to-end test

Verifies box-level mutual TLS and custom-CA verification
(`EARSHOT_TLS_CLIENT_CERT`, `EARSHOT_TLS_CLIENT_KEY`, `EARSHOT_TLS_CA`).

> **These are process environment variables, not channel variables.** They are read
> **once at module load** (the WS context pool is built then), so set them where
> FreeSWITCH is launched — systemd `EnvironmentFile`, a wrapper script, or the shell —
> then `fs_cli -x "reload mod_earshot"`. Setting them in the dialplan with `set` will
> **not** work.

## 1. Set up the server

```bash
cd test/mtls
./gen-certs.sh                 # -> ca.pem, server.*, client.*, bogus-ca.pem (all gitignored)
pip install websockets
python3 mtls_echo_server.py    # wss://localhost:9443/ , requires a client cert
```

## 2. Point earshot at it

Launch FreeSWITCH with the client identity in its environment, e.g.:

```bash
EARSHOT_TLS_CLIENT_CERT="$PWD/client.pem" \
EARSHOT_TLS_CLIENT_KEY="$PWD/client.key" \
EARSHOT_TLS_CA="$PWD/ca.pem" \
  freeswitch -nonat        # (or your systemd unit's EnvironmentFile)
```

Dialplan (no TLS options needed on the app line — the identity is box-level):

```xml
<action application="answer"/>
<action application="earshot" data="start wss://localhost:9443/ proto=native dir=both"/>
<action application="playback" data="silence_stream://-1"/>
```

Call the extension and talk — you should hear yourself echoed back, and the server
prints `client connected (mTLS handshake passed)`.

## 3. Test matrix

| # | Change | Expected result | Proves |
|---|--------|-----------------|--------|
| 1 | all three vars set | echo works; server prints "client connected" | mTLS + custom-CA succeed |
| 2 | unset `EARSHOT_TLS_CLIENT_CERT` + `_KEY` | connection fails; server never prints "client connected" | server-required client cert is enforced (earshot presents none) |
| 3 | `EARSHOT_TLS_CA=$PWD/bogus-ca.pem` | connection fails (server cert not trusted) | the CA field is actually honored, not ignored |
| 4 | set only `EARSHOT_TLS_CLIENT_CERT` (no `_KEY`) | `lwsl_err` in the FS log; no cert presented; server rejects | the both-or-neither guard fires |

**Regression:** with **none** of the vars set, a normal `wss://` call to a public
vendor (OpenAI Realtime / Deepgram) must still connect — box-level TLS is fully
opt-in and the default path is unchanged.
