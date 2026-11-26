/*
 * es_util — small pure-C helpers shared by the module and the protocol adapters.
 * Copyright (c) 2026 Varun Pratap Singh. MIT.
 *
 * No FreeSWITCH, no allocation: everything here is unit-tested in test/test_util.c.
 */
#ifndef ES_UTIL_H
#define ES_UTIL_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Longest WebSocket URL a stream may be started with; a longer one is refused, not cut
 * short. The bound comes from libwebsockets: 4.1 and older format the request line
 * ("GET <path and query> HTTP/1.1") into 2048 bytes and silently cut what does not fit,
 * which sends a broken request (seen on 4.0.20 and 4.1.6). 2000 is whole everywhere. The
 * transport sizes its handshake buffers for this plus the headers it adds (es_ws.c). */
#define ES_URL_MAX 2000

/* Sample rates an agent may declare for the audio it sends. The rate drives a resampler
 * on the shared service thread, so it is not taken on trust: a declared rate of 1 Hz
 * asks for an 8000:1 upsample (over 100 MB of output per 8 KB of input) and stalls every
 * other call on that thread. These are the rates speech and TTS services produce. */
static inline int es_rate_supported(long hz)
{
    switch (hz) {
    case 8000: case 11025: case 12000: case 16000: case 22050:
    case 24000: case 32000: case 44100: case 48000:
        return 1;
    default:
        return 0;
    }
}

/* Split "key=value". Copies the key into `key` (always NUL-terminated, cut to fit) and
 * returns the value, or NULL when there is no '='. A key that does not fit matches no
 * option name, so the caller reports it as unrecognised like any other typo. */
static inline const char *es_opt_split(const char *kv, char *key, size_t klen)
{
    const char *eq = kv ? strchr(kv, '=') : NULL;
    size_t n;
    if (!eq || !key || !klen) return NULL;
    n = (size_t) (eq - kv);
    if (n > klen - 1) n = klen - 1;
    memcpy(key, kv, n);
    key[n] = '\0';
    return eq + 1;
}

/* A copy of a WebSocket URL that is safe to put in an event, a log line or an API reply.
 * Credentials ride in URLs: a query string (`?token=`, `?access_token=`), a userinfo part
 * (`user:pass@host`) or, for services that hand out one URL per call, the path itself.
 * The query and the userinfo are always removed; `hide_path` removes the path as well.
 * What remains says which host the stream went to, which is what an operator needs. */
static inline void es_url_redact(const char *url, int hide_path, char *out, size_t olen)
{
    const char *p, *auth, *end, *at, *cut;
    if (!out || !olen) return;
    out[0] = '\0';
    if (!url) return;

    p = strstr(url, "://");
    auth = p ? p + 3 : url;                       /* start of [userinfo@]host[:port] */
    end = auth + strcspn(auth, "/?#");            /* end of the authority */
    for (at = NULL, p = auth; p < end; p++) if (*p == '@') at = p;   /* last '@' ends userinfo */

    cut = hide_path ? end : end + strcspn(end, "?#");
    snprintf(out, olen, "%.*s%.*s%s",
             (int) (auth - url), url,
             (int) (cut - (at ? at + 1 : auth)), at ? at + 1 : auth,
             hide_path ? (*cut == '/' ? "/..." : "") : (*cut == '?' ? "?..." : ""));
}

#endif /* ES_UTIL_H */
