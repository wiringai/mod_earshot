#!/usr/bin/env bash
# One-shot mTLS end-to-end matrix.
#
# For each case it (re)starts FreeSWITCH with that case's EARSHOT_TLS_* environment,
# originates a test call through a dialplan extension that runs earshot against the local
# mTLS echo server, and checks whether the mTLS handshake reached the server. Reports
# PASS/FAIL per case and a final verdict.
#
# ─── PREREQUISITES on this box ──────────────────────────────────────────────────────────
#   • A dialplan extension ($TEST_EXT) that does:
#         answer → earshot start wss://localhost:9443/ proto=native dir=both → park/silence
#     and does NOT set `insecure` / EARSHOT_TLS_NO_HOSTNAME_CHECK.
#   • FreeSWITCH reads its environment from $FS_ENV_FILE (systemd EnvironmentFile or similar),
#     and $FS_RESTART restarts it so the new environment takes effect.
#   • openssl, python3, `pip install 'websockets>=11'`, and fs_cli on PATH.
#
# ─── CONFIGURE (all env-overridable) ────────────────────────────────────────────────────
set -uo pipefail
cd "$(dirname "$0")" || exit 1

FS_ENV_FILE="${FS_ENV_FILE:-/etc/default/mod_earshot-tls}"   # file the FS unit's EnvironmentFile points at
FS_RESTART="${FS_RESTART:-sudo systemctl restart freeswitch}" # how to restart FS so it re-reads the env
TEST_EXT="${TEST_EXT:-5000}"                                  # dialplan ext that runs earshot -> wss://localhost:9443/
ECHO_LOG="${ECHO_LOG:-$PWD/echo-server.log}"                  # where the echo server logs
WAIT_S="${WAIT_S:-8}"                                         # seconds to wait for a connection per case

CERT="$PWD/client.pem"; KEY="$PWD/client.key"; CA="$PWD/ca.pem"; BOGUS="$PWD/bogus-ca.pem"
FAILED=0; ECHO_PID=""

cleanup() { [ -n "$ECHO_PID" ] && kill "$ECHO_PID" 2>/dev/null; }
trap cleanup EXIT

need() { command -v "$1" >/dev/null 2>&1 || { echo "missing: $1"; exit 1; }; }
need openssl; need python3; need fs_cli

# ─── certs + echo server ────────────────────────────────────────────────────────────────
[ -f "$CERT" ] && [ -f "$CA" ] || ./gen-certs.sh
python3 -c "import websockets" 2>/dev/null || { echo "pip install 'websockets>=11' first"; exit 1; }

: > "$ECHO_LOG"
python3 mtls_echo_server.py >>"$ECHO_LOG" 2>&1 &
ECHO_PID=$!
sleep 1
kill -0 "$ECHO_PID" 2>/dev/null || { echo "echo server failed to start; see $ECHO_LOG"; exit 1; }
echo "echo server up (pid $ECHO_PID), logging to $ECHO_LOG"

# ─── guard: this restarts FreeSWITCH repeatedly on THIS box ──────────────────────────────
echo
echo "This will restart FreeSWITCH via:  $FS_RESTART"
echo "  writing env to: $FS_ENV_FILE   originating to ext: $TEST_EXT"
read -rp "Restart FreeSWITCH on THIS box up to 4 times? [y/N] " a
[ "$a" = y ] || [ "$a" = Y ] || { echo "aborted."; exit 1; }

restart_fs_with() {                       # args: EARSHOT_TLS_* lines to write (may be none)
  printf '%s\n' "$@" | sudo tee "$FS_ENV_FILE" >/dev/null
  $FS_RESTART || return 1
  for _ in $(seq 1 30); do fs_cli -x status >/dev/null 2>&1 && return 0; sleep 0.5; done
  echo "  ! FreeSWITCH did not come back up"; return 1
}

run_case() {                              # args: name  expect(connect|reject)  env-lines...
  local name="$1" expect="$2"; shift 2
  echo; echo "== $name  (expect: $expect) =="
  local before; before=$(grep -c "client connected" "$ECHO_LOG" 2>/dev/null || echo 0)
  restart_fs_with "$@" || { echo "  FAIL: fs restart"; FAILED=1; return; }
  fs_cli -x "originate loopback/$TEST_EXT &sleep(5000)" >/dev/null 2>&1
  local got=reject
  for _ in $(seq 1 "$WAIT_S"); do
    local now; now=$(grep -c "client connected" "$ECHO_LOG" 2>/dev/null || echo 0)
    [ "$now" -gt "$before" ] && { got=connect; break; }
    sleep 1
  done
  if [ "$got" = "$expect" ]; then echo "  PASS ($got)"; else echo "  >>> FAIL: got $got, expected $expect"; FAILED=1; fi
}

#            name                         expect    environment (written to FS_ENV_FILE)
run_case "1 all vars set"                 connect   "EARSHOT_TLS_CLIENT_CERT=$CERT" "EARSHOT_TLS_CLIENT_KEY=$KEY" "EARSHOT_TLS_CA=$CA"
run_case "2 no client cert"              reject     "EARSHOT_TLS_CA=$CA"
run_case "3 bogus CA (cert+key kept)"    reject     "EARSHOT_TLS_CLIENT_CERT=$CERT" "EARSHOT_TLS_CLIENT_KEY=$KEY" "EARSHOT_TLS_CA=$BOGUS"
run_case "4 cert only, no key"           reject     "EARSHOT_TLS_CLIENT_CERT=$CERT"

echo
if [ "$FAILED" = 0 ]; then
  echo "ALL CASES PASSED ✅"
else
  echo "SOME CASES FAILED ❌  — inspect $ECHO_LOG and the FreeSWITCH log"
fi
echo "(Case 4 also: confirm the FS log shows the both-or-neither 'lwsl_err' guard message.)"
echo "(Regression — no-vars wss to a public vendor still connects — is a separate manual check;"
echo " this harness's server requires a client cert, so it can't stand in for that.)"
exit "$FAILED"
