/* The transport's three clocks are per stream and do what they say.
 *
 *   connect_timeout_ms  an attempt the far end never completes is abandoned after this
 *                       long, and the owner is told once ("connect timeout"). It can be
 *                       shorter than the default and longer than libwebsockets' own
 *                       20-second limit, which used to be the ceiling.
 *   liveness_ms         a connected peer that goes silent (no frames, no pongs) is dropped
 *                       after this long; negative switches the check off.
 *   teardown            stopping a stream whose attempt is still pending returns promptly.
 *   reconnect_max /     a stream whose agent stays unreachable stops trying after a number
 *   reconnect_deadline  of attempts or a length of time, and says so once (ES_WS_GAVE_UP);
 *                       with neither set it keeps trying, as it always has.
 *
 * The far end is a plain socket in this process: either one that accepts and never
 * answers, or one that completes the WebSocket handshake by hand and then says nothing,
 * not even a pong (a libwebsockets server would answer pings by itself). */
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

/* ---- the far end -------------------------------------------------------- */
typedef struct { int fd, upgrade; pthread_t t; _Atomic int stop; } peer_t;

/* Answer the opening request with a correct 101, then go silent. */
static void upgrade(int c)
{
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char req[8192], key[128], cat[200], acc[64], resp[512];
    unsigned char sha[20];
    int n = 0;
    const char *k, *e;
    while (n < (int) sizeof req - 1) {
        ssize_t r = recv(c, req + n, sizeof req - 1 - (size_t) n, 0);
        if (r <= 0) return;
        n += (int) r; req[n] = '\0';
        if (strstr(req, "\r\n\r\n")) break;
    }
    if (!(k = strcasestr(req, "Sec-WebSocket-Key: "))) return;
    k += 19;
    if (!(e = strstr(k, "\r\n")) || e - k >= (long) sizeof key) return;
    memcpy(key, k, (size_t) (e - k)); key[e - k] = '\0';
    snprintf(cat, sizeof cat, "%s%s", key, guid);
    lws_SHA1((const unsigned char *) cat, strlen(cat), sha);
    lws_b64_encode_string((const char *) sha, 20, acc, sizeof acc);
    n = snprintf(resp, sizeof resp,
                 "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                 "Sec-WebSocket-Accept: %s\r\nSec-WebSocket-Protocol: earshot\r\n\r\n", acc);
    (void) !send(c, resp, (size_t) n, 0);
}

static void *peer_thread(void *arg)
{
    peer_t *p = (peer_t *) arg;
    int held[16], nheld = 0, i;
    while (!p->stop && nheld < 16) {
        struct timeval tv = { 0, 100000 };
        fd_set r; FD_ZERO(&r); FD_SET(p->fd, &r);
        if (select(p->fd + 1, &r, NULL, NULL, &tv) > 0) {
            int c = accept(p->fd, NULL, NULL);
            if (c < 0) continue;
            if (p->upgrade) upgrade(c);
            held[nheld++] = c;            /* keep it open and say nothing */
        }
    }
    while (!p->stop) usleep(20000);
    for (i = 0; i < nheld; i++) close(held[i]);
    return NULL;
}

static void peer_start(peer_t *p, int port, int do_upgrade)
{
    struct sockaddr_in a;
    int one = 1;
    memset(p, 0, sizeof *p); memset(&a, 0, sizeof a);
    p->upgrade = do_upgrade;
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t) port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    p->fd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(p->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (p->fd < 0 || bind(p->fd, (struct sockaddr *) &a, sizeof a) != 0 || listen(p->fd, 8) != 0
        || pthread_create(&p->t, NULL, peer_thread, p) != 0) {
        fprintf(stderr, "could not listen on %d\n", port); exit(1);
    }
}
static void peer_stop(peer_t *p) { p->stop = 1; pthread_join(p->t, NULL); close(p->fd); }

/* ---- what the owner is told --------------------------------------------- */
static _Atomic int  ups, downs, gave_up, fails_seen;
static _Atomic long gave_up_ms;
static _Atomic long first_down_ms, up_ms;
static char         down_reason[64];

static void on_event(void *user, int connected, int code, const char *reason)
{
    (void) user;
    if (connected) { ups++; up_ms = now_ms(); return; }
    if (code == ES_WS_GAVE_UP) { gave_up++; gave_up_ms = now_ms(); return; }
    if (code < 0) fails_seen++;
    if (downs++ == 0) {
        snprintf(down_reason, sizeof down_reason, "%s", reason ? reason : "");
        first_down_ms = now_ms();
    }
}

static es_ws_t *open_retrying(int port, int reconnect_max, int deadline_ms)
{
    char url[64];
    es_ws_opts_t o;
    es_ws_t *w;
    memset(&o, 0, sizeof o);
    snprintf(url, sizeof url, "ws://127.0.0.1:%d/agent", port);
    o.url = url; o.reconnect = 1; o.on_event = on_event;
    o.reconnect_max = reconnect_max; o.reconnect_deadline_ms = deadline_ms;
    ups = 0; downs = 0; gave_up = 0; fails_seen = 0; gave_up_ms = 0; first_down_ms = 0; down_reason[0] = '\0';
    w = es_ws_create(&o);
    if (!w || es_ws_start(w) != 0) { fprintf(stderr, "stream did not start\n"); exit(1); }
    return w;
}

static es_ws_t *open_stream(int port, int connect_ms, int liveness_ms)
{
    char url[64];
    es_ws_opts_t o;
    es_ws_t *w;
    memset(&o, 0, sizeof o);
    snprintf(url, sizeof url, "ws://127.0.0.1:%d/agent", port);
    o.url = url; o.reconnect = 0; o.on_event = on_event;
    o.connect_timeout_ms = connect_ms; o.liveness_ms = liveness_ms;
    ups = 0; downs = 0; first_down_ms = 0; up_ms = 0; down_reason[0] = '\0';
    w = es_ws_create(&o);
    if (!w || es_ws_start(w) != 0) { fprintf(stderr, "stream did not start\n"); exit(1); }
    return w;
}
static void close_stream(es_ws_t *w) { es_ws_stop(w); es_ws_destroy(w); }

static void wait_ms(long ms) { long until = now_ms() + ms; while (now_ms() < until) usleep(10000); }
static int  wait_down(long limit_ms)
{
    long until = now_ms() + limit_ms;
    while (now_ms() < until && !downs) usleep(10000);
    return downs > 0;
}
static int  wait_up(long limit_ms)
{
    long until = now_ms() + limit_ms;
    while (now_ms() < until && !ups) usleep(10000);
    return ups > 0;
}

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("    !! FAIL " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(void)
{
    peer_t peer;
    es_ws_t *w;
    long t0, took;
    int port = 39400 + (int) (getpid() % 400);

    lws_set_log_level(0, NULL);
    es_ws_global_init();

    /* ---- an agent that accepts the socket and never answers ---- */
    peer_start(&peer, port, 0);

    printf("1. connect_timeout_ms=2000: the attempt is abandoned after about 2 s, reported once\n");
    t0 = now_ms(); w = open_stream(port, 2000, 0);
    CHECK(wait_down(6000), "no connect failure within 6 s");
    took = first_down_ms - t0;
    CHECK(took >= 1800 && took <= 3500, "abandoned after %ld ms, expected about 2000", took);
    CHECK(strcmp(down_reason, "connect timeout") == 0, "reason was '%s'", down_reason);
    wait_ms(700);
    CHECK(downs == 1, "the owner was told %d times", (int) downs);
    CHECK(ups == 0, "a connection was reported");
    close_stream(w);

    printf("2. default: nothing is abandoned in the first 5 s, and stopping a pending attempt is prompt\n");
    w = open_stream(port, 0, 0);
    wait_ms(5000);
    CHECK(downs == 0, "the default gave up after %ld ms", first_down_ms ? (long) first_down_ms : 0L);
    t0 = now_ms(); close_stream(w); took = now_ms() - t0;
    CHECK(took < 1500, "stopping a pending attempt took %ld ms", took);

    printf("3. connect_timeout_ms=24000: still waiting at 22 s (past the library's old 20 s limit), abandoned by 27 s\n");
    t0 = now_ms(); w = open_stream(port, 24000, 0);
    wait_ms(22000);
    CHECK(downs == 0, "gave up after %ld ms, before the 24 s asked for", (long) first_down_ms - t0);
    CHECK(wait_down(5000), "not abandoned by 27 s");
    took = first_down_ms - t0;
    CHECK(took >= 23500 && took <= 26500, "abandoned after %ld ms, expected about 24000", took);
    close_stream(w);
    peer_stop(&peer);

    /* ---- an agent that completes the handshake and then goes silent ---- */
    peer_start(&peer, port + 1, 1);

    printf("4. liveness_ms=2000: a silent peer is dropped about 2 s after connecting\n");
    w = open_stream(port + 1, 0, 2000);
    CHECK(wait_up(3000), "never connected");
    CHECK(wait_down(6000), "the silent peer was never dropped");
    took = first_down_ms - up_ms;
    CHECK(took >= 1800 && took <= 4000, "dropped after %ld ms, expected about 2000", took);
    close_stream(w);

    printf("5. liveness_ms=-1: the check is off, a silent peer stays connected\n");
    w = open_stream(port + 1, 0, -1);
    CHECK(wait_up(3000), "never connected");
    wait_ms(5000);
    CHECK(downs == 0 && es_ws_connected(w), "dropped after %ld ms with the check off", (long) first_down_ms - up_ms);
    close_stream(w);
    peer_stop(&peer);

    /* ---- an agent that is not there at all (nothing listens on the port) ---- */
    printf("6. reconnect_max=3: three failed attempts, then one 'gave up', then silence\n");
    t0 = now_ms(); w = open_retrying(port + 2, 3, 0);
    { long until = now_ms() + 8000; while (now_ms() < until && !gave_up) usleep(10000); }
    CHECK(gave_up == 1, "gave up %d times", (int) gave_up);
    CHECK(fails_seen == 3, "%d failed attempts were reported before giving up, expected 3", (int) fails_seen);
    wait_ms(1500);
    CHECK(gave_up == 1 && fails_seen == 3, "it kept going after giving up (%d failures, %d give-ups)", (int) fails_seen, (int) gave_up);
    close_stream(w);

    printf("7. reconnect_deadline_ms=1500: gives up about 1.5 s after going down\n");
    t0 = now_ms(); w = open_retrying(port + 2, 0, 1500);
    { long until = now_ms() + 8000; while (now_ms() < until && !gave_up) usleep(10000); }
    took = gave_up_ms - t0;
    CHECK(gave_up == 1, "gave up %d times", (int) gave_up);
    CHECK(took >= 1400 && took <= 3200, "gave up after %ld ms, expected about 1500", took);
    close_stream(w);

    printf("8. no limit set: still trying after 4 s, as before\n");
    w = open_retrying(port + 2, 0, 0);
    wait_ms(4000);
    CHECK(gave_up == 0 && fails_seen >= 3, "gave up %d times after %d failures with no limit set", (int) gave_up, (int) fails_seen);
    close_stream(w);

    es_ws_global_shutdown();
    printf(fails ? "\nFAILED: %d check(s)\n" : "\nok\n", fails);
    return fails ? 1 : 0;
}
