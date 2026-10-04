/* Test-only stubs so es_proto.c can link/run without FreeSWITCH:
 *  - trivial switch_* implementations (none exercised on the command path)
 *  - no-op es_ws_* (the command path never sends)
 *  - a minimal cJSON for flat {"k":"string"} objects (all command payloads are flat)
 * This lets the REAL es_proto_on_text -> es_dispatch_command -> es_cmdguard run. */
#include <switch.h>
#include "es_ws.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>

size_t switch_copy_string(char *d, const char *s, size_t n)
{ size_t i = 0; if (!n) return 0; for (; s && s[i] && i < n - 1; i++) d[i] = s[i]; d[i] = 0; return i; }
void switch_uuid_get(switch_uuid_t *u) { if (u) memset(u, 0, sizeof *u); }
void switch_uuid_format(char *b, switch_uuid_t *u) { (void) u; if (b) strcpy(b, "00000000-0000-0000-0000-000000000000"); }
int switch_resample_create(switch_audio_resampler_t **r, int a, int b, int c, int d, int e)
{ (void) a;(void) b;(void) c;(void) d;(void) e; if (r) *r = 0; return 0; }
void switch_resample_destroy(switch_audio_resampler_t **r) { if (r) *r = 0; }
int switch_resample_process(switch_audio_resampler_t *r, int16_t *x, int n) { (void) r;(void) x;(void) n; return 0; }
switch_size_t switch_b64_encode(unsigned char *s, switch_size_t sl, unsigned char *d, switch_size_t dl)
{ (void) s;(void) sl;(void) dl; if (d) d[0] = 0; return 0; }
/* real base64 decode (RFC 4648, '=' padding, no line breaks), FreeSWITCH-style: returns bytes
 * written and NUL-terminates when room allows; stops at the output capacity */
switch_size_t switch_b64_decode(char *s, char *d, switch_size_t dl)
{
    static const char *tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned v = 0; int bits = 0; switch_size_t n = 0;
    if (!s || !d || !dl) return 0;
    for (; *s && *s != '='; s++) {
        const char *q = strchr(tbl, *s);
        if (!q) continue;
        v = (v << 6) | (unsigned) (q - tbl); bits += 6;
        if (bits >= 8) { bits -= 8; if (n + 1 >= dl) break; d[n++] = (char) ((v >> bits) & 0xFF); }
    }
    if (n < dl) d[n] = 0;
    return n;
}
switch_size_t switch_b64_encode_real(const unsigned char *in, switch_size_t il, char *out, switch_size_t ol)
{
    static const char *tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    switch_size_t i, o = 0;
    for (i = 0; i + 2 < il; i += 3) {
        if (o + 4 >= ol) return 0;
        out[o++] = tbl[in[i] >> 2]; out[o++] = tbl[((in[i] & 3) << 4) | (in[i + 1] >> 4)];
        out[o++] = tbl[((in[i + 1] & 15) << 2) | (in[i + 2] >> 6)]; out[o++] = tbl[in[i + 2] & 63];
    }
    if (i < il) {
        if (o + 4 >= ol) return 0;
        out[o++] = tbl[in[i] >> 2];
        if (i + 1 < il) { out[o++] = tbl[((in[i] & 3) << 4) | (in[i + 1] >> 4)]; out[o++] = tbl[(in[i + 1] & 15) << 2]; }
        else            { out[o++] = tbl[(in[i] & 3) << 4]; out[o++] = '='; }
        out[o++] = '=';
    }
    out[o] = 0; return o;
}

int es_ws_send_text(es_ws_t *w, const char *d, size_t l) { (void) w;(void) d;(void) l; return 0; }
int es_ws_send_binary(es_ws_t *w, const void *d, size_t l) { (void) w;(void) d;(void) l; return 0; }

/* --- minimal cJSON (flat object, string values; sufficient for command JSON) --- */
static const char *skipws(const char *s) { while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++; return s; }
static char *parse_string(const char **sp)
{
    const char *s = *sp; size_t cap = 32, len = 0; char *out;
    if (*s != '"') return 0;
    s++; out = malloc(cap);
    while (*s && *s != '"') {
        char c = *s++;
        if (c == '\\' && *s) { char e = *s++; c = e == 'n' ? '\n' : e == 'r' ? '\r' : e == 't' ? '\t' : e; }
        if (len + 1 >= cap) { cap *= 2; out = realloc(out, cap); }
        out[len++] = c;
    }
    if (*s == '"') s++;
    out[len] = 0; *sp = s; return out;
}
cJSON *cJSON_Parse(const char *value)
{
    const char *s = skipws(value); cJSON *root, *tail = 0;
    if (*s != '{') return 0;
    s++; root = calloc(1, sizeof *root); s = skipws(s);
    while (*s && *s != '}') {
        char *key, *val = 0; cJSON *it;
        s = skipws(s); key = parse_string(&s); if (!key) break;
        s = skipws(s); if (*s == ':') s++; s = skipws(s);
        if (*s == '"') val = parse_string(&s);
        else { const char *b = s; size_t n; while (*s && *s != ',' && *s != '}') s++; n = s - b; val = malloc(n + 1); memcpy(val, b, n); val[n] = 0; }
        it = calloc(1, sizeof *it); it->string = key; it->valuestring = val;
        if (tail) tail->next = it; else root->child = it; tail = it;
        s = skipws(s); if (*s == ',') s++; s = skipws(s);
    }
    return root;
}
cJSON *cJSON_GetObjectItem(const cJSON *obj, const char *key)
{
    cJSON *c;
    if (!obj) return 0;
    for (c = obj->child; c; c = c->next) if (c->string && !strcasecmp(c->string, key)) return c;
    return 0;
}
int cJSON_IsTrue(const cJSON *i)
{ return i && i->valuestring && (!strcasecmp(i->valuestring, "true") || !strcmp(i->valuestring, "1")); }
void cJSON_Delete(cJSON *i)
{ while (i) { cJSON *n = i->next; if (i->child) cJSON_Delete(i->child); free(i->string); free(i->valuestring); free(i); i = n; } }
