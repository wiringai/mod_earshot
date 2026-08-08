# Security Policy

## Reporting a vulnerability

Please report security issues **privately** — do not open a public issue for anything exploitable.

- Use GitHub's **[Report a vulnerability](https://github.com/wiringai/mod_earshot/security/advisories/new)**
  (Security → Advisories), or
- email **xpertvoipai@gmail.com**.

Include: affected version/commit, a description, and a minimal reproduction (a crafted WebSocket frame,
dialplan, or call flow). We aim to acknowledge within a few days and to ship a fix or mitigation before
any public disclosure. Please give us a reasonable window before disclosing.

## Scope & threat model

`mod_earshot` runs **inside** FreeSWITCH and talks to two parties: the caller (SIP/RTP, via FreeSWITCH)
and the agent (over a WebSocket you configure). The agent endpoint is semi-trusted — you point Earshot
at it — but the module still treats agent input defensively:

- **Wire parsers** (protobuf, JSON) must handle malformed/adversarial input without crashing or reading
  out of bounds. The Pipecat protobuf codec (`es_pb`) is a pure-C unit with tests covering truncated
  frames and length-overflow attacks (`test/test_pb.c`); JSON goes through FreeSWITCH's cJSON with
  null-checked access.
- **The control channel is off by default.** It is enabled per stream with `commands=` and can be
  scoped per action (`commands=play,hangup`), not just all-or-nothing (`commands=true`). Two layers
  guard it:
  1. **Action allowlist** — only a fixed set of `uuid_*` call-control actions is reachable; there is
     no arbitrary API passthrough.
  2. **Argument validation** — the action allowlist is *not* a sufficient boundary by itself, because
     `uuid_broadcast`/`uuid_transfer`/`uuid_setvar` can each execute a dialplan application, run a
     shell command, or trigger a hook when handed hostile arguments (FreeSWITCH's `app::args` form,
     the `inline` dialplan, and `execute_on_`/`api_on_` channel variables). Every argument is therefore
     validated before it reaches `switch_api_execute` (`es_cmdguard`): the `::` app-exec form, `inline`
     dialplan, `..` path traversal, and execution-triggering variable names are rejected, and blocked
     attempts are recorded in the `earshot::command` audit.

  This still grants real call-control power to whatever agent you connect — enable it only for agents
  you trust to drive the call, and prefer the narrowest `commands=` grant the flow needs. Because agent
  output can be steered by spoken prompt-injection, treat an injection that reaches this channel as part
  of your threat model. Notes: **`setvar` is fail-closed** — a name denylist cannot secure it (identifier-clean
  variables such as `transfer_after_bridge` reach the `inline` dialplan, i.e. RCE, through their *value*), so
  only variable names you list in `setvars=<name,...>` are settable, and none are settable by default. `record`
  paths are validated (no `::`, `..`, whitespace) but are **not confined to a base directory**, so a granted
  `record` still permits an attacker-chosen absolute write path — don't grant `record` to untrusted agents.
  Media arguments cannot contain `::`, which also rejects IPv6-literal URLs (`http://[2001:db8::1]/x.wav`).
- **PCI/PII masking** (`mask on`) mutes caller audio and redacts DTMF to the agent; the digit value is
  never placed in the `earshot::dtmf` audit event during a masking window.
- **Transport**: use `wss://` for agents across a network. `auth=<token>` (or the `EARSHOT_AUTH`
  channel variable for a value containing a space, e.g. `Bearer <key>`) sets the `Authorization`
  header. For **mutual TLS**, set `EARSHOT_TLS_CLIENT_CERT` + `EARSHOT_TLS_CLIENT_KEY` (a box-level
  client identity; the key must be an **unencrypted PEM**). `EARSHOT_TLS_CA` pins a private CA for the
  agent — but it **replaces the system
  trust store for every connection on the box**, so set it only when all agent endpoints chain to
  that CA (otherwise public-CA vendors will fail verification). `EARSHOT_TLS_NO_HOSTNAME_CHECK`
  disables cert/hostname validation and is **dev-only**.

Reports that require an already-compromised FreeSWITCH host or a malicious operator-supplied
configuration are generally out of scope, but tell us anyway if the impact is surprising.

## Supported versions

The latest tagged release (currently **0.1.0**) and the `main` branch are supported: fixes land on
`main` and in the next tagged release. A formal support matrix will accompany the `1.0` release.
