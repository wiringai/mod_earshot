# Contributing to Earshot

Thanks for your interest. Earshot is a FreeSWITCH module — small, C-based, and focused.

## Build & run locally

```bash
cmake -S . -B build && cmake --build build
ctest --test-dir build              # unit tests (portable core — no FreeSWITCH needed)
sudo cmake --install build          # -> $FS_MOD_DIR/mod_earshot.so
fs_cli -x "load mod_earshot"
```

Two build targets:

- **Portable core + unit tests** (`es_codec`, `es_pb`) build with just a C compiler +
  CMake — **no FreeSWITCH**. Always build and run these; they're what CI gates on.
- **The module** (`mod_earshot.so`) needs FreeSWITCH **dev headers** (1.10 or 1.11) +
  `libwebsockets-dev`. FreeSWITCH packages are token-gated by SignalWire (a free personal access
  token). The [`Dockerfile`](Dockerfile) does this for you — build the module + `.deb` with:

  ```bash
  docker build --build-arg SW_TOKEN=pat_... -t earshot-build .   # module + .deb
  docker build -t earshot-build .                                 # unit tests only (no token)
  ```

  In CI, set the `SW_TOKEN` repository secret to enable the module/`.deb` job; without it the unit
  job still runs and passes (see `.github/workflows/ci.yml`).

## Ground rules that keep the module safe

- **Never block the media thread.** The media-bug callback only moves frames
  to/from queues. All socket I/O lives on the per-stream thread. This is the rule
  that makes a slow/dead agent degrade gracefully instead of stalling audio.
- **One teardown path.** `stop`, hangup, and socket-close must converge on a single
  guarded teardown. Reconnect + hangup races are where media modules leak or wedge.
- **Adapters at the edge only.** Protocol-specific code (`es_proto.c`) lives in the adapter layer;
  the core stays protocol-neutral.
- **Match FreeSWITCH memory idioms.** Prefer session/module pools over raw malloc; pair every locate
  with an rwunlock.
- **Anything parsing untrusted bytes gets a test.** Wire parsers (protobuf, JSON) must handle
  malformed/adversarial input safely — see `test/test_pb.c` for the pattern.

## Before a PR

- `cmake --build build && ctest --test-dir build` is green (unit tests need no FreeSWITCH).
- The module compiles where FS headers are available (`docker build --build-arg SW_TOKEN=…`).
- New behavior is reflected in `docs/API.md` and, if it changes the wire, `docs/ARCHITECTURE.md`
  and `FEATURES.md`; add a `test/` case where the logic is testable off-box.
- Validate the audio path end-to-end where relevant: `test/mock_agent.py` is a reference agent that
  speaks every wire protocol with no vendor keys — drive a call into it with `sipp` or a softphone.

## Good first issues

See [ROADMAP.md](ROADMAP.md) for open items (e.g. `track=both` supervisor mixing, Opus, a Prometheus
textfile exporter).

## Code of conduct

By participating, you agree to abide by the [Code of Conduct](CODE_OF_CONDUCT.md).
