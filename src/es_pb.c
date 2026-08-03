/*
 * es_pb — minimal protobuf codec for Pipecat frames. Pure C, unit-tested.
 * Copyright (c) 2026 Varun Pratap Singh. MIT License.
 */
#include "es_pb.h"
#include <string.h>

size_t es_pb_varint(uint8_t *buf, uint64_t v)
{
    size_t n = 0;
    do {
        uint8_t b = v & 0x7f;
        v >>= 7;
        if (v) b |= 0x80;
        buf[n++] = b;
    } while (v);
    return n;
}

const uint8_t *es_pb_read_varint(const uint8_t *p, const uint8_t *end, uint64_t *out)
{
    uint64_t v = 0;
    int shift = 0;
    while (p < end && shift < 64) {
        uint8_t b = *p++;
        v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) { *out = v; return p; }
        shift += 7;
    }
    return NULL;   /* truncated or overlong */
}

size_t es_pb_encode_audio(uint8_t *out, size_t cap,
                          const uint8_t *audio, size_t audio_len, uint32_t sample_rate)
{
    uint8_t al_v[10], sr_v[10], nc_v[10], il_v[10];
    size_t al_n, sr_n, nc_n, il_n, inner_len, total, o = 0;

    if (!out || !audio) return 0;
    al_n = es_pb_varint(al_v, audio_len);
    sr_n = es_pb_varint(sr_v, sample_rate);
    nc_n = es_pb_varint(nc_v, 1);
    /* inner AudioRawFrame: tag3+len+audio, tag4+sr, tag5+nc */
    inner_len = 1 + al_n + audio_len + 1 + sr_n + 1 + nc_n;
    il_n = es_pb_varint(il_v, inner_len);
    total = 1 + il_n + inner_len;             /* outer Frame.audio tag + len + inner */
    if (total > cap) return 0;

    out[o++] = (2 << 3) | 2;                  /* Frame.audio (field 2, length-delimited) */
    memcpy(out + o, il_v, il_n); o += il_n;
    out[o++] = (3 << 3) | 2;                  /* AudioRawFrame.audio (field 3, bytes) */
    memcpy(out + o, al_v, al_n); o += al_n;
    memcpy(out + o, audio, audio_len); o += audio_len;
    out[o++] = (4 << 3) | 0;                  /* sample_rate (field 4, varint) */
    memcpy(out + o, sr_v, sr_n); o += sr_n;
    out[o++] = (5 << 3) | 0;                  /* num_channels (field 5, varint) */
    memcpy(out + o, nc_v, nc_n); o += nc_n;
    return o;
}

/* skip a field's payload given its wire type; returns new pos or NULL on malformed */
static const uint8_t *es_pb_skip(const uint8_t *pos, const uint8_t *end, int wt)
{
    uint64_t v;
    if (wt == 0) return es_pb_read_varint(pos, end, &v);      /* varint */
    if (wt == 2) {                                            /* length-delimited */
        pos = es_pb_read_varint(pos, end, &v);
        if (!pos || v > (uint64_t)(end - pos)) return NULL;   /* remaining-bytes check, no ptr math */
        return pos + v;
    }
    if (wt == 5) return (end - pos) >= 4 ? pos + 4 : NULL;    /* fixed32 */
    if (wt == 1) return (end - pos) >= 8 ? pos + 8 : NULL;    /* fixed64 */
    return NULL;                                              /* unknown/group */
}

int es_pb_decode_frame(const uint8_t *data, size_t len, es_pb_frame_t *out)
{
    const uint8_t *pos, *end;
    if (!data || !out) return -1;
    memset(out, 0, sizeof *out);
    pos = data; end = data + len;

    while (pos < end) {
        uint64_t tag, flen;
        int field, wt;
        const uint8_t *ip, *iend;

        pos = es_pb_read_varint(pos, end, &tag);
        if (!pos) return -1;
        field = (int)(tag >> 3);
        wt    = (int)(tag & 7);

        if (wt != 2) {                                        /* only length-delimited fields matter */
            pos = es_pb_skip(pos, end, wt);
            if (!pos) return -1;
            continue;
        }
        pos = es_pb_read_varint(pos, end, &flen);
        if (!pos || flen > (uint64_t)(end - pos)) return -1;  /* remaining-bytes check, no ptr overflow */
        ip = pos; iend = pos + flen;

        if (field == 2) {                                     /* AudioRawFrame */
            while (ip < iend) {
                uint64_t itag, ilen;
                int ifield, iwt;
                ip = es_pb_read_varint(ip, iend, &itag);
                if (!ip) return -1;
                ifield = (int)(itag >> 3);
                iwt    = (int)(itag & 7);
                if (iwt == 0) {
                    uint64_t v;
                    ip = es_pb_read_varint(ip, iend, &v);
                    if (!ip) return -1;
                    if (ifield == 4) out->sample_rate = (uint32_t) v;
                } else if (iwt == 2) {
                    ip = es_pb_read_varint(ip, iend, &ilen);
                    if (!ip || ilen > (uint64_t)(iend - ip)) return -1;
                    if (ifield == 3) { out->has_audio = 1; out->audio = ip; out->audio_len = (size_t) ilen; }
                    ip += ilen;
                } else {
                    ip = es_pb_skip(ip, iend, iwt);
                    if (!ip) return -1;
                }
            }
        } else if (field == 5) {
            out->interrupted = 1;                             /* InterruptionFrame */
        }
        pos += flen;
    }
    return 0;
}
