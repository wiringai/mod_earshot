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
- **The control channel is off by default.** It is enabled per stream with `commands=true`, and even
  then is **whitelisted** to a fixed set of `uuid_*` call-control actions — there is no arbitrary API
  passthrough. Only enable it for agents you trust to drive the call.
- **PCI/PII masking** (`mask on`) mutes caller audio and redacts DTMF to the agent; the digit value is
  never placed in the `earshot::dtmf` audit event during a masking window.
- **Transport**: use `wss://` for agents across a network. `auth=` sets the `Authorization` header;
  `EARSHOT_TLS_NO_HOSTNAME_CHECK` disables cert/hostname validation and is **dev-only**.

Reports that require an already-compromised FreeSWITCH host or a malicious operator-supplied
configuration are generally out of scope, but tell us anyway if the impact is surprising.

## Supported versions

The latest tagged release (currently **0.1.0**) and the `main` branch are supported: fixes land on
`main` and in the next tagged release. A formal support matrix will accompany the `1.0` release.
