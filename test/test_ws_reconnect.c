/* What a connection carries first, and how long a URL it can be opened with.
 *
 * Agents speak protocols with a preamble: OpenAI wants session.update before audio,
 * Gemini closes the socket on anything before setup, a Twilio-style agent expects
 * connected/start. The owner of a stream sends that preamble from its connected callback.
 * The transport used to queue it BEHIND whatever was already waiting, and on a reconnect
 * what was waiting was audio meant for the connection that had just been lost. Three
 * cases, against a libwebsockets server in this process that records what it receives:
 *
 *   1. First connection, agent not up yet: a message sent while waiting arrives, after
 *      the preamble.
 *   2. Reconnect: a message queued for the lost connection is never delivered; the new
 *      connection starts with the preamble, and messages sent after it flow normally.
 *   3. A URL at the length limit (ES_URL_MAX), with every handshake header at full
 *      size, goes on the wire whole; a longer one is refused outright. The path used to
 *      be cut at 511 bytes without a word. */
#define _GNU_SOURCE   /* strcasestr */
#include "../src/es_ws.h"
#include "../src/es_util.h"

#include <libwebsockets.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- what the server saw ------------------------------------------------- */
#define LOG_MAX 64
static pthread_mutex_t log_mu = PTHREAD_MUTEX_INITIALIZER;
static char  log_msg[LOG_MAX][64];
static int   log_n;

static void log_reset(void)
{
    pthread_mutex_lock(&log_mu);
    log_n = 0;
    pthread_mutex_unlock(&log_mu);
}

static int log_count(void)
{
    int n;
    pthread_mutex_lock(&log_mu); n = log_n; pthread_mutex_unlock(&log_mu);
    return n;
}

static int log_has(const char *msg)
{
    int i, found = 0;
    pthread_mutex_lock(&log_mu);
    for (i = 0; i < log_n; i++) if (!strcmp(log_msg[i], msg)) found = 1;
    pthread_mutex_unlock(&log_mu);
    return found;
}

static int log_is(int i, const char *msg)
{
    int same;
    pthread_mutex_lock(&log_mu);
    same = i < log_n && !strcmp(log_msg[i], msg);
    pthread_mutex_unlock(&log_mu);
    return same;
}

static void log_print(const char *what)
{
    int i;
    pthread_mutex_lock(&log_mu);
    printf("%s:", what);
    for (i = 0; i < log_n; i++) printf(" [%s]", log_msg[i]);
    printf("\n");
    pthread_mutex_unlock(&log_mu);
}

static int srv_cb(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len)
{
    (void) user;
    (void) wsi;
    if (reason == LWS_CALLBACK_RECEIVE) {
        pthread_mutex_lock(&log_mu);
        if (log_n < LOG_MAX) snprintf(log_msg[log_n++], sizeof log_msg[0], "%.*s", (int) len, (const char *) in);
        pthread_mutex_unlock(&log_mu);
    }
    return 0;
}

static const struct lws_protocols srv_protocols[] = {
    { "earshot", srv_cb, 0, 4096 },
    { 0 }
};

typedef struct {
    struct lws_context *ctx;
    pthread_t           thread;
    _Atomic int         run;
} server_t;

static void *srv_thread(void *arg)
{
    server_t *s = (server_t *) arg;
    while (s->run) lws_service(s->ctx, 0);
    return NULL;
}

static void server_start(server_t *s, int port)
{
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof info);
    info.port = port;
    info.iface = "127.0.0.1";
    info.protocols = srv_protocols;
    s->ctx = lws_create_context(&info);
    if (!s->ctx) { fprintf(stderr, "server did not start on %d\n", port); exit(1); }
    s->run = 1;
    if (pthread_create(&s->thread, NULL, srv_thread, s) != 0) { fprintf(stderr, "no server thread\n"); exit(1); }
}

static void server_stop(server_t *s)
{
    s->run = 0;
    lws_cancel_service(s->ctx);
    pthread_join(s->thread, NULL);
    lws_context_destroy(s->ctx);
}

/* ---- the stream's owner: sends a preamble whenever a connection opens ------ */
static es_ws_t *g_w;
static _Atomic int g_closed;

static void on_event(void *user, int connected, int code, const char *reason)
{
    (void) user; (void) code; (void) reason;
    if (connected) es_ws_send_text(g_w, "PREAMBLE", 8);
    else g_closed++;
}

static void say(const char *msg) { es_ws_send_text(g_w, msg, strlen(msg)); }

static int wait_for(int (*cond)(void), long limit_ms)
{
    long until = now_ms() + limit_ms;
    while (now_ms() < until) { if (cond()) return 1; usleep(5000); }
    return cond();
}
static int is_up(void)        { return es_ws_connected(g_w); }
static int is_down(void)      { return !es_ws_connected(g_w); }
static int got_two(void)      { return log_count() >= 2; }
static int got_fresh(void)    { return log_has("fresh"); }

/* ---- a listener that only reads the opening request ----------------------
 * For the long-URL case the question is what the transport puts on the wire, so the
 * other end is a plain socket: no server-side limits of its own in the way. It reads up
 * to the blank line that ends the request headers and closes. */
static char        req[32768];
static _Atomic int req_len = -1;
static int         req_fd = -1;

static void *req_thread(void *arg)
{
    int c = accept(req_fd, NULL, NULL), n = 0;
    (void) arg;
    if (c >= 0) {
        struct timeval tv = { 3, 0 };
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        while (n < (int) sizeof req - 1) {
            ssize_t r = recv(c, req + n, sizeof req - 1 - (size_t) n, 0);
            if (r <= 0) break;
            n += (int) r; req[n] = '\0';
            if (strstr(req, "\r\n\r\n")) break;
        }
        close(c);
    }
    req[n] = '\0';
    req_len = n;
    return NULL;
}

static void req_listen(int port, pthread_t *t)
{
    struct sockaddr_in a;
    int one = 1;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t) port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    req_fd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(req_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (req_fd < 0 || bind(req_fd, (struct sockaddr *) &a, sizeof a) != 0 || listen(req_fd, 1) != 0
        || pthread_create(t, NULL, req_thread, NULL) != 0) {
        fprintf(stderr, "could not listen on %d\n", port); exit(1);
    }
}
static int got_req(void) { return req_len >= 0; }

/* the value of one request header, as a length (-1: header absent) */
static int hdr_len(const char *name)
{
    char key[64];
    const char *h, *e;
    snprintf(key, sizeof key, "\r\n%s: ", name);
    if (!(h = strcasestr(req, key))) return -1;
    h += strlen(key);
    e = strstr(h, "\r\n");
    return e ? (int) (e - h) : -1;
}

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("    !! FAIL " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(void)
{
    server_t srv;
    char url[64];
    int port = 20000 + (int) (getpid() % 20000);
    es_ws_opts_t o;
    es_ws_stats_t st;

    lws_set_log_level(LLL_ERR, NULL);
    snprintf(url, sizeof url, "ws://127.0.0.1:%d/", port);
    if (es_ws_global_init() != 0) { fprintf(stderr, "pool init failed\n"); return 1; }

    memset(&o, 0, sizeof o);
    o.url = url; o.reconnect = 1; o.on_event = on_event;

    /* 1. first connection: the agent comes up after the stream was started */
    log_reset();
    g_w = es_ws_create(&o);
    if (!g_w || es_ws_start(g_w) != 0) { fprintf(stderr, "could not start the stream\n"); return 1; }
    usleep(300 * 1000);
    say("early");                              /* nobody is listening yet */
    server_start(&srv, port);
    CHECK(wait_for(is_up, 10000), "never connected to the late agent");
    CHECK(wait_for(got_two, 3000), "first connection: expected the preamble and the early message");
    log_print("first connection");
    CHECK(log_is(0, "PREAMBLE"), "first connection must start with the preamble");
    CHECK(log_is(1, "early"), "a message sent before the first connection must follow the preamble");

    /* 2. the agent goes away and comes back */
    server_stop(&srv);
    CHECK(wait_for(is_down, 5000), "the stream never noticed the agent was gone");
    say("stale-1"); say("stale-2");            /* queued for a connection that no longer exists */
    log_reset();
    server_start(&srv, port);
    CHECK(wait_for(is_up, 15000), "no reconnect");
    say("fresh");
    CHECK(wait_for(got_fresh, 3000), "a message sent after the reconnect did not arrive");
    log_print("after reconnect");
    CHECK(log_is(0, "PREAMBLE"), "a reconnect must start with the preamble");
    CHECK(!log_has("stale-1") && !log_has("stale-2"), "messages queued for the lost connection were replayed");
    es_ws_get_stats(g_w, &st);
    CHECK(st.queue_drops >= 2, "the dropped messages were not counted (queue_drops=%llu)", (unsigned long long) st.queue_drops);
    es_ws_destroy(g_w);

    server_stop(&srv);

    /* 3. a URL at the limit, with full-size Authorization and X-Earshot-Meta */
    {
        char *big = malloc(ES_URL_MAX + 64), *auth = malloc(512), *meta = malloc(1025);
        int n = snprintf(big, ES_URL_MAX + 64, "ws://127.0.0.1:%d/ws/call?token=", port);
        const char *want, *sp;
        pthread_t t;
        memset(big + n, 'T', (size_t) (ES_URL_MAX - n)); big[ES_URL_MAX - 1] = 'Z'; big[ES_URL_MAX] = '\0';
        want = strchr(big + 5, '/');             /* the request target: path and query */
        memset(auth, 'a', 511); auth[511] = '\0';
        memset(meta, 'm', 1024); meta[1024] = '\0';

        req_listen(port, &t);
        o.url = big; o.auth = auth; o.hdr_meta = meta; o.reconnect = 0;
        o.hdr_call_id = "call-id@198.51.100.7"; o.hdr_channel_uuid = "3f2b8c1e-0000-4000-8000-000000000001";
        o.hdr_correlation = "call-id@198.51.100.7";
        g_w = es_ws_create(&o);
        CHECK(g_w != NULL, "a %d-byte URL was refused", ES_URL_MAX);
        if (g_w && es_ws_start(g_w) == 0) {
            CHECK(wait_for(got_req, 6000), "no request reached the listener for a %d-byte URL", ES_URL_MAX);
            pthread_join(t, NULL);
            sp = !strncmp(req, "GET ", 4) ? strchr(req + 4, ' ') : NULL;
            printf("long url: request target sent %zu bytes, on the wire %d; Authorization %d, X-Earshot-Meta %d\n",
                   strlen(want), sp ? (int) (sp - (req + 4)) : -1, hdr_len("Authorization"), hdr_len("X-Earshot-Meta"));
            CHECK(sp && (size_t) (sp - (req + 4)) == strlen(want) && !strncmp(req + 4, want, strlen(want)),
                  "the request target on the wire is not the URL that was given (the token is at its end)");
            CHECK(strstr(req, "\r\n\r\n") != NULL, "the request headers never ended: the handshake did not fit");
            CHECK(hdr_len("Authorization") == 511, "Authorization header missing or cut");
            CHECK(hdr_len("X-Earshot-Meta") == 1024, "X-Earshot-Meta header missing or cut");
            CHECK(hdr_len("X-Call-ID") == 20 && hdr_len("X-Channel-UUID") == 36, "correlation headers missing");
        } else {
            pthread_cancel(t); pthread_join(t, NULL);
        }
        close(req_fd);
        es_ws_destroy(g_w);

        big[ES_URL_MAX] = 'T'; big[ES_URL_MAX + 1] = '\0';                          /* one byte over */
        g_w = es_ws_create(&o);
        CHECK(g_w == NULL, "a URL over the limit must be refused, not shortened");
        if (g_w) es_ws_destroy(g_w);
        free(big); free(auth); free(meta);
    }

    es_ws_global_shutdown();
    printf(fails ? "ws reconnect: %d FAILURE(S)\n" : "ws reconnect: ok\n", fails);
    return fails ? 1 : 0;
}
