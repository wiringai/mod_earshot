/*
 * es_ws — a small WebSocket client for Earshot, over libwebsockets.
 *
 * Owns one connection on its own service thread: all socket I/O happens there,
 * never on the FreeSWITCH media thread. Outbound frames are queued and flushed
 * on WRITEABLE; inbound frames + lifecycle are delivered via callbacks.
 *
 * STATUS: reviewed draft — compiles/validates on a FreeSWITCH+libwebsockets dev
 * box, not on the authoring machine. See ../docs/ARCHITECTURE.md.
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
/* connected=1 on open; connected=0 on close/error (code+reason may be set). */
typedef void (*es_ws_on_event_fn)(void *user, int connected, int code, const char *reason);

typedef struct {
    const char        *url;          /* ws:// or wss:// */
    const char        *auth;         /* Authorization header value, or NULL */
    int                reconnect;    /* 1 = auto-reconnect with backoff + jitter */
    int                max_queue_bytes; /* cap on the outbound backlog (0 = default);
                                        * drop-oldest past this so a long outage can't
                                        * grow without bound / replay stale audio */

    void              *user;         /* opaque, passed back to callbacks */
    es_ws_on_text_fn   on_text;
    es_ws_on_binary_fn on_binary;
    es_ws_on_event_fn  on_event;

    /* correlation / extra handshake headers (added verbatim, may be NULL) */
    const char        *hdr_call_id;      /* -> X-Call-ID */
    const char        *hdr_channel_uuid; /* -> X-Channel-UUID */
    const char        *hdr_correlation;  /* -> X-Correlation-ID */
    /* optional single protocol-specific handshake header (currently unused; extension point).
     * When set, the name includes the trailing ':'. */
    const char        *hdr_extra_name;
    const char        *hdr_extra_value;
    /* ws subprotocol to advertise on the wire (Sec-WebSocket-Protocol). NULL = none.
     * OpenAI Realtime answers with "realtime"; we must offer exactly that or lws
     * rejects the mismatch. Most agents use no subprotocol. */
    const char        *subprotocol;

    /* TLS (wss) */
    const char        *ca_file;
    const char        *cert_file;    /* client cert (mTLS), optional */
    const char        *key_file;
    int                insecure;     /* skip cert/hostname checks (dev only) */
} es_ws_opts_t;

/* transport counters for metrics (all monotonically increasing) */
typedef struct {
    int      connected;
    unsigned reconnects;    /* reconnect attempts after the first connect */
    uint64_t queue_drops;   /* outbound frames dropped to stay under the queue cap */
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
 * stream has been stopped — the pool's threads outlive individual streams, so they
 * must be joined before the module's code is unmapped. */
void     es_ws_global_shutdown(void);

#endif /* ES_WS_H */
