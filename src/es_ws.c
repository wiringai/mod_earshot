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
    struct lws             *wsi;     /* current connection (service thread) */
    pthread_mutex_t         mu;      /* guards the queue + the metrics fields */
    es_ws_msg_t            *head, *tail;   /* FIFO outbound queue */
    size_t                  queued_bytes;
    size_t                  max_queue_bytes;
    volatile int            connected;
    volatile int            alive;        /* stream wants to stay up (0 once stopping) */
    volatile int            want_connect; /* service thread should (re)connect now */
    volatile int            teardown;     /* stop requested by the owning thread */
    volatile int            done;         /* service thread released the wsi + unlinked us */
    volatile int            orphaned;     /* owner gave up waiting; service thread must free */
    int                     kill;         /* force-close the wsi (service thread only) */
    volatile int            suppress_events; /* mute owner callbacks once teardown starts */
    int                     backoff_ms;
    long                    next_reconnect_ms;
    long                    connected_since_ms;   /* when the current link established (0 if down) */
    unsigned                rng;
    unsigned                reconnects;
    uint64_t                queue_drops;
    long                    rtt_ms;
    long                    ping_sent_ms;
    long                    last_ping_ms;
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
    volatile int        running;
    pthread_mutex_t     mu;      /* guards `list` + each stream's lifecycle flags */
    es_ws_t            *list;    /* attached streams (via w->lnext) */
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
    pthread_mutex_unlock(&w->mu);

    /* wake the shared loop; it requests writable for us on the service thread */
    if (w->pool && w->pool->ctx) lws_cancel_service(w->pool->ctx);
    return 0;
}

static es_ws_msg_t *es_dequeue(es_ws_t *w)
{
    es_ws_msg_t *m;
    pthread_mutex_lock(&w->mu);
    m = w->head;
    if (m) { w->head = m->next; if (!w->head) w->tail = NULL; w->queued_bytes -= m->len; }
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
        w->last_ping_ms = 0;
        /* Gate on suppress_events like every other callback: a wsi that establishes
         * after teardown began (the orphan window) must not call into freed owner state. */
        if (!w->suppress_events && w->o.on_event) w->o.on_event(w->o.user, 1, 0, "established");
        if (w->head) lws_callback_on_writable(wsi);
        break;

    case LWS_CALLBACK_CLIENT_RECEIVE: {
        int final;
        if (!w) break;
        if (w->kill) return -1;                        /* teardown: refuse further rx, close */
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
                if (lws_frame_is_binary(wsi)) {
                    if (w->o.on_binary) w->o.on_binary(w->o.user, w->rx, w->rx_len);
                } else if (w->o.on_text) {
                    w->rx[w->rx_len] = '\0';
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
            if (w->head) lws_callback_on_writable(wsi);
        }
        break;
    }

    case LWS_CALLBACK_CLIENT_RECEIVE_PONG:
        if (w && w->ping_sent_ms) {
            pthread_mutex_lock(&w->mu);
            w->rtt_ms = es_now_ms() - w->ping_sent_ms;
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
    while (p->running) {
        lws_service(p->ctx, 50);          /* woken early by lws_cancel_service */
        long now = es_now_ms();
        es_ws_t *prev = NULL, *w;
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
                if (w->head) lws_callback_on_writable(w->wsi);
                if (w->last_ping_ms == 0) w->last_ping_ms = now;
                if (now - w->last_ping_ms >= 5000) {
                    w->last_ping_ms = now; w->want_ping = 1;
                    lws_callback_on_writable(w->wsi);
                }
            } else if (!w->connected && !w->wsi && w->o.reconnect && now >= w->next_reconnect_ms) {
                int jitter = (int) (w->rng = w->rng * 1103515245u + 12345u) % (w->backoff_ms / 2 + 1);
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

/* ---- pool lifecycle ---------------------------------------------------- */
static int es_pool_init(void)   /* lazy, once */
{
    long cores;
    int i, k;
    if (g_pool.inited) return 0;
    pthread_mutex_lock(&g_pool.mu);
    if (g_pool.inited) { pthread_mutex_unlock(&g_pool.mu); return 0; }

    cores = sysconf(_SC_NPROCESSORS_ONLN);
    g_pool.n = (int) (cores > 0 ? cores : 1);
    if (g_pool.n > ES_POOL_MAX) g_pool.n = ES_POOL_MAX;

    for (i = 0; i < g_pool.n; i++) {
        struct lws_context_creation_info info;
        es_pool_ctx_t *p = &g_pool.c[i];
        memset(&info, 0, sizeof info);
        info.port = CONTEXT_PORT_NO_LISTEN;
        info.protocols = es_protocols;
        info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
        pthread_mutex_init(&p->mu, NULL);
        p->list = NULL;
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
    g_pool.n = 0;
    pthread_mutex_unlock(&g_pool.mu);
    return -1;
}

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
