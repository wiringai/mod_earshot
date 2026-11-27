# Reproducible build image for mod_earshot.
#
# The portable core (codec, protobuf, queue) and its unit tests build with no
# FreeSWITCH. The module itself needs FreeSWITCH dev headers, which come from the
# SignalWire apt repo (free personal access token at https://signalwire.com):
#
#   docker build -t earshot-build .                       # unit tests only
#   docker build --build-arg SW_TOKEN=pat_... -t earshot-build .   # + module + .deb
#
# Then copy the artifacts out:
#   id=$(docker create earshot-build); docker cp $id:/out ./out; docker rm $id
#
# DEBIAN picks the release to build on (12 = bookworm, 13 = trixie):
#   docker build --build-arg DEBIAN=13 --build-arg SW_TOKEN=pat_... -t earshot-build .
# The module is built against whatever FreeSWITCH the SignalWire channel serves
# for that release, and against that release's libwebsockets. /out/BUILD_INFO
# records both, so a binary can always be traced to what it was built with.
ARG DEBIAN=12
FROM debian:${DEBIAN}-slim AS build
ARG SW_TOKEN=""

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake pkg-config git ca-certificates gnupg curl \
      libwebsockets-dev libopus-dev libsoxr-dev \
      file dpkg-dev \
 && rm -rf /var/lib/apt/lists/*

# FreeSWITCH dev headers (optional — skipped without a SignalWire token; the module
# target is then simply not built and only the portable unit tests run).
RUN if [ -n "$SW_TOKEN" ]; then set -eux; \
      curl -sSL --user "signalwire:$SW_TOKEN" \
        https://freeswitch.signalwire.com/repo/deb/debian-release/signalwire-freeswitch-repo.gpg \
        -o /usr/share/keyrings/signalwire.gpg; \
      echo "machine freeswitch.signalwire.com login signalwire password $SW_TOKEN" > /etc/apt/auth.conf; \
      . /etc/os-release; \
      echo "deb [signed-by=/usr/share/keyrings/signalwire.gpg] https://freeswitch.signalwire.com/repo/deb/debian-release/ $VERSION_CODENAME main" \
        > /etc/apt/sources.list.d/freeswitch.list; \
      apt-get update && apt-get install -y --no-install-recommends libfreeswitch-dev; \
      rm -rf /var/lib/apt/lists/*; \
    fi

WORKDIR /src
COPY . .

RUN set -eux; \
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release; \
    cmake --build build --parallel; \
    ctest --test-dir build --output-on-failure; \
    mkdir -p /out; \
    . /etc/os-release; \
    { echo "debian=$VERSION_ID ($VERSION_CODENAME)"; \
      echo "freeswitch=$(dpkg-query -W -f='${Version}' libfreeswitch-dev 2>/dev/null || echo none)"; \
      echo "libwebsockets=$(dpkg-query -W -f='${Version}' libwebsockets-dev)"; \
      echo "gcc=$(gcc -dumpfullversion)"; } > /out/BUILD_INFO; \
    cat /out/BUILD_INFO; \
    if [ -f build/mod_earshot.so ]; then \
      cp build/mod_earshot.so /out/; \
      (cd build && cpack -G DEB && cp mod-earshot_*.deb /out/) || true; \
    fi

# Artifact stage: `docker cp` /out out of a container made from this image. Uses debian
# (not scratch) so `docker create` has a command to instantiate — a scratch image has none
# ("Error response from daemon: no command specified"). The base layer is already cached
# from the build stage, so this adds no pull.
FROM debian:${DEBIAN}-slim AS artifacts
COPY --from=build /out /out
