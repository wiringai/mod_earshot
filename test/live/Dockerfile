# Live test rig for mod_earshot: a real FreeSWITCH that places calls to itself.
#
# Built on a public FreeSWITCH image that ships its headers, so no SignalWire token is
# needed. That image carries its own libwebsockets (4.3.3) without headers, for modules we
# do not load; it is removed so mod_earshot builds and runs against the distribution's
# libwebsockets, headers and library from the same package.
#
#   test/live/run.sh           builds this image, the module, and runs the calls
FROM drachtio/drachtio-freeswitch-mrf:latest

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake pkg-config libwebsockets-dev \
      python3 python3-websockets procps gdb \
 && rm -rf /var/lib/apt/lists/* \
 && rm -f /usr/local/lib/libwebsockets* /usr/local/lib/pkgconfig/libwebsockets* \
 && rm -rf /usr/local/lib/cmake/libwebsockets && ldconfig

COPY modules.conf.xml /usr/local/freeswitch/conf/autoload_configs/modules.conf.xml
COPY dialplan.xml     /usr/local/freeswitch/conf/dialplan/mrf.xml
ENV PKG_CONFIG_PATH=/usr/local/freeswitch/lib/pkgconfig
