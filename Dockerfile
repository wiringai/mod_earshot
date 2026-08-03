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
FROM debian:12-slim AS build
ARG SW_TOKEN=""

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake pkg-config git ca-certificates gnupg curl \
      libwebsockets-dev libopus-dev libsoxr-dev \
 && rm -rf /var/lib/apt/lists/*

# FreeSWITCH dev headers (optional — skipped without a SignalWire token; the module
# target is then simply not built and only the portable unit tests run).
RUN if [ -n "$SW_TOKEN" ]; then set -eux; \
      curl -sSL --user "signalwire:$SW_TOKEN" \
        https://freeswitch.signalwire.com/repo/deb/debian-release/signalwire-freeswitch-repo.gpg \
        -o /usr/share/keyrings/signalwire.gpg; \
      echo "machine freeswitch.signalwire.com login signalwire password $SW_TOKEN" > /etc/apt/auth.conf; \
      echo "deb [signed-by=/usr/share/keyrings/signalwire.gpg] https://freeswitch.signalwire.com/repo/deb/debian-release/ bookworm main" \
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
    if [ -f build/mod_earshot.so ]; then \
      cp build/mod_earshot.so /out/; \
      (cd build && cpack -G DEB && cp mod-earshot_*.deb /out/) || true; \
    fi

# Minimal artifact stage: `docker cp` /out out of a container made from this image.
FROM scratch AS artifacts
COPY --from=build /out /out
