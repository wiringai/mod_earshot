/*
 * es_ws — libwebsockets client transport for Earshot.
 *
 * STATUS: reviewed draft. Builds/validates on a test environment with libwebsockets-dev.
 * The structure (context on a service thread, mutex-guarded outbound queue
 * flushed on WRITEABLE, callback delivery of inbound frames + lifecycle,
 * reconnect-with-backoff) is the standard lws client pattern; expect small
 * fixes against the installed lws version on first compile.
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

/* Upper bound on a single reassembled inbound message. Real agent frames
 * (session config, audio deltas) are well under this; the cap only stops a
 * runaway or hostile server from growing the rx buffer without limit. */
#define ES_WS_MAX_MSG (4u * 1024u * 1024u)

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
    struct lws_context     *ctx;
    struct lws             *wsi;
    pthread_t               thread;
    pthread_mutex_t         mu;
    es_ws_msg_t            *head, *tail;   /* FIFO outbound queue */
    size_t                  queued_bytes;  /* current backlog size (guarded by mu) */
    size_t                  max_queue_bytes; /* drop-oldest past this (0 = unlimited) */
    volatile int            running;
    volatile int            connected;
    int                     backoff_ms;    /* current reconnect backoff */
    unsigned                rng;           /* backoff jitter state */
    unsigned                reconnects;    /* reconnect attempts (metrics) */
    uint64_t                queue_drops;   /* frames dropped at the queue cap (metrics) */
    long                    rtt_ms;        /* last measured ws round-trip (metrics) */
    long                    ping_sent_ms;  /* send time of the outstanding ping */
    long                    last_ping_ms;  /* last ping schedule time */
    int                     want_ping;     /* a ping is due on the next WRITEABLE */
    /* inbound reassembly: lws can split one ws message across several RECEIVE
     * callbacks (and never NUL-terminates); accumulate here, dispatch once whole. */
    unsigned char          *rx;            /* reassembly buffer (service thread only) */
    size_t                  rx_len;        /* bytes accumulated so far */
    size_t                  rx_cap;        /* allocated capacity */
    int                     rx_drop;       /* current message over cap / OOM: discard it whole */
    /* parsed url */
    char                    host[256];
    char                    path[512];
    int                     port;
    int                     use_tls;
};

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
    /* Bound the backlog: past the cap, drop the OLDEST frames. This buffers audio
     * across a brief reconnect but prevents unbounded growth (and stale playback)
     * during a long outage. Never drop the frame we just enqueued. */
    while (w->max_queue_bytes && w->queued_bytes > w->max_queue_bytes && w->head != w->tail) {
        es_ws_msg_t *old = w->head;
        w->head = old->next;
        w->queued_bytes -= old->len;
        w->queue_drops++;
        free(old);
    }
    pthread_mutex_unlock(&w->mu);

    /* Wake the service loop ONLY via lws_cancel_service — the one lws call that is
     * safe from another thread. Requesting writable (lws_callback_on_writable) must
     * happen on the service thread that owns the wsi; doing it here raced the wsi's
     * create/destroy during a reconnect storm and corrupted the heap. The service
     * loop requests writable itself once it sees queued data. */
    lws_cancel_service(w->ctx);
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

/* ---- lws protocol callback -------------------------------------------- */
static int es_cb(struct lws *wsi, enum lws_callback_reasons reason,
                 void *user, void *in, size_t len)
{
    es_ws_t *w = (es_ws_t *) lws_context_user(lws_get_context(wsi));
    (void) user;

    switch (reason) {
    case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER: {
        unsigned char **p = (unsigned char **) in, *end = (*p) + len;
        int rc = 0;   /* lws_add_http_header_by_name returns non-zero if the header buffer is full */
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
        if (rc) return -1;   /* couldn't fit the required headers -> abort rather than truncate */
        break;
    }

    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        w->connected = 1;
        w->backoff_ms = 250;                 /* reset backoff on success */
        if (w->o.on_event) w->o.on_event(w->o.user, 1, 0, "established");
        if (w->head) lws_callback_on_writable(wsi);
        break;

    case LWS_CALLBACK_CLIENT_RECEIVE: {
        /* Accumulate fragments: lws may split a single ws message across several
         * RECEIVE callbacks, and the delivered buffer is NOT NUL-terminated.
         * Dispatch only once the whole message has arrived; NUL-terminate text so
         * cJSON_Parse never reads past the end (a live agent's multi-KB frames
         * would otherwise over-read the rx buffer and corrupt the heap). */
        int final = lws_is_final_fragment(wsi) && !lws_remaining_packet_payload(wsi);
        /* Accumulate unless this message already blew the cap / OOM'd — in which case
         * we must discard EVERY fragment of it (not just resync), or the tail would be
         * dispatched as a bogus standalone message. */
        if (!w->rx_drop) {
            size_t need = w->rx_len + len + 1;             /* +1 reserves the text NUL */
            if (need > ES_WS_MAX_MSG) {
                w->rx_drop = 1;                            /* runaway/hostile server guard */
            } else if (need > w->rx_cap) {
                size_t ncap = w->rx_cap ? w->rx_cap : 4096;
                unsigned char *nb;
                while (ncap < need) ncap *= 2;
                nb = realloc(w->rx, ncap);
                if (!nb) w->rx_drop = 1;                   /* OOM: drop, keep old buffer */
                else { w->rx = nb; w->rx_cap = ncap; }
            }
            if (!w->rx_drop && len) { memcpy(w->rx + w->rx_len, in, len); w->rx_len += len; }
        }
        if (final) {
            if (!w->rx_drop) {
                if (lws_frame_is_binary(wsi)) {
                    if (w->o.on_binary) w->o.on_binary(w->o.user, w->rx, w->rx_len);
                } else if (w->o.on_text) {
                    w->rx[w->rx_len] = '\0';               /* room reserved by the +1 above */
                    w->o.on_text(w->o.user, (const char *) w->rx, w->rx_len);
                }
            }
            w->rx_len = 0; w->rx_drop = 0;                 /* reset for the next message (buffer kept) */
        }
        break;
    }

    case LWS_CALLBACK_CLIENT_WRITEABLE: {
        es_ws_msg_t *m;
        if (w->want_ping) {                     /* latency probe: send a ws PING with a timestamp */
            unsigned char pbuf[LWS_PRE + sizeof(long)];
            w->want_ping = 0;
            w->ping_sent_ms = es_now_ms();
            memcpy(pbuf + LWS_PRE, &w->ping_sent_ms, sizeof(long));
            lws_write(wsi, pbuf + LWS_PRE, sizeof(long), LWS_WRITE_PING);
            lws_callback_on_writable(wsi);      /* come back to flush queued audio */
            break;
        }
        m = es_dequeue(w);
        if (m) {
            enum lws_write_protocol wp = m->binary ? LWS_WRITE_BINARY : LWS_WRITE_TEXT;
            int n = lws_write(wsi, m->buf + LWS_PRE, m->len, wp);
            free(m);
            if (n < 0) return -1;
            if (w->head) lws_callback_on_writable(wsi);   /* more to send */
        }
        break;
    }

    case LWS_CALLBACK_CLIENT_RECEIVE_PONG:      /* latency probe reply */
        if (w->ping_sent_ms) {                  /* guarded: es_ws_get_stats reads rtt_ms under mu */
            pthread_mutex_lock(&w->mu);
            w->rtt_ms = es_now_ms() - w->ping_sent_ms;
            pthread_mutex_unlock(&w->mu);
        }
        break;

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        w->connected = 0; w->wsi = NULL;
        if (w->o.on_event) w->o.on_event(w->o.user, 0, -1, in ? (const char *) in : "connect error");
        break;

    case LWS_CALLBACK_CLIENT_CLOSED:
        w->connected = 0; w->wsi = NULL;
        if (w->o.on_event) w->o.on_event(w->o.user, 0, 0, "closed");
        break;

    default:
        break;
    }
    return 0;
}

static const struct lws_protocols es_protocols[] = {
    { "earshot", es_cb, 0, 65536 },   /* remaining fields zero-initialized */
    { 0 }                             /* terminator (portable across lws versions) */
};

/* ---- connect + service thread ----------------------------------------- */
static int es_connect(es_ws_t *w)
{
    struct lws_client_connect_info i;
    memset(&i, 0, sizeof i);
    i.context = w->ctx;
    i.address = w->host;
    i.port    = w->port;
    i.path    = w->path;
    i.host    = w->host;
    i.origin  = w->host;
    /* Advertise the vendor's ws subprotocol when it needs one. OpenAI's Realtime
     * endpoint answers the upgrade with "Sec-WebSocket-Protocol: realtime"; if we
     * offer anything else (or our default "earshot"), lws rejects the mismatch
     * ("HS: PROTOCOL malformed"). Bind our callback via local_protocol_name so the
     * wire subprotocol can differ from our internal protocol name. */
    i.protocol = w->o.subprotocol ? w->o.subprotocol : es_protocols[0].name;
    i.local_protocol_name = es_protocols[0].name;
    i.pwsi     = &w->wsi;
    if (w->use_tls) {
        i.ssl_connection = LCCSCF_USE_SSL;
        if (w->o.insecure)
            i.ssl_connection |= LCCSCF_ALLOW_SELFSIGNED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;
    }
    return lws_client_connect_via_info(&i) ? 0 : -1;
}

static void *es_service_thread(void *arg)
{
    es_ws_t *w = (es_ws_t *) arg;
    if (es_connect(w) != 0 && w->o.on_event)
        w->o.on_event(w->o.user, 0, -1, "initial connect failed");

    while (w->running) {
        lws_service(w->ctx, 50);      /* service; woken early by lws_cancel_service */
        /* Drain the outbound queue: request writable HERE, on the wsi's owning
         * thread (es_enqueue only wakes us via lws_cancel_service). */
        if (w->connected && w->wsi && w->head)
            lws_callback_on_writable(w->wsi);
        if (w->connected && w->wsi) { /* schedule an RTT ping every 5s */
            long now = es_now_ms();
            if (w->last_ping_ms == 0) w->last_ping_ms = now;
            if (now - w->last_ping_ms >= 5000) {
                w->last_ping_ms = now; w->want_ping = 1;
                lws_callback_on_writable(w->wsi);
            }
        }
        if (!w->connected && !w->wsi && w->running) {
            if (w->o.reconnect) {
                /* exp backoff with +/- up to half-interval jitter (thundering-herd guard).
                 * Sleep in short slices so a concurrent es_ws_stop (running=0) is honored
                 * within ~20ms instead of blocking pthread_join behind a multi-second sleep. */
                int jitter = (int) (w->rng = w->rng * 1103515245u + 12345u) % (w->backoff_ms / 2 + 1);
                int total = w->backoff_ms + jitter, slept = 0;
                while (slept < total && w->running) { usleep(20000); slept += 20; }
                if (!w->running) break;
                if (w->backoff_ms < 8000) w->backoff_ms *= 2;   /* exp backoff, cap 8s */
                pthread_mutex_lock(&w->mu);                      /* es_ws_get_stats reads under mu */
                w->reconnects++;
                pthread_mutex_unlock(&w->mu);
                es_connect(w);
            } else {
                break;
            }
        }
    }
    return NULL;
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

    w->o = *opts;   /* shallow copy, then dup the strings we keep */
    w->o.url  = es_strdup(opts->url);
    w->o.auth = es_strdup(opts->auth);
    w->o.hdr_call_id      = es_strdup(opts->hdr_call_id);
    w->o.hdr_channel_uuid = es_strdup(opts->hdr_channel_uuid);
    w->o.hdr_correlation  = es_strdup(opts->hdr_correlation);
    w->o.hdr_extra_name   = es_strdup(opts->hdr_extra_name);
    w->o.hdr_extra_value  = es_strdup(opts->hdr_extra_value);
    w->o.subprotocol      = es_strdup(opts->subprotocol);
    w->backoff_ms = 250;
    w->max_queue_bytes = opts->max_queue_bytes > 0 ? (size_t) opts->max_queue_bytes : 262144; /* ~16 s mu-law */
    w->rng = (unsigned) ((size_t) w ^ (size_t) opts->url);   /* cheap per-instance seed, no time dep */

    /* parse url (lws_parse_uri mutates its buffer) */
    strncpy(tmp, opts->url, sizeof tmp - 1); tmp[sizeof tmp - 1] = 0;
    if (lws_parse_uri(tmp, &prot, &ads, &port, &path) != 0) { es_ws_destroy(w); return NULL; }
    w->use_tls = (!strcasecmp(prot, "wss") || !strcasecmp(prot, "https"));
    w->port = port;
    snprintf(w->host, sizeof w->host, "%s", ads);
    snprintf(w->path, sizeof w->path, "/%s", path);   /* lws strips the leading '/' */

    return w;
}

int es_ws_start(es_ws_t *w)
{
    struct lws_context_creation_info info;
    if (!w) return -1;

    memset(&info, 0, sizeof info);
    info.port = CONTEXT_PORT_NO_LISTEN;   /* client only */
    info.protocols = es_protocols;
    info.user = w;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    if (w->o.ca_file)   info.client_ssl_ca_filepath = w->o.ca_file;
    if (w->o.cert_file) info.client_ssl_cert_filepath = w->o.cert_file;
    if (w->o.key_file)  info.client_ssl_private_key_filepath = w->o.key_file;

    w->ctx = lws_create_context(&info);
    if (!w->ctx) return -1;

    pthread_mutex_init(&w->mu, NULL);
    w->running = 1;
    if (pthread_create(&w->thread, NULL, es_service_thread, w) != 0) {
        w->running = 0; lws_context_destroy(w->ctx); w->ctx = NULL; return -1;
    }
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

void es_ws_stop(es_ws_t *w)
{
    if (!w || !w->running) return;
    w->running = 0;
    if (w->ctx) lws_cancel_service(w->ctx);   /* wake the loop so it can exit */
    pthread_join(w->thread, NULL);
    es_drain(w);
    free(w->rx); w->rx = NULL; w->rx_len = w->rx_cap = 0;   /* service thread is joined; safe to free */
    if (w->ctx) { lws_context_destroy(w->ctx); w->ctx = NULL; }
    pthread_mutex_destroy(&w->mu);
}

void es_ws_destroy(es_ws_t *w)
{
    if (!w) return;
    if (w->running) es_ws_stop(w);
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
