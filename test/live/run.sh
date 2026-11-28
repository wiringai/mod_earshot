#!/usr/bin/env bash
# run.sh — live calls through mod_earshot on a real FreeSWITCH, in one container.
#
#   test/live/run.sh                      every scenario (about 12 minutes)
#   test/live/run.sh basic twilio         only these (test/live/run.sh --list)
#   test/live/run.sh --asan               the module built with AddressSanitizer
#   test/live/run.sh --keep basic         leave the container up afterwards (es-live)
#
# Needs Docker and nothing else: no SignalWire token, no carrier, no agent keys.
# Exit status is the test's: 0 all passed, 1 a check failed, 2 the rig did not come up.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
NAME="${NAME:-es-live}"
P='JambonzR0ck$'
ASAN=0; KEEP=0; ARGS=()
for a in "$@"; do
  case "$a" in
    --asan) ASAN=1 ;;
    --keep) KEEP=1 ;;
    *) ARGS+=("$a") ;;
  esac
done

docker build -q -t es-live "$HERE" >/dev/null || { echo "FATAL: could not build the rig image"; exit 2; }
docker rm -f "$NAME" >/dev/null 2>&1

RUN_ENV=(); BUILD_ENV=(-e LD_PRELOAD=)
if [[ "$ASAN" -eq 1 ]]; then
  # FreeSWITCH itself is not instrumented; preloading the runtime lets it load a module that is.
  # Errors are written to /tmp/asan.<pid> in the container and the run carries on.
  LIB="$(docker run --rm --entrypoint bash es-live -c 'ls /usr/lib/*-linux-gnu/libasan.so.? | head -1')"
  RUN_ENV=(-e "LD_PRELOAD=$LIB" -e ASAN_OPTIONS=detect_leaks=0:halt_on_error=0:log_path=/tmp/asan)
  BUILD_ENV+=(-e "CFLAGS=-fsanitize=address -fsanitize-recover=address -fno-omit-frame-pointer -g")
fi

docker run -d --name "$NAME" -v "$REPO":/src:ro ${RUN_ENV[@]+"${RUN_ENV[@]}"} es-live freeswitch -g -l info >/dev/null \
  || { echo "FATAL: could not start the rig container"; exit 2; }
cleanup() { [[ "$KEEP" -eq 1 ]] || docker rm -f "$NAME" >/dev/null 2>&1; }
trap cleanup EXIT

up=0
for _ in $(seq 1 90); do
  docker exec -e LD_PRELOAD= "$NAME" fs_cli -p "$P" -x status >/dev/null 2>&1 && { up=1; break; }
  sleep 1
done
[[ "$up" -eq 1 ]] || { echo "FATAL: FreeSWITCH did not come up"; docker logs "$NAME" 2>&1 | tail -20; exit 2; }

docker exec "${BUILD_ENV[@]}" "$NAME" /src/test/live/rebuild.sh >/dev/null \
  || { echo "FATAL: mod_earshot did not build or load"; docker exec "$NAME" cat /tmp/build.log 2>/dev/null | tail -20; exit 2; }

docker exec -e LD_PRELOAD= "$NAME" bash -c 'cd /w && python3 -W ignore live_test.py "$@"' _ ${ARGS[@]+"${ARGS[@]}"}
rc=$?

if [[ "$ASAN" -eq 1 ]]; then
  n="$(docker exec -e LD_PRELOAD= "$NAME" bash -c 'ls /tmp/asan.* 2>/dev/null | wc -l')"
  if [[ "$n" -gt 0 ]]; then
    echo "=== AddressSanitizer reported errors ==="
    docker exec -e LD_PRELOAD= "$NAME" bash -c 'head -60 /tmp/asan.*'
    rc=1
  else
    echo "=== AddressSanitizer: no errors ==="
  fi
fi
if [[ "$rc" -ne 0 ]]; then
  echo "--- the module's last log lines ---"
  docker exec -e LD_PRELOAD= "$NAME" bash -c 'grep -i earshot /usr/local/freeswitch/log/freeswitch.log | grep -v DEBUG | tail -25'
fi
exit "$rc"
