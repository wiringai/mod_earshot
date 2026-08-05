#!/usr/bin/env python3
"""Minimal wss echo server that REQUIRES a client certificate (mutual TLS).

Serves TLS on :9443 using server.pem/server.key and verifies the client cert
against ca.pem. A client that presents no cert — or a cert not signed by ca.pem —
is rejected during the TLS handshake, before `echo` ever runs. So the line
"client connected" printing at all means mTLS succeeded.

  pip install websockets
  ./gen-certs.sh && python3 mtls_echo_server.py
"""
import asyncio, ssl, pathlib, websockets

HERE = pathlib.Path(__file__).parent
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(HERE / "server.pem", HERE / "server.key")
ctx.load_verify_locations(HERE / "ca.pem")
ctx.verify_mode = ssl.CERT_REQUIRED          # require a client cert => mTLS

async def echo(ws, *args):                    # *args: tolerate the 2-arg (ws, path) handler on websockets<11
    print("client connected (mTLS handshake passed)")
    async for msg in ws:                      # bounce audio straight back
        await ws.send(msg)

async def main():
    # bind "localhost" (both ::1 and 127.0.0.1) — a wss client resolving localhost to IPv6 must
    # still reach us; 0.0.0.0 is IPv4-only and would leave an IPv6-first client unable to connect.
    async with websockets.serve(echo, "localhost", 9443, ssl=ctx):
        print("mTLS echo server on wss://localhost:9443/  (Ctrl-C to stop)")
        await asyncio.Future()

asyncio.run(main())
