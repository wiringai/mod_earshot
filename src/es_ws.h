/*
 * es_ws — a small WebSocket client for Earshot, over libwebsockets.
 *
 * Connections share a small pool of libwebsockets service threads (sized to
 * CPU cores): all socket I/O happens there, never on the FreeSWITCH media
 * thread. Outbound frames are queued and flushed on WRITEABLE; inbound frames
 * and lifecycle are delivered via callbacks. See ../docs/ARCHITECTURE.md.
 *
 * Copyright (c) 2026 Varun Pratap Singh. MIT License.
 */
#ifndef ES_WS_H
#define ES_WS_H

#include <stddef.h>
#include <stdint.h>

typedef struct es_ws es_ws_t;

/* Callbacks run on the service thread; keep them fast and non-blocking. */
typedef void (*es_ws_on_text_fn)(void *user, const char *data, size_t len);
typedef void (*es_ws_on_binary_fn)(void *user, const void *data, size_t len);
/* connected=1 on open; connected=0 on close/error (code+reason may be set).
 * What the owner sends from inside the connected=1 call is the first thing the new
 * connection carries (the place for a protocol preamble). On a reconnect, anything that
 * was still queued for the lost connection is dropped, not replayed. */
typedef void (*es_ws_on_event_fn)(void *user, int connected, int code, const char *reason);

typedef struct {
    const char        *url;          /* ws:// or wss://, at most ES_URL_MAX bytes (es_util.h) */
    const char        *auth;         /* Authorization header value, or NULL */
    int                reconnect;    /* 1 = auto-reconnect with backoff + jitter */
    int                max_queue_bytes; /* cap on the outbound backlog (0 = default);
                                        * drop-oldest past this so a slow agent can't make
                                        * it grow without bound. Nothing queued for one
                                        * connection is ever sent on the next: see the
                                        * on_event note below. */

    void              *user;         /* opaque, passed back to callbacks */
    es_ws_on_text_fn   on_text;
    es_ws_on_binary_fn on_binary;
    es_ws_on_event_fn  on_event;

    /* correlation / extra handshake headers (added verbatim, may be NULL) */
    const char        *hdr_call_id;      /* -> X-Call-ID */
    const char        *hdr_channel_uuid; /* -> X-Channel-UUID */
    const char        *hdr_correlation;  /* -> X-Correlation-ID */
    const char        *hdr_meta;         /* -> X-Earshot-Meta (opaque caller context, may be NULL) */
    /* optional single protocol-specific handshake header (currently unused; extension point).
     * When set, the name includes the trailing ':'. */
    const char        *hdr_extra_name;
    const char        *hdr_extra_value;
    /* ws subprotocol to advertise on the wire (Sec-WebSocket-Protocol). NULL = none.
     * OpenAI Realtime answers with "realtime"; we must offer exactly that or lws
     * rejects the mismatch. Most agents use no subprotocol. */
    const char        *subprotocol;

    /* TLS (wss). Server certs are verified against the system trust store (covers the public
     * CAs the AI vendors use). Box-level mTLS + custom-CA are configured out-of-band via the
     * EARSHOT_TLS_CLIENT_CERT / _KEY / EARSHOT_TLS_CA environment variables, applied to every
     * pooled context at init (see es_ws.c) — one client identity per box, since lws binds
     * client TLS material at context (not per-wsi) level. Distinct per-stream certs would
     * still require partitioning streams onto their own context (a future item). */
    int                insecure;     /* skip cert/hostname checks (dev only) */
    /* Timers, in milliseconds. 0 keeps the default; out-of-range values are clamped.
     *   connect_timeout_ms  how long one connection attempt may take, from the TCP connect to
     *                       the end of the WebSocket handshake, before it is abandoned (and,
     *                       with reconnect, retried). Default 20000, range 1000..120000. An
     *                       agent behind a cold-starting platform can hold the handshake open
     *                       for tens of seconds; abandoning it early only starts a second
     *                       session next to the first.
     *   liveness_ms         a connected peer that sends nothing (no frame, no pong) for this
     *                       long is taken for dead and dropped. Default 20000, minimum 2000;
     *                       negative switches the check off.
     *   ping_ms             how often a ping is sent while connected. Default 5000, minimum
     *                       1000, and never more than half of liveness_ms: liveness is only
     *                       meaningful if pings are sent well inside it. */
    int                connect_timeout_ms;
    int                liveness_ms;
    int                ping_ms;
} es_ws_opts_t;

#define ES_WS_CONNECT_TIMEOUT_DEFAULT_MS 20000
#define ES_WS_CONNECT_TIMEOUT_MIN_MS      1000
#define ES_WS_CONNECT_TIMEOUT_MAX_MS    120000
#define ES_WS_LIVENESS_DEFAULT_MS        20000
#define ES_WS_LIVENESS_MIN_MS             2000
#define ES_WS_PING_DEFAULT_MS             5000
#define ES_WS_PING_MIN_MS                 1000

/* transport counters for metrics (all monotonically increasing) */
typedef struct {
    int      connected;
    unsigned reconnects;    /* reconnect attempts after the first connect */
    uint64_t queue_drops;   /* outbound frames dropped: over the queue cap, or queued for a
                             * connection that was lost before they could be sent */
    size_t   queued_bytes;  /* current outbound backlog */
    long     rtt_ms;        /* last ws ping/pong round-trip (ms), 0 if not yet measured */
} es_ws_stats_t;

es_ws_t *es_ws_create(const es_ws_opts_t *opts);   /* copies opts */
int      es_ws_start(es_ws_t *ws);                  /* spawn thread + connect; 0 ok */
int      es_ws_send_text(es_ws_t *ws, const char *data, size_t len);
int      es_ws_send_binary(es_ws_t *ws, const void *data, size_t len);
int      es_ws_connected(es_ws_t *ws);
void     es_ws_get_stats(es_ws_t *ws, es_ws_stats_t *out);
void     es_ws_stop(es_ws_t *ws);                   /* disconnect + detach from the pool */
void     es_ws_destroy(es_ws_t *ws);

/* Tear down the shared service-thread pool. Call once at module unload, AFTER every
 * stream has been stopped (mod_earshot's shutdown does that itself) — the pool's threads
 * outlive individual streams, so they must be joined before the module's code is unmapped,
 * and a stream still holding an es_ws_t would be left with a freed handle. */
void     es_ws_global_shutdown(void);

/* Create the shared service-thread pool up front (idempotent). Call at module load:
 * lazy creation inside the first call's session thread delays that call's media
 * start by the pool build time (~1s of lost caller audio on a small box). */
int      es_ws_global_init(void);

#endif /* ES_WS_H */
