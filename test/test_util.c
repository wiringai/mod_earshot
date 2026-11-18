/* The small helpers that stand between untrusted input and a buffer, a resampler or a log. */
#include "../src/es_util.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("    !! FAIL " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static void redact_is(const char *url, int hide_path, const char *want)
{
    char out[256];
    es_url_redact(url, hide_path, out, sizeof out);
    CHECK(!strcmp(out, want), "redact(%s, %d) = '%s', want '%s'", url, hide_path, out, want);
}

int main(void)
{
    /* --- agent-declared sample rates --- */
    {
        static const long ok[] = { 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000 };
        static const long bad[] = { 1, 0, -8000, 7999, 8001, 96000, 2000000000L };
        size_t i;
        for (i = 0; i < sizeof ok / sizeof *ok; i++) CHECK(es_rate_supported(ok[i]), "%ld should be accepted", ok[i]);
        for (i = 0; i < sizeof bad / sizeof *bad; i++) CHECK(!es_rate_supported(bad[i]), "%ld should be refused", bad[i]);
    }

    /* --- key=value split: the key buffer is never overrun --- */
    {
        struct { char key[64]; char guard[8]; } b;
        char longkv[600];
        const char *val;

        memset(&b, 0, sizeof b);
        val = es_opt_split("codec=pcmu", b.key, sizeof b.key);
        CHECK(val && !strcmp(val, "pcmu") && !strcmp(b.key, "codec"), "plain option split wrong");

        val = es_opt_split("auth=", b.key, sizeof b.key);
        CHECK(val && !*val && !strcmp(b.key, "auth"), "empty value split wrong");

        val = es_opt_split("a=b=c", b.key, sizeof b.key);
        CHECK(val && !strcmp(val, "b=c") && !strcmp(b.key, "a"), "split must stop at the first '='");

        CHECK(es_opt_split("novalue", b.key, sizeof b.key) == NULL, "no '=' must return NULL");

        /* a 500-character key used to be copied whole into the 64-byte buffer */
        memset(longkv, 'k', 500); strcpy(longkv + 500, "=v");
        memcpy(b.guard, "GUARD!!", 8);
        val = es_opt_split(longkv, b.key, sizeof b.key);
        CHECK(val && !strcmp(val, "v"), "long key: value lost");
        CHECK(strlen(b.key) == sizeof b.key - 1, "long key: expected a %zu-char cut, got %zu", sizeof b.key - 1, strlen(b.key));
        CHECK(!memcmp(b.guard, "GUARD!!", 8), "long key wrote past the key buffer");
    }

    /* --- URLs shown to operators carry no credentials --- */
    redact_is("wss://agent.example.com/ws/call/42", 0, "wss://agent.example.com/ws/call/42");
    redact_is("wss://agent.example.com:8443/ws?token=sk-secret&x=1", 0, "wss://agent.example.com:8443/ws?...");
    redact_is("wss://api.cartesia.ai/stt/websocket?access_token=sk_car_secret", 0, "wss://api.cartesia.ai/stt/websocket?...");
    redact_is("wss://user:pa55@agent.example.com/ws", 0, "wss://agent.example.com/ws");
    redact_is("wss://user:p@ss@agent.example.com/ws?k=v", 0, "wss://agent.example.com/ws?...");
    redact_is("ws://10.0.0.5:8089", 0, "ws://10.0.0.5:8089");
    redact_is("ws://10.0.0.5:8089?token=abc", 0, "ws://10.0.0.5:8089?...");
    redact_is("wss://agent.example.com/ws#frag", 0, "wss://agent.example.com/ws");
    /* a per-call URL is the credential: keep only the host */
    redact_is("wss://phone.vapi.ai/call-secret-0123/transport?x=1", 1, "wss://phone.vapi.ai/...");
    redact_is("wss://phone.vapi.ai", 1, "wss://phone.vapi.ai");
    redact_is("wss://phone.vapi.ai?secret=1", 1, "wss://phone.vapi.ai");
    redact_is("", 0, "");
    {
        char small[12];
        es_url_redact("wss://agent.example.com/ws?token=secret", 0, small, sizeof small);
        CHECK(strlen(small) < sizeof small && !strstr(small, "secret"), "small buffer: '%s'", small);
    }

    printf(fails ? "util: %d FAILURE(S)\n" : "util: all passed\n", fails);
    return fails ? 1 : 0;
}
