/*
 * es_ws — libwebsockets client transport for Earshot.
 *
 * Shared-event-loop model: instead of one pthread + one lws_context per stream
 * (which capped concurrency at threads/RAM), all streams share a small POOL of
 * service contexts (sized to CPU cores). Each stream is one wsi on a pool
 * context; a single service thread per pool context drives many wsi.
 *
 * Threading rule (the whole design hinges on it): every lws call for a context —
 * connect, writable, close — happens ONLY on that context's service thread. Other
 * threads communicate exactly one way: mutate shared state under a lock, then
 * lws_cancel_service() (the one lws call that is safe cross-thread) to wake the
 * loop, which does the actual lws work. Connect and teardown are therefore async:
 * the caller flags intent and the service thread carries it out.
 *
 * Copyright (c) 2026 Varun Pratap Singh. MIT License.
 */
#include "es_ws.h"

#include <libwebsockets.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <time.h>

/* Upper bound on a single reassembled inbound message (runaway/hostile guard). */
#define ES_WS_MAX_MSG (4u * 1024u * 1024u)
#define ES_POOL_MAX   16     /* hard cap on service contexts */
#define ES_STABLE_MS  5000   /* a connection must stay up this long before backoff resets,
                              * so a flapping (accept-then-close) endpoint keeps backing off */
#define ES_PING_MS    5000   /* send a WebSocket ping this often while connected */
#define ES_LIVENESS_MS 20000 /* no inbound frame or pong for this long => peer is dead
                              * (hung agent / half-open TCP): drop the wsi and reconnect */

static long es_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* one queued outbound message (LWS_PRE-padded so lws_write can prepend framing) */
typedef struct es_ws_msg {
    struct es_ws_msg *next;
    size_t            len;
    int               binary;
    unsigned char     buf[];        /* [LWS_PRE .. LWS_PRE+len) */
} es_ws_msg_t;

struct es_ws {
    es_ws_opts_t            o;       /* owned copy (strings duped) */
    struct es_pool_ctx     *pool;    /* service context we run on (shared) */
    struct es_ws           *lnext;   /* pool stream-list linkage (pool->mu) */
    struct es_ws           *arm_next;/* pending-arm list linkage          (pool->mu) */
    int                     in_arm;  /* on the pending-arm list           (pool->mu) */
    struct lws             *wsi;     /* current connection (service thread) */
    pthread_mutex_t         mu;      /* guards the queue + the metrics fields */
    es_ws_msg_t            *head, *tail;   /* FIFO outbound queue (both under w->mu) */
    _Atomic int             have_queued;   /* queue non-empty; read lock-free by the service thread */
    size_t                  queued_bytes;
    size_t                  max_queue_bytes;
    _Atomic int             connected;    /* set by es_cb (service thread), read by owners  */
    volatile int            alive;        /* stream wants to stay up (0 once stopping) [p->mu] */
    volatile int            want_connect; /* service thread should (re)connect now  [p->mu] */
    volatile int            teardown;     /* stop requested by the owning thread     [p->mu] */
    _Atomic int             done;         /* service released the wsi + unlinked us (spun on) */
    volatile int            orphaned;     /* owner gave up waiting; service frees    [p->mu] */
    int                     kill;         /* force-close the wsi (service thread only) */
    _Atomic int             suppress_events; /* mute owner callbacks once teardown starts */
    int                     backoff_ms;
    long                    next_reconnect_ms;
    long                    connected_since_ms;   /* when the current link established (0 if down) */
    unsigned                rng;
    unsigned                reconnects;
    uint64_t                queue_drops;
    long                    rtt_ms;
    long                    ping_sent_ms;
    long                    last_ping_ms;
    long                    last_rx_ms;    /* last inbound frame/pong; drives liveness timeout */
    int                     want_ping;
    /* inbound reassembly (service thread only) */
    unsigned char          *rx;
    size_t                  rx_len, rx_cap;
    int                     rx_drop;
    /* parsed url */
    char                    host[256];
    char                    path[512];
    int                     port;
    int                     use_tls;
};

/* one shared service context: an lws_context + its service thread + the streams on it */
typedef struct es_pool_ctx {
    struct lws_context *ctx;
    pthread_t           thread;
    _Atomic int         running;   /* service loop reads it unlocked; shutdown writes it */
    pthread_mutex_t     mu;      /* guards `list` + each stream's lifecycle flags */
    es_ws_t            *list;    /* attached streams (via w->lnext) */
    es_ws_t            *arm_list;/* streams awaiting a writable arm (via w->arm_next) */
    long                last_walk_ms;  /* service thread only: housekeeping rate limit */
} es_pool_ctx_t;

static struct {
    pthread_mutex_t mu;
    int             inited;
    int             n;
    es_pool_ctx_t   c[ES_POOL_MAX];
    unsigned        rr;
} g_pool = { PTHREAD_MUTEX_INITIALIZER, 0, 0, {{0}}, 0 };

static char *es_strdup(const char *s) { return s ? strdup(s) : NULL; }

/* ---- outbound queue ---------------------------------------------------- */
static int es_enqueue(es_ws_t *w, const void *data, size_t len, int binary)
{
    int was_queued;
    es_ws_msg_t *m = malloc(sizeof(*m) + LWS_PRE + len);
    if (!m) return -1;
    m->next = NULL; m->len = len; m->binary = binary;
    memcpy(m->buf + LWS_PRE, data, len);

    pthread_mutex_lock(&w->mu);
    if (w->tail) w->tail->next = m; else w->head = m;
    w->tail = m;
    w->queued_bytes += len;
    while (w->max_queue_bytes && w->queued_bytes > w->max_queue_bytes && w->head != w->tail) {
        es_ws_msg_t *old = w->head;
        w->head = old->next;
        w->queued_bytes -= old->len;
        w->queue_drops++;
        free(old);
    }
    was_queued = w->have_queued;
    w->have_queued = 1;   /* atomic pending-flag; the service thread reads this, not w->head */
    pthread_mutex_unlock(&w->mu);

    /* Wake the shared loop only on the empty->non-empty transition, and register on
     * the pool's pending-arm list so the EVENT_WAIT_CANCELLED handler arms exactly
     * this stream — O(pending), not O(all streams) — the moment the service thread
     * wakes. While the queue stays non-empty the WRITEABLE chain keeps draining with
     * no wakes at all; the rate-limited walk's have_queued check remains the backstop,
     * so no wakeup is ever lost. (Never holds w->mu and p->mu together: w->mu was
     * released above, and the service thread's order is p->mu -> w->mu.) */
    if (!was_queued && w->pool && w->pool->ctx) {
        es_pool_ctx_t *p = w->pool;
        pthread_mutex_lock(&p->mu);
        if (!w->in_arm && !w->teardown) { w->in_arm = 1; w->arm_next = p->arm_list; p->arm_list = w; }
        pthread_mutex_unlock(&p->mu);
        lws_cancel_service(p->ctx);
    }
    return 0;
}

static es_ws_msg_t *es_dequeue(es_ws_t *w)
{
    es_ws_msg_t *m;
    pthread_mutex_lock(&w->mu);
    m = w->head;
    if (m) { w->head = m->next; if (!w->head) { w->tail = NULL; w->have_queued = 0; } w->queued_bytes -= m->len; }
    pthread_mutex_unlock(&w->mu);
    return m;
}

static void es_drain(es_ws_t *w)
{
    es_ws_msg_t *m;
    while ((m = es_dequeue(w))) free(m);
}

/* Free everything owned by a stream. Called by exactly one thread per stream: the
 * owner (es_ws_destroy) if it reclaimed the stream, otherwise the service thread
 * once it finishes teardown. Never called while a wsi still references w. */
static void es_free_full(es_ws_t *w)
{
    es_drain(w);
    free(w->rx);
    if (w->pool) pthread_mutex_destroy(&w->mu);   /* mu is only initialised once started */
    free((void *) w->o.url);
    free((void *) w->o.auth);
    free((void *) w->o.hdr_call_id);
    free((void *) w->o.hdr_channel_uuid);
    free((void *) w->o.hdr_correlation);
    free((void *) w->o.hdr_meta);
    free((void *) w->o.hdr_extra_name);
    free((void *) w->o.hdr_extra_value);
    free((void *) w->o.subprotocol);
    free(w);
}

/* ---- lws protocol callback (per-wsi `user` = the owning es_ws) ---------- */
static int es_cb(struct lws *wsi, enum lws_callback_reasons reason,
                 void *user, void *in, size_t len)
{
    es_ws_t *w = (es_ws_t *) user;   /* set via i.userdata at connect time */

    switch (reason) {
    case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER: {
        unsigned char **p = (unsigned char **) in, *end = (*p) + len;
        int rc = 0;
        if (!w) break;
        if (w->o.auth)
            rc |= lws_add_http_header_by_name(wsi, (const unsigned char *)"Authorization:",
                (const unsigned char *)w->o.auth, (int)strlen(w->o.auth), p, end);
        if (w->o.hdr_call_id)
            rc |= lws_add_http_header_by_name(wsi, (const unsigned char *)"X-Call-ID:",
                (const unsigned char *)w->o.hdr_call_id, (int)strlen(w->o.hdr_call_id), p, end);
        if (w->o.hdr_channel_uuid)
            rc |= lws_add_http_header_by_name(wsi, (const unsigned char *)"X-Channel-UUID:",
                (const unsigned char *)w->o.hdr_channel_uuid, (int)strlen(w->o.hdr_channel_uuid), p, end);
        if (w->o.hdr_correlation)
            rc |= lws_add_http_header_by_name(wsi, (const unsigned char *)"X-Correlation-ID:",
                (const unsigned char *)w->o.hdr_correlation, (int)strlen(w->o.hdr_correlation), p, end);
        if (w->o.hdr_meta)
            rc |= lws_add_http_header_by_name(wsi, (const unsigned char *)"X-Earshot-Meta:",
                (const unsigned char *)w->o.hdr_meta, (int)strlen(w->o.hdr_meta), p, end);
        if (w->o.hdr_extra_name && w->o.hdr_extra_value)
            rc |= lws_add_http_header_by_name(wsi, (const unsigned char *)w->o.hdr_extra_name,
                (const unsigned char *)w->o.hdr_extra_value, (int)strlen(w->o.hdr_extra_value), p, end);
        if (rc) return -1;
        break;
    }

    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        if (!w) break;
        w->connected = 1;
        w->connected_since_ms = es_now_ms();   /* backoff resets only once this proves stable */
        w->last_rx_ms = w->connected_since_ms; /* liveness clock starts now */
        w->last_ping_ms = 0;
        /* Gate on suppress_events like every other callback: a wsi that establishes
         * after teardown began (the orphan window) must not call into freed owner state. */
        if (!w->suppress_events && w->o.on_event) w->o.on_event(w->o.user, 1, 0, "established");
        if (w->have_queued) lws_callback_on_writable(wsi);
        break;

    case LWS_CALLBACK_CLIENT_RECEIVE: {
        int final;
        if (!w) break;
        if (w->kill || w->suppress_events) return -1;  /* teardown: refuse further rx, close, and never call a
                                                        * sink. suppress_events is set synchronously in es_ws_stop
                                                        * BEFORE the owner frees its state; gating rx on it (not
                                                        * just the async kill) closes the orphan-timeout window
                                                        * where es_ws_destroy returns without kill/done and a late
                                                        * receive would touch a freed play_buf. Liveness/reconnect
                                                        * uses kill only, so this never blocks a recoverable drop. */
        w->last_rx_ms = es_now_ms();                   /* inbound data = peer is alive */
        final = lws_is_final_fragment(wsi) && !lws_remaining_packet_payload(wsi);
        if (!w->rx_drop) {
            size_t need = w->rx_len + len + 1;
            if (need > ES_WS_MAX_MSG) {
                w->rx_drop = 1;
            } else if (need > w->rx_cap) {
                size_t ncap = w->rx_cap ? w->rx_cap : 4096;
                unsigned char *nb;
                while (ncap < need) ncap *= 2;
                nb = realloc(w->rx, ncap);
                if (!nb) w->rx_drop = 1;
                else { w->rx = nb; w->rx_cap = ncap; }
            }
            if (!w->rx_drop && len) { memcpy(w->rx + w->rx_len, in, len); w->rx_len += len; }
        }
        if (final) {
            if (!w->rx_drop) {
                w->rx[w->rx_len] = '\0';   /* always NUL-terminate (buffer sized rx_len+len+1); some
                                            * peers, e.g. Gemini, deliver JSON as a binary frame */
                if (lws_frame_is_binary(wsi)) {
                    if (w->o.on_binary) w->o.on_binary(w->o.user, w->rx, w->rx_len);
                } else if (w->o.on_text) {
                    w->o.on_text(w->o.user, (const char *) w->rx, w->rx_len);
                }
            }
            w->rx_len = 0; w->rx_drop = 0;
        }
        break;
    }

    case LWS_CALLBACK_CLIENT_WRITEABLE: {
        es_ws_msg_t *m;
        if (!w) break;
        if (w->kill) return -1;                        /* teardown: close now */
        if (w->want_ping) {
            unsigned char pbuf[LWS_PRE + sizeof(long)];
            w->want_ping = 0;
            w->ping_sent_ms = es_now_ms();
            memcpy(pbuf + LWS_PRE, &w->ping_sent_ms, sizeof(long));
            lws_write(wsi, pbuf + LWS_PRE, sizeof(long), LWS_WRITE_PING);
            lws_callback_on_writable(wsi);
            break;
        }
        m = es_dequeue(w);
        if (m) {
            enum lws_write_protocol wp = m->binary ? LWS_WRITE_BINARY : LWS_WRITE_TEXT;
            int n = lws_write(wsi, m->buf + LWS_PRE, m->len, wp);
            free(m);
            if (n < 0) return -1;
            if (w->have_queued) lws_callback_on_writable(wsi);
        }
        break;
    }

    case LWS_CALLBACK_CLIENT_RECEIVE_PONG:
        if (!w) break;
        w->last_rx_ms = es_now_ms();                   /* pong = peer is alive */
        if (w->ping_sent_ms) {
            pthread_mutex_lock(&w->mu);
            w->rtt_ms = w->last_rx_ms - w->ping_sent_ms;
            pthread_mutex_unlock(&w->mu);
        }
        break;

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
    case LWS_CALLBACK_CLIENT_CLOSED:
        /* Only flag state + notify here. Unlink/finalize/free happens on the service
         * thread in es_pool_service (outside lws_service), so w is never freed while
         * lws might still deliver a callback for this wsi. */
        if (!w) break;
        w->connected = 0;
        w->wsi = NULL;
        if (!w->suppress_events && w->o.on_event) {
            int code = (reason == LWS_CALLBACK_CLIENT_CONNECTION_ERROR) ? -1 : 0;
            w->o.on_event(w->o.user, 0, code,
                          in ? (const char *) in : (code < 0 ? "connect error" : "closed"));
        }
        break;

    case LWS_CALLBACK_WSI_DESTROY:
        /* Backstop: killing a still-connecting wsi (LWS_TO_KILL_ASYNC) can deliver only
         * WSI_DESTROY, not CLOSED/CONNECTION_ERROR. Null our wsi here too so es_pool_service
         * can finalize teardown instead of stalling until es_ws_destroy times out. Match on
         * the exact wsi so a newer reconnect's wsi is never cleared. */
        if (w && w->wsi == wsi) { w->connected = 0; w->wsi = NULL; }
        break;

    case LWS_CALLBACK_EVENT_WAIT_CANCELLED: {
        /* Delivered ON the service thread, inside lws_service, immediately after any
         * lws_cancel_service wake (w/user are NULL — this is a context broadcast).
         * Arm writables for the streams that registered on the pending-arm list —
         * O(streams needing arming), not O(all streams), so per-frame audio cadence
         * stays exact (peer-review measured 25-60ms gaps and a ~1s stall when arming
         * waited for the 10ms-quantized walk) without re-coupling wake cost to the
         * stream count. Streams are freed only by the walk's finalize, which purges
         * them from this list first, so entries can never dangle. */
        struct lws_context *cx = wsi ? lws_get_context(wsi) : NULL;
        es_pool_ctx_t *pp = cx ? (es_pool_ctx_t *) lws_context_user(cx) : NULL;
        es_ws_t *s;
        if (!pp) break;
        pthread_mutex_lock(&pp->mu);
        while ((s = pp->arm_list)) {
            pp->arm_list = s->arm_next;
            s->in_arm = 0; s->arm_next = NULL;
            if (s->have_queued && s->connected && s->wsi && !s->kill)
                lws_callback_on_writable(s->wsi);
        }
        pthread_mutex_unlock(&pp->mu);
        break;
    }

    default:
        break;
    }
    return 0;
}

static const struct lws_protocols es_protocols[] = {
    { "earshot", es_cb, 0, 65536 },   /* per_session_data_size 0 -> i.userdata is the wsi user */
    { 0 }
};

/* ---- connect (service thread only) ------------------------------------- */
static void es_connect(es_ws_t *w)
{
    struct lws_client_connect_info i;
    memset(&i, 0, sizeof i);
    i.context  = w->pool->ctx;
    i.address  = w->host;
    i.port     = w->port;
    i.path     = w->path;
    i.host     = w->host;
    i.origin   = w->host;
    i.protocol = w->o.subprotocol ? w->o.subprotocol : es_protocols[0].name;
    i.local_protocol_name = es_protocols[0].name;
    i.pwsi     = &w->wsi;
    i.userdata = w;              /* -> es_cb `user` for this wsi */
    if (w->use_tls) {
        i.ssl_connection = LCCSCF_USE_SSL;
        if (w->o.insecure)
            i.ssl_connection |= LCCSCF_ALLOW_SELFSIGNED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;
    }
    if (!lws_client_connect_via_info(&i)) {
        /* Be authoritative: on some immediate-failure paths lws returns NULL without
         * firing CONNECTION_ERROR and without nulling *pwsi. Clear it ourselves so the
         * reconnect guard (!w->wsi) can fire — otherwise the stream never retries. */
        w->wsi = NULL;
        if (w->o.on_event && !w->suppress_events)
            w->o.on_event(w->o.user, 0, -1, "connect failed");
    }
}

/* ---- pool service thread ----------------------------------------------- */
static void *es_pool_service(void *arg)
{
    es_pool_ctx_t *p = (es_pool_ctx_t *) arg;
    int gated = 0;
    while (p->running) {
        /* When the previous iteration skipped the walk, an intent (a queued frame's
         * transition wake, a stop, a start) may already be pending with its wake
         * consumed — nothing else would wake an idle context for up to 50ms. Shorten
         * the service timeout for exactly that window so the deferred walk runs
         * within ~10ms instead. (Peer review: recurring ~50ms audio stall when a
         * pong-phased wake landed in the gated window.) */
        lws_service(p->ctx, gated ? 10 : 50);   /* woken early by lws_cancel_service */
        long now = es_now_ms();
        es_ws_t *prev = NULL, *w;
        /* Rate-limit the housekeeping walk. Under load lws_service returns per socket
         * event, and walking every stream on every return makes housekeeping cost scale
         * with traffic x streams (quadratic-ish). Everything the walk does — connect,
         * teardown, ping/liveness/backoff timers, the have_queued backstop — is fine at
         * 10ms resolution, so cap it there; socket I/O itself is not delayed. */
        if (now - p->last_walk_ms < 10) { gated = 1; continue; }
        gated = 0;
        p->last_walk_ms = now;
        pthread_mutex_lock(&p->mu);
        w = p->list;
        while (w) {
            es_ws_t *next = w->lnext;
            if (w->teardown) {
                if (w->wsi) {
                    if (!w->kill) {           /* force the connection closed on this thread */
                        w->kill = 1; w->suppress_events = 1;
                        lws_set_timeout(w->wsi, PENDING_TIMEOUT_CLOSE_SEND, LWS_TO_KILL_ASYNC);
                        lws_callback_on_writable(w->wsi);
                    }
                    /* wait for CLOSED to clear w->wsi, then finalize below */
                } else {                      /* wsi released: finalize here, off lws_service */
                    if (w->in_arm) {          /* purge from the pending-arm list before any free */
                        es_ws_t **ap = &p->arm_list;
                        while (*ap && *ap != w) ap = &(*ap)->arm_next;
                        if (*ap) *ap = w->arm_next;
                        w->in_arm = 0;
                    }
                    if (prev) prev->lnext = next; else p->list = next;
                    w->done = 1;              /* es_ws_destroy may be waiting on this */
                    if (w->orphaned) es_free_full(w);  /* owner gave up; we own the free now */
                    w = next; continue;       /* removed from list: don't advance prev */
                }
            } else if (w->want_connect) {
                w->want_connect = 0;
                /* es_connect can synchronously re-enter es_cb -> on_event; never hold p->mu
                 * across it (an owner callback that calls es_ws_* would deadlock the pool).
                 * Safe to drop the lock mid-walk: only this thread frees/unlinks streams, so
                 * w and next stay valid; a concurrent es_ws_start only prepends a new head. */
                pthread_mutex_unlock(&p->mu);
                es_connect(w);
                pthread_mutex_lock(&p->mu);
            } else if (w->connected && w->wsi) {
                /* Only a link that has stayed up past ES_STABLE_MS counts as healthy and
                 * earns a backoff reset; a flapping endpoint never gets here long enough,
                 * so its reconnects keep backing off instead of hammering at the 250ms floor. */
                if (w->backoff_ms != 250 && now - w->connected_since_ms >= ES_STABLE_MS)
                    w->backoff_ms = 250;
                if (w->have_queued) lws_callback_on_writable(w->wsi);
                if (w->last_ping_ms == 0) w->last_ping_ms = now;
                if (now - w->last_ping_ms >= ES_PING_MS) {
                    w->last_ping_ms = now; w->want_ping = 1;
                    lws_callback_on_writable(w->wsi);
                }
                /* Liveness: a live peer answers pings with pongs (and usually sends audio),
                 * so last_rx_ms stays fresh. If nothing has arrived for ES_LIVENESS_MS the
                 * peer is hung or the TCP is half-open — force the wsi closed so the reconnect
                 * path below recovers it. kill closes via WRITEABLE returning -1; it is cleared
                 * before the next connect so the fresh wsi is not killed too. */
                else if (!w->kill && now - w->last_rx_ms >= ES_LIVENESS_MS) {
                    w->kill = 1;
                    lws_callback_on_writable(w->wsi);
                }
            } else if (!w->connected && !w->wsi && w->o.reconnect && now >= w->next_reconnect_ms) {
                int jitter = (int) (w->rng = w->rng * 1103515245u + 12345u) % (w->backoff_ms / 2 + 1);
                w->kill = 0;   /* a liveness drop set this; clear it so the new wsi survives */
                w->next_reconnect_ms = now + w->backoff_ms + jitter;   /* non-blocking backoff */
                if (w->backoff_ms < 8000) w->backoff_ms *= 2;
                pthread_mutex_lock(&w->mu); w->reconnects++; pthread_mutex_unlock(&w->mu);
                pthread_mutex_unlock(&p->mu);   /* same reason as want_connect: no callbacks under p->mu */
                es_connect(w);
                pthread_mutex_lock(&p->mu);
            }
            prev = w; w = next;
        }
        pthread_mutex_unlock(&p->mu);
    }
    return NULL;
}

/* ---- box-level client TLS (mTLS / custom CA) --------------------------- *
 * libwebsockets binds client TLS material at the context (not per-wsi) level, and
 * streams share pooled contexts — so this is a box-level identity: read once from the
 * environment at pool init and applied to every pooled context. All-NULL leaves the
 * default behaviour unchanged (server cert verified against the system trust store, no
 * client cert). The client key must be an unencrypted PEM (passphrase keys unsupported).
 * WARNING: EARSHOT_TLS_CA replaces the system trust store for EVERY connection on the box,
 * so public-CA endpoints (OpenAI, Deepgram) then fail verification — set it only when all
 * agent endpoints chain to that private CA. */
static char *g_tls_client_cert;   /* EARSHOT_TLS_CLIENT_CERT — PEM client cert (enables mTLS)      */
static char *g_tls_client_key;    /* EARSHOT_TLS_CLIENT_KEY  — matching private key (unencrypted)  */
static char *g_tls_ca;            /* EARSHOT_TLS_CA          — verify server against this CA instead
                                   *                           of the system trust store           */

/* A configured TLS file that isn't readable would make lws_create_context fail and take the
 * whole pool (every stream, mTLS or not) down. Check first and drop unreadable material with a
 * specific error, so the transport still comes up. */
static int es_tls_readable(const char *var, const char *path)
{
    if (access(path, R_OK) == 0) return 1;
    lwsl_err("earshot: %s=%s is not readable — ignoring this TLS material\n", var, path);
    return 0;
}

static void es_tls_load_env(void)
{
    const char *cert = getenv("EARSHOT_TLS_CLIENT_CERT");
    const char *key  = getenv("EARSHOT_TLS_CLIENT_KEY");
    const char *ca   = getenv("EARSHOT_TLS_CA");
    int have_cert = cert && *cert, have_key = key && *key;

    /* mTLS needs the cert+key as a pair; a half-config would silently present no cert. */
    if (have_cert != have_key) {
        lwsl_err("earshot: mTLS needs BOTH EARSHOT_TLS_CLIENT_CERT and EARSHOT_TLS_CLIENT_KEY; "
                 "only one is set — presenting no client certificate\n");
        have_cert = have_key = 0;
    }
    /* Drop material we can't read (with a specific error) rather than failing the whole context.
     * NOTE: a readable-but-invalid cert (mismatched pair, passphrase key) can still fail
     * lws_create_context — configure mTLS on a box whose agents all use it. */
    if (have_cert && (!es_tls_readable("EARSHOT_TLS_CLIENT_CERT", cert) ||
                      !es_tls_readable("EARSHOT_TLS_CLIENT_KEY",  key)))
        have_cert = have_key = 0;
    if (ca && *ca && !es_tls_readable("EARSHOT_TLS_CA", ca))
        ca = NULL;

    if (have_cert) {                       /* allocate the pair together; never apply a half */
        g_tls_client_cert = es_strdup(cert);
        g_tls_client_key  = es_strdup(key);
        if (!g_tls_client_cert || !g_tls_client_key) {
            free(g_tls_client_cert); g_tls_client_cert = NULL;
            free(g_tls_client_key);  g_tls_client_key  = NULL;
        }
    }
    if (ca && *ca) g_tls_ca = es_strdup(ca);

    /* Make the effective state visible so a mistyped/broken config isn't a silent no-mTLS. */
    if (g_tls_client_cert || g_tls_ca)
        lwsl_notice("earshot: box-level TLS active — client cert %s, custom CA %s\n",
                    g_tls_client_cert ? "on" : "off", g_tls_ca ? "on" : "off");
}

static void es_tls_free_env(void)
{
    free(g_tls_client_cert); g_tls_client_cert = NULL;
    free(g_tls_client_key);  g_tls_client_key  = NULL;
    free(g_tls_ca);          g_tls_ca          = NULL;
}

/* ---- pool lifecycle ---------------------------------------------------- */
static int es_pool_init(void)   /* lazy, once */
{
    long cores;
    int i, k;
    /* No unlocked fast-path read of g_pool.inited: that would race the locked write
     * below and, worse, let a caller observe inited=1 without the release barrier that
     * publishes g_pool.n / g_pool.c[].ctx — i.e. use a half-built pool. Always take the
     * lock; es_pool_init runs once per stream start, so it is not hot. */
    pthread_mutex_lock(&g_pool.mu);
    if (g_pool.inited) { pthread_mutex_unlock(&g_pool.mu); return 0; }

    cores = sysconf(_SC_NPROCESSORS_ONLN);
    g_pool.n = (int) (cores > 0 ? cores : 1);
    if (g_pool.n > ES_POOL_MAX) g_pool.n = ES_POOL_MAX;

    es_tls_load_env();   /* box-level mTLS / custom-CA material for every pooled context */

    for (i = 0; i < g_pool.n; i++) {
        struct lws_context_creation_info info;
        es_pool_ctx_t *p = &g_pool.c[i];
        memset(&info, 0, sizeof info);
        info.port = CONTEXT_PORT_NO_LISTEN;
        info.protocols = es_protocols;
        info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
        if (g_tls_client_cert) info.client_ssl_cert_filepath        = g_tls_client_cert;
        if (g_tls_client_key)  info.client_ssl_private_key_filepath = g_tls_client_key;
        if (g_tls_ca)          info.client_ssl_ca_filepath          = g_tls_ca;
        info.user = p;   /* EVENT_WAIT_CANCELLED broadcast -> find our pool ctx */
        pthread_mutex_init(&p->mu, NULL);
        p->list = NULL;
        p->arm_list = NULL;
        p->last_walk_ms = 0;   /* first walk always runs (reload hygiene) */
        p->ctx = lws_create_context(&info);
        if (!p->ctx) goto fail;
        p->running = 1;
        if (pthread_create(&p->thread, NULL, es_pool_service, p) != 0) {
            p->running = 0; lws_context_destroy(p->ctx); p->ctx = NULL; goto fail;
        }
    }
    g_pool.inited = 1;
    pthread_mutex_unlock(&g_pool.mu);
    return 0;

fail:
    for (k = 0; k <= i; k++) {
        es_pool_ctx_t *p = &g_pool.c[k];
        if (p->running) { p->running = 0; if (p->ctx) lws_cancel_service(p->ctx); pthread_join(p->thread, NULL); }
        if (p->ctx) { lws_context_destroy(p->ctx); p->ctx = NULL; }
        pthread_mutex_destroy(&p->mu);
    }
    es_tls_free_env();
    g_pool.n = 0;
    pthread_mutex_unlock(&g_pool.mu);
    return -1;
}

int es_ws_global_init(void) { return es_pool_init(); }

void es_ws_global_shutdown(void)
{
    int i;
    pthread_mutex_lock(&g_pool.mu);
    for (i = 0; i < g_pool.n; i++) {
        es_pool_ctx_t *p = &g_pool.c[i];
        es_ws_t *w;
        if (p->running) { p->running = 0; if (p->ctx) lws_cancel_service(p->ctx); pthread_join(p->thread, NULL); }
        /* The service thread is gone. Any stream still attached is a straggler (an orphan
         * whose wsi never closed, or a caller that skipped teardown). Mute it, let
         * lws_context_destroy close its wsi (callbacks run here, on this thread, with w still
         * valid and events suppressed), then free — so nothing leaks and no wsi outlives w. */
        for (w = p->list; w; w = w->lnext) { w->suppress_events = 1; w->teardown = 1; }
        if (p->ctx) { lws_context_destroy(p->ctx); p->ctx = NULL; }
        while (p->list) { w = p->list; p->list = w->lnext; es_free_full(w); }
        pthread_mutex_destroy(&p->mu);
    }
    es_tls_free_env();
    g_pool.n = 0; g_pool.inited = 0;
    pthread_mutex_unlock(&g_pool.mu);
}

/* ---- public API -------------------------------------------------------- */
es_ws_t *es_ws_create(const es_ws_opts_t *opts)
{
    es_ws_t *w;
    const char *prot, *ads, *path; int port;
    char tmp[1024];

    if (!opts || !opts->url) return NULL;
    w = calloc(1, sizeof(*w));
    if (!w) return NULL;

    w->o = *opts;
    w->o.url  = es_strdup(opts->url);
    w->o.auth = es_strdup(opts->auth);
    w->o.hdr_call_id      = es_strdup(opts->hdr_call_id);
    w->o.hdr_channel_uuid = es_strdup(opts->hdr_channel_uuid);
    w->o.hdr_correlation  = es_strdup(opts->hdr_correlation);
    w->o.hdr_meta         = es_strdup(opts->hdr_meta);
    w->o.hdr_extra_name   = es_strdup(opts->hdr_extra_name);
    w->o.hdr_extra_value  = es_strdup(opts->hdr_extra_value);
    w->o.subprotocol      = es_strdup(opts->subprotocol);
    w->backoff_ms = 250;
    w->max_queue_bytes = opts->max_queue_bytes > 0 ? (size_t) opts->max_queue_bytes : 262144;
    w->rng = (unsigned) ((size_t) w ^ (size_t) opts->url);

    strncpy(tmp, opts->url, sizeof tmp - 1); tmp[sizeof tmp - 1] = 0;
    if (lws_parse_uri(tmp, &prot, &ads, &port, &path) != 0) { es_ws_destroy(w); return NULL; }
    w->use_tls = (!strcasecmp(prot, "wss") || !strcasecmp(prot, "https"));
    w->port = port;
    snprintf(w->host, sizeof w->host, "%s", ads);
    snprintf(w->path, sizeof w->path, "/%s", path);

    return w;
}

int es_ws_start(es_ws_t *w)
{
    es_pool_ctx_t *p;
    if (!w) return -1;
    if (es_pool_init() != 0) return -1;

    pthread_mutex_init(&w->mu, NULL);
    /* round-robin assignment to a pool context */
    pthread_mutex_lock(&g_pool.mu);
    p = &g_pool.c[g_pool.rr++ % (unsigned) g_pool.n];
    pthread_mutex_unlock(&g_pool.mu);
    w->pool = p;
    w->alive = 1;
    w->want_connect = 1;
    w->next_reconnect_ms = 0;

    pthread_mutex_lock(&p->mu);
    w->lnext = p->list; p->list = w;
    pthread_mutex_unlock(&p->mu);

    lws_cancel_service(p->ctx);   /* wake the service thread to run the connect */
    return 0;
}

int es_ws_send_text(es_ws_t *w, const char *data, size_t len)   { return w ? es_enqueue(w, data, len, 0) : -1; }
int es_ws_send_binary(es_ws_t *w, const void *data, size_t len) { return w ? es_enqueue(w, data, len, 1) : -1; }
int es_ws_connected(es_ws_t *w)                                 { return w && w->connected; }

void es_ws_get_stats(es_ws_t *w, es_ws_stats_t *out)
{
    if (!out) return;
    if (!w) { memset(out, 0, sizeof *out); return; }
    pthread_mutex_lock(&w->mu);
    out->connected    = w->connected;
    out->reconnects   = w->reconnects;
    out->queue_drops  = w->queue_drops;
    out->queued_bytes = w->queued_bytes;
    out->rtt_ms       = w->rtt_ms;
    pthread_mutex_unlock(&w->mu);
}

/* Request teardown: the connection goes away, but w stays a valid handle until
 * es_ws_destroy reclaims it. Idempotent and non-blocking.
 *
 * suppress_events is set HERE, synchronously, before returning: the owner (mod_earshot)
 * frees its per-stream state right after destroy, so the service thread must fire no
 * further on_event/on_text/on_binary once teardown is requested. A callback already
 * in-flight when this flips is still safe — es_ws_destroy waits for the service thread
 * to go quiescent (w->done) before the owner frees anything. */
void es_ws_stop(es_ws_t *w)
{
    es_pool_ctx_t *p;
    if (!w || !w->pool) return;
    p = w->pool;
    pthread_mutex_lock(&p->mu);
    if (w->alive) { w->alive = 0; w->teardown = 1; w->suppress_events = 1; }
    pthread_mutex_unlock(&p->mu);
    lws_cancel_service(p->ctx);           /* wake the service thread to close us */
}

void es_ws_destroy(es_ws_t *w)
{
    es_pool_ctx_t *p;
    int spins;
    if (!w) return;

    if (!w->pool) { es_free_full(w); return; }   /* never started (e.g. create failed) */

    p = w->pool;
    es_ws_stop(w);

    /* Wait for the service thread to release the wsi, run any in-flight callback to
     * completion (w is still valid meanwhile), and unlink us. This is what lets the
     * owner safely free its per-stream state the instant we return: once w->done is
     * set the service thread has provably stopped touching w. Teardown normally
     * completes in a few service ticks (suppress_events already muted the callbacks,
     * and the wsi is force-closed). The generous bound only guards against a wsi that
     * refuses to die; on timeout we hand ownership off rather than free under it —
     * suppress_events means no callback can still reference the owner's state either way.
     * Whoever wins the p->mu race frees exactly once; the loser never touches w. */
    for (spins = 0; !w->done && spins < 5000; spins++) usleep(1000);

    pthread_mutex_lock(&p->mu);
    if (w->done) { pthread_mutex_unlock(&p->mu); es_free_full(w); }
    else         { w->orphaned = 1; pthread_mutex_unlock(&p->mu); }  /* service thread frees */
}
