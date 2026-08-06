/* Standalone driver for the REAL es_ws.c transport (no FreeSWITCH), to verify the
 * box-level mTLS path: reads EARSHOT_TLS_* from the env exactly like production,
 * connects to a wss URL, and reports whether the handshake succeeded.
 *
 *   argv[1] = url (default wss://localhost:9443/)
 *   argv[2] = seconds to wait (default 5)
 * exit 0 = connected, 1 = not connected, 2 = harness error
 */
#include "es_ws.h"
#include <libwebsockets.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile int g_state = -1;      /* -1 pending, 1 connected, 0 closed/error */
static char g_reason[256];

static void on_event(void *user, int connected, int code, const char *reason) {
    (void) user; (void) code;
    if (connected) {
        g_state = 1;
    } else {
        if (g_state != 1) g_state = 0;
        if (reason && !g_reason[0]) snprintf(g_reason, sizeof g_reason, "%s", reason);
    }
}

int main(int argc, char **argv) {
    const char *url = argc > 1 ? argv[1] : "wss://localhost:9443/";
    int wait_s = argc > 2 ? atoi(argv[2]) : 5;

    if (getenv("ES_TEST_VERBOSE"))
        lws_set_log_level(LLL_ERR|LLL_WARN|LLL_NOTICE|LLL_INFO|LLL_CLIENT, NULL);
    if (es_ws_global_init() != 0) { fprintf(stderr, "global_init failed\n"); return 2; }

    es_ws_opts_t o; memset(&o, 0, sizeof o);
    o.url = url;
    o.reconnect = 0;                    /* single attempt — we want the first verdict */
    o.on_event = on_event;
    if (getenv("ES_TEST_INSECURE")) o.insecure = 1;   /* skip server-cert verification */

    es_ws_t *w = es_ws_create(&o);
    if (!w || es_ws_start(w) != 0) { fprintf(stderr, "create/start failed\n"); return 2; }

    for (int i = 0; i < wait_s * 10 && g_state == -1; i++) usleep(100000);
    int ok = es_ws_connected(w);

    printf("RESULT: %s%s%s\n", ok ? "CONNECTED" : "NOT-CONNECTED",
           g_reason[0] ? "  reason=" : "", g_reason);

    es_ws_stop(w);
    es_ws_destroy(w);
    es_ws_global_shutdown();
    return ok ? 0 : 1;
}
