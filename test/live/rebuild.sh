#!/usr/bin/env bash
# Inside the rig container: rebuild mod_earshot from /src and load the new build.
set -euo pipefail
P='JambonzR0ck$'
rm -rf /w/src /w/CMakeLists.txt && mkdir -p /w && cp -r /src/src /src/CMakeLists.txt /w/ && cp /src/test/live/live_test.py /w/
cd /w
cmake -S . -B build -DEARSHOT_TESTS=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo ${EXTRA_CMAKE:-} >/dev/null
cmake --build build --parallel > /tmp/build.log 2>&1 || { grep -iE "warning|error" /tmp/build.log; exit 1; }
grep -iE "warning" /tmp/build.log || true
fs_cli -p "$P" -x "hupall" >/dev/null
fs_cli -p "$P" -x "unload mod_earshot" >/dev/null || true
cp build/mod_earshot.so /usr/local/freeswitch/mod/
cp /src/test/live/dialplan.xml /usr/local/freeswitch/conf/dialplan/mrf.xml
fs_cli -p "$P" -x "reloadxml" >/dev/null
fs_cli -p "$P" -x "load mod_earshot"
