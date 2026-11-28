/* Stopping a stream ends its connection cleanly, and never slowly.
 *
 *   1. What was queued just before the stop still reaches the agent (a protocol's
 *      closing message, such as Twilio's `stop`), and the agent then sees a normal
 *      close (status 1000). It used to see a dropped connection (1006) and never the
 *      last message.
 *   2. An agent that has stopped reading cannot hold the stop up: the stream is cut
 *      off after a short bound, so a hangup or transfer is not delayed by it.
 *
 * The agent in case 1 is a libwebsockets server in this process that records what it
 * receives and how the connection closed; in case 2 it is a plain socket that answers
 * the handshake by hand and then never reads again. */
#define _GNU_SOURCE   /* strcasestr */
#include "../src/es_ws.h"

#include <arpa/inet.h>
#include <libwebsockets.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- case 1: a recording WebSocket server -------------------------------- */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static char        got[8][64];
static int         got_n;
static _Atomic int close_code = -1;     /* status the client closed with (-1: none seen) */
static _Atomic int closed;

static int srv_cb(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len)
{
    (void) wsi; (void) user;
    switch (reason) {
    case LWS_CALLBACK_RECEIVE:
        pthread_mutex_lock(&mu);
        if (got_n < 8) snprintf(got[got_n++], sizeof got[0], "%.*s", (int) len, (const char *) in);
        pthread_mutex_unlock(&mu);
        break;
    case LWS_CALLBACK_WS_PEER_INITIATED_CLOSE:
        if (len >= 2) close_code = (((unsigned char *) in)[0] << 8) | ((unsigned char *) in)[1];
        break;
    case LWS_CALLBACK_CLOSED:
        closed = 1;
        break;
    default:
        break;
    }
    return 0;
}
static const struct lws_protocols srv_protocols[] = { { "earshot", srv_cb, 0, 4096 }, { 0 } };

typedef struct { struct lws_context *ctx; pthread_t thread; _Atomic int run; } server_t;
static void *srv_thread(void *arg) { server_t *s = arg; while (s->run) lws_service(s->ctx, 0); return NULL; }
static void server_start(server_t *s, int port)
{
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof info);
    info.port = port; info.iface = "127.0.0.1"; info.protocols = srv_protocols;
    if (!(s->ctx = lws_create_context(&info))) { fprintf(stderr, "server did not start on %d\n", port); exit(1); }
    s->run = 1;
    if (pthread_create(&s->thread, NULL, srv_thread, s) != 0) exit(1);
}
static void server_stop(server_t *s)
{
    s->run = 0; lws_cancel_service(s->ctx); pthread_join(s->thread, NULL); lws_context_destroy(s->ctx);
}
static int has(const char *msg)
{
    int i, r = 0;
    pthread_mutex_lock(&mu);
    for (i = 0; i < got_n; i++) if (!strcmp(got[i], msg)) r = 1;
    pthread_mutex_unlock(&mu);
    return r;
}

/* ---- case 2: a peer that answers the handshake and never reads again ------ */
typedef struct { int fd; pthread_t t; _Atomic int stop; } deaf_t;
static void *deaf_thread(void *arg)
{
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    deaf_t *d = arg;
    char req[8192], key[128], cat[200], acc[64], resp[512];
    unsigned char sha[20];
    const char *k, *e;
    int c = accept(d->fd, NULL, NULL), n = 0;
    if (c < 0) return NULL;
    while (n < (int) sizeof req - 1) {
        ssize_t r = recv(c, req + n, sizeof req - 1 - (size_t) n, 0);
        if (r <= 0) break;
        n += (int) r; req[n] = '\0';
        if (strstr(req, "\r\n\r\n")) break;
    }
    if ((k = strcasestr(req, "Sec-WebSocket-Key: ")) && (e = strstr(k + 19, "\r\n")) && e - (k + 19) < (long) sizeof key) {
        k += 19;
        memcpy(key, k, (size_t) (e - k)); key[e - k] = '\0';
        snprintf(cat, sizeof cat, "%s%s", key, guid);
        lws_SHA1((const unsigned char *) cat, strlen(cat), sha);
        lws_b64_encode_string((const char *) sha, 20, acc, sizeof acc);
        n = snprintf(resp, sizeof resp,
                     "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: %s\r\nSec-WebSocket-Protocol: earshot\r\n\r\n", acc);
        (void) !send(c, resp, (size_t) n, 0);
    }
    while (!d->stop) usleep(20000);    /* holds the socket open, reads nothing, answers nothing */
    close(c);
    return NULL;
}
static void deaf_start(deaf_t *d, int port)
{
    struct sockaddr_in a;
    int one = 1;
    memset(d, 0, sizeof *d); memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t) port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    d->fd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(d->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (d->fd < 0 || bind(d->fd, (struct sockaddr *) &a, sizeof a) != 0 || listen(d->fd, 1) != 0
        || pthread_create(&d->t, NULL, deaf_thread, d) != 0) { fprintf(stderr, "could not listen on %d\n", port); exit(1); }
}
static void deaf_stop(deaf_t *d) { d->stop = 1; pthread_join(d->t, NULL); close(d->fd); }

/* ---- the stream ----------------------------------------------------------- */
static _Atomic int up;
static void on_event(void *user, int connected, int code, const char *reason)
{
    (void) user; (void) code; (void) reason;
    if (connected) up = 1;
}
static es_ws_t *open_stream(int port)
{
    char url[64];
    es_ws_opts_t o;
    es_ws_t *w;
    long until = now_ms() + 3000;
    memset(&o, 0, sizeof o);
    snprintf(url, sizeof url, "ws://127.0.0.1:%d/agent", port);
    o.url = url; o.reconnect = 0; o.on_event = on_event;
    up = 0;
    w = es_ws_create(&o);
    if (!w || es_ws_start(w) != 0) { fprintf(stderr, "stream did not start\n"); exit(1); }
    while (now_ms() < until && !up) usleep(5000);
    if (!up) { fprintf(stderr, "stream never connected\n"); exit(1); }
    return w;
}

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("    !! FAIL " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(void)
{
    server_t srv;
    deaf_t deaf;
    es_ws_t *w;
    long t0, took, until;
    int port = 39900 + (int) (getpid() % 90), round;

    lws_set_log_level(0, NULL);
    es_ws_global_init();

    printf("1. a message queued just before the stop is delivered, then the agent sees a normal close\n");
    server_start(&srv, port);
    for (round = 0; round < 5; round++) {
        pthread_mutex_lock(&mu); got_n = 0; pthread_mutex_unlock(&mu);
        close_code = -1; closed = 0;
        w = open_stream(port);
        es_ws_send_text(w, "hello", 5);
        usleep(50000);
        es_ws_send_text(w, "stop", 4);            /* queued and stopped in the same breath */
        t0 = now_ms(); es_ws_stop(w); es_ws_destroy(w); took = now_ms() - t0;
        until = now_ms() + 2000;
        while (now_ms() < until && !closed) usleep(5000);
        CHECK(has("hello"), "round %d: the earlier message was lost", round);
        CHECK(has("stop"), "round %d: the message queued before the stop never arrived", round);
        CHECK(close_code == 1000, "round %d: the agent saw close status %d, not 1000", round, (int) close_code);
        CHECK(took < 400, "round %d: stopping took %ld ms with a healthy agent", round, took);
    }
    server_stop(&srv);

    printf("2. an agent that has stopped reading cannot hold the stop up\n");
    deaf_start(&deaf, port + 1);
    w = open_stream(port + 1);
    es_ws_send_text(w, "stop", 4);
    t0 = now_ms(); es_ws_stop(w); es_ws_destroy(w); took = now_ms() - t0;
    CHECK(took < 700, "stopping took %ld ms against an agent that never answers", took);
    printf("    stopped in %ld ms\n", took);
    deaf_stop(&deaf);

    es_ws_global_shutdown();
    printf(fails ? "\nFAILED: %d check(s)\n" : "\nok\n", fails);
    return fails ? 1 : 0;
}
