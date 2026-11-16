/* The transport keeps its own time on a box with nothing else going on.
 *
 * Everything the service thread does on a clock (finishing a teardown, retrying a
 * connect after its backoff, pings) happens in a walk that runs when lws_service
 * returns. libwebsockets ignores a positive lws_service timeout and sleeps until a
 * socket event, so on a context with one stream and no traffic the walk has to be
 * woken by the transport itself. Two cases, each with a single stream and no audio:
 *
 *   1. es_ws_destroy returns promptly once the connection has closed. It used to
 *      wait its whole bound (seconds of silence before a transfer or hangup).
 *   2. A stream whose agent is not up yet reconnects once the agent appears. It
 *      used to stop retrying after the second failed attempt.
 *
 * The agent is a libwebsockets server in this process, on loopback. */
#include "../src/es_ws.h"

#include <libwebsockets.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- a WebSocket server that accepts and stays quiet -------------------- */
static int srv_cb(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len)
{
    (void) wsi; (void) reason; (void) user; (void) in; (void) len;
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

static int server_start(server_t *s, int port)
{
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof info);
    info.port = port;
    info.iface = "127.0.0.1";
    info.protocols = srv_protocols;
    s->ctx = lws_create_context(&info);
    if (!s->ctx) return -1;
    s->run = 1;
    if (pthread_create(&s->thread, NULL, srv_thread, s) != 0) {
        lws_context_destroy(s->ctx);
        return -1;
    }
    return 0;
}

static void server_stop(server_t *s)
{
    s->run = 0;
    lws_cancel_service(s->ctx);
    pthread_join(s->thread, NULL);
    lws_context_destroy(s->ctx);
}

/* ---- helpers ------------------------------------------------------------ */
static es_ws_t *client(const char *url, int reconnect)
{
    es_ws_opts_t o;
    es_ws_t *w;
    memset(&o, 0, sizeof o);
    o.url = url;
    o.reconnect = reconnect;
    w = es_ws_create(&o);
    if (!w || es_ws_start(w) != 0) { fprintf(stderr, "could not start a stream to %s\n", url); exit(1); }
    return w;
}

static int wait_connected(es_ws_t *w, long limit_ms)
{
    long until = now_ms() + limit_ms;
    while (now_ms() < until) {
        if (es_ws_connected(w)) return 1;
        usleep(5000);
    }
    return es_ws_connected(w);
}

#define DESTROY_LIMIT_MS   500    /* the old behaviour took over 5000 */
#define RECONNECT_LIMIT_MS 10000  /* backoff reaches the agent's start within about 4 s */

int main(void)
{
    server_t srv;
    char url[64];
    int port = 20000 + (int) (getpid() % 20000);
    int i, failed = 0;

    lws_set_log_level(LLL_ERR, NULL);
    snprintf(url, sizeof url, "ws://127.0.0.1:%d/", port);
    if (es_ws_global_init() != 0) { fprintf(stderr, "pool init failed\n"); return 1; }

    /* 1. teardown of a lone, idle stream */
    if (server_start(&srv, port) != 0) { fprintf(stderr, "server did not start on %d\n", port); return 1; }
    for (i = 0; i < 6; i++) {
        es_ws_t *w = client(url, 0);
        long t0, took;
        if (!wait_connected(w, 3000)) { fprintf(stderr, "stream %d never connected\n", i); return 1; }
        usleep(150 * 1000);   /* nothing is sent: the context goes idle */
        t0 = now_ms();
        es_ws_destroy(w);
        took = now_ms() - t0;
        printf("teardown %d: %ld ms\n", i, took);
        if (took > DESTROY_LIMIT_MS) failed = 1;
    }
    server_stop(&srv);
    if (failed) { fprintf(stderr, "FAIL: es_ws_destroy waited more than %d ms on an idle context\n", DESTROY_LIMIT_MS); return 1; }

    /* 2. reconnect to an agent that comes up late */
    {
        es_ws_t *w = client(url, 1);
        long t0;
        int up;
        usleep(1500 * 1000);          /* the first attempts are refused */
        if (server_start(&srv, port) != 0) { fprintf(stderr, "server did not restart on %d\n", port); return 1; }
        t0 = now_ms();
        up = wait_connected(w, RECONNECT_LIMIT_MS);
        printf("reconnect: %s after %ld ms\n", up ? "connected" : "not connected", now_ms() - t0);
        es_ws_destroy(w);
        server_stop(&srv);
        if (!up) { fprintf(stderr, "FAIL: no reconnect within %d ms on an idle context\n", RECONNECT_LIMIT_MS); return 1; }
    }

    es_ws_global_shutdown();
    printf("ws idle: ok\n");
    return 0;
}
