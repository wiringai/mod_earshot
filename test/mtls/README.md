# mTLS / custom-CA end-to-end test

Verifies box-level mutual TLS and custom-CA verification
(`EARSHOT_TLS_CLIENT_CERT`, `EARSHOT_TLS_CLIENT_KEY`, `EARSHOT_TLS_CA`).

> **These are process environment variables, not channel variables.** `mod_earshot` reads them
> with `getenv()` when its WebSocket pool is built at module load, and a running process's
> environment is fixed at launch — so **each test case below needs FreeSWITCH (re)started with
> that case's environment.** A `set` in the dialplan, or a bare `reload mod_earshot` (same process
> env), will **not** change them. Provide them via systemd `EnvironmentFile`, a wrapper script, or
> the launching shell.

> **Do not set `insecure` / `EARSHOT_TLS_NO_HOSTNAME_CHECK` for these tests.** It skips server-cert
> verification, which would make case 3 (bogus CA) connect anyway and silently pass.

## 1. Set up the server

```bash
cd test/mtls
./gen-certs.sh                    # -> ca.pem, server.*, client.*, bogus-ca.pem (all gitignored)
pip install 'websockets>=11'      # <11 uses a 2-arg handler and would false-fail case 1
python3 mtls_echo_server.py       # wss://localhost:9443/ , requires a client cert
```

## 2. Point earshot at it

(Re)start FreeSWITCH with the client identity in its environment:

```bash
EARSHOT_TLS_CLIENT_CERT="$PWD/client.pem" \
EARSHOT_TLS_CLIENT_KEY="$PWD/client.key" \
EARSHOT_TLS_CA="$PWD/ca.pem" \
  freeswitch -nonat               # (or put these in your systemd unit's EnvironmentFile)
```

On startup the FS log should show the effective state — confirm it before dialing:

```
earshot: box-level TLS active — client cert on, custom CA on
```

Dialplan (no TLS options on the app line — the identity is box-level; do **not** add `insecure`):

```xml
<action application="answer"/>
<action application="earshot" data="start wss://localhost:9443/ proto=native dir=both"/>
<action application="playback" data="silence_stream://-1"/>
```

Call the extension and talk — you should hear yourself echoed back, and the server prints
`client connected (mTLS handshake passed)`.

## 3. Test matrix

Each row is a **full environment**. Restart FreeSWITCH with exactly the vars shown, then dial.
(Keep cert+key set for case 3 — that's what makes it test the *CA*, not a missing client cert.)

| # | Restart FS with this env | Expected | Proves |
|---|---|---|---|
| 1 | `CERT=client.pem` · `KEY=client.key` · `CA=ca.pem` | echo works; server prints "client connected" | mTLS + custom-CA succeed |
| 2 | `CA=ca.pem`; **CERT/KEY unset** | connect fails; server never prints "client connected" | the server-required client cert is enforced (earshot presents none) |
| 3 | `CERT=client.pem` · `KEY=client.key` · **`CA=bogus-ca.pem`** | connect fails (server cert not trusted) | the CA is honored — cert+key stay set, so the failure is CA-only |
| 4 | **`CERT=client.pem` only** (KEY unset) | `lwsl_err` in the FS log; no cert presented; server rejects | the both-or-neither guard fires |

(`CERT`/`KEY`/`CA` above are the `EARSHOT_TLS_CLIENT_CERT` / `EARSHOT_TLS_CLIENT_KEY` /
`EARSHOT_TLS_CA` vars, pointed at the files from `gen-certs.sh`.)

**Regression:** with **none** of the vars set (restart FS with a clean env), a normal `wss://`
call to a public vendor (OpenAI Realtime / Deepgram) must still connect — box-level TLS is fully
opt-in and the default path is unchanged.

## 4. Automated runner (optional)

`run-matrix.sh` does all four cases in one shot: for each case it writes that case's env,
restarts FreeSWITCH, originates a call through `$TEST_EXT`, and checks the echo-server log for
the handshake — reporting PASS/FAIL and a final verdict. It prompts before restarting FS (so it
can't surprise a production box) and is configurable for non-systemd setups:

```bash
# needs a dialplan ext that runs `earshot start wss://localhost:9443/ ... ` and no `insecure`
FS_ENV_FILE=/etc/default/mod_earshot-tls \
FS_RESTART='sudo systemctl restart freeswitch' \
TEST_EXT=5000 \
  ./run-matrix.sh
```

It covers cases 1–4; the no-vars **regression** (a public-vendor `wss://` still connects) stays a
manual check, since this harness's server requires a client cert and can't stand in for a public
endpoint.
