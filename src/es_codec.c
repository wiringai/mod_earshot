/*
 * es_codec — G.711 mu-law / a-law + L16. Canonical ITU/Sun algorithms
 * (public-domain lineage), reimplemented and unit-tested.
 *
 * Copyright (c) 2026 Varun Pratap Singh. MIT License.
 */
#include "es_codec.h"
#include <string.h>
#include <strings.h>

/* ---- G.711 mu-law ------------------------------------------------------- */

#define ULAW_BIAS 0x84
#define ULAW_CLIP 32635

/* exponent of the highest segment for a given (biased) magnitude high byte */
static const uint8_t ulaw_exp[256] = {
    0,0,1,1,2,2,2,2,3,3,3,3,3,3,3,3,
    4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
    5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,
    5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,
    6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
    6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
    6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
    6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7
};

uint8_t es_l16_to_ulaw(int16_t pcm)
{
    int sample = pcm;   /* widen to int so -32768 negates without overflow */
    int sign, exponent, mantissa;
    uint8_t ubyte;

    sign = (sample >> 8) & 0x80;
    if (sign) sample = -sample;
    if (sample > ULAW_CLIP) sample = ULAW_CLIP;
    sample += ULAW_BIAS;

    exponent = ulaw_exp[(sample >> 7) & 0xFF];
    mantissa = (sample >> (exponent + 3)) & 0x0F;
    ubyte = (uint8_t)~(sign | (exponent << 4) | mantissa);
    return ubyte;
}

int16_t es_ulaw_to_l16(uint8_t u)
{
    static const int exp_lut[8] = { 0, 132, 396, 924, 1980, 4092, 8316, 16764 };
    int sign, exponent, mantissa, sample;

    u = (uint8_t)~u;
    sign = u & 0x80;
    exponent = (u >> 4) & 0x07;
    mantissa = u & 0x0F;
    sample = exp_lut[exponent] + (mantissa << (exponent + 3));
    if (sign) sample = -sample;
    return (int16_t)sample;
}

/* ---- G.711 a-law -------------------------------------------------------- */

/* Canonical ITU/Sun a-law (public-domain lineage). Encode scales input by >>3;
 * the decode's segment reconstruction restores full 16-bit scale. */
static const int16_t seg_aend[8] = { 0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF };

static int es_seg_search(int val, const int16_t *table, int size)
{
    int i;
    for (i = 0; i < size; i++) if (val <= table[i]) return i;
    return size;
}

uint8_t es_l16_to_alaw(int16_t pcm)
{
    int mask, seg, pcm_val = pcm >> 3;
    uint8_t aval;

    if (pcm_val >= 0) {
        mask = 0xD5;                 /* sign bit = 1 (positive) */
    } else {
        mask = 0x55;                 /* sign bit = 0 (negative) */
        pcm_val = -pcm_val - 1;
    }
    seg = es_seg_search(pcm_val, seg_aend, 8);
    if (seg >= 8) return (uint8_t)(0x7F ^ mask);

    aval = (uint8_t)(seg << 4);
    if (seg < 2) aval |= (pcm_val >> 1) & 0x0F;
    else         aval |= (pcm_val >> seg) & 0x0F;
    return (uint8_t)(aval ^ mask);
}

int16_t es_alaw_to_l16(uint8_t a)
{
    int t, seg;

    a ^= 0x55;
    t = (a & 0x0F) << 4;
    seg = ((unsigned)a & 0x70) >> 4;
    switch (seg) {
    case 0:  t += 8;                       break;
    case 1:  t += 0x108;                   break;
    default: t += 0x108; t <<= seg - 1;    break;
    }
    return (int16_t)((a & 0x80) ? t : -t);
}

/* ---- frame-level + dispatch -------------------------------------------- */

size_t es_encoded_size(es_codec_t codec, size_t nsamples)
{
    switch (codec) {
    case ES_CODEC_L16:  return nsamples * 2;
    case ES_CODEC_PCMU:
    case ES_CODEC_PCMA: return nsamples;      /* 1 byte/sample */
    default:            return 0;
    }
}

size_t es_decoded_samples(es_codec_t codec, size_t nbytes)
{
    switch (codec) {
    case ES_CODEC_L16:  return nbytes / 2;
    case ES_CODEC_PCMU:
    case ES_CODEC_PCMA: return nbytes;
    default:            return 0;
    }
}

size_t es_encode(es_codec_t codec, const int16_t *in, size_t nsamples, uint8_t *out)
{
    size_t i;
    switch (codec) {
    case ES_CODEC_L16:
        memcpy(out, in, nsamples * 2);
        return nsamples * 2;
    case ES_CODEC_PCMU:
        for (i = 0; i < nsamples; i++) out[i] = es_l16_to_ulaw(in[i]);
        return nsamples;
    case ES_CODEC_PCMA:
        for (i = 0; i < nsamples; i++) out[i] = es_l16_to_alaw(in[i]);
        return nsamples;
    default:
        return (size_t)-1;
    }
}

size_t es_decode(es_codec_t codec, const uint8_t *in, size_t nbytes, int16_t *out)
{
    size_t i, n;
    switch (codec) {
    case ES_CODEC_L16:
        memcpy(out, in, nbytes);
        return nbytes / 2;
    case ES_CODEC_PCMU:
        for (i = 0; i < nbytes; i++) out[i] = es_ulaw_to_l16(in[i]);
        return nbytes;
    case ES_CODEC_PCMA:
        for (i = 0; i < nbytes; i++) out[i] = es_alaw_to_l16(in[i]);
        return nbytes;
    default:
        n = 0; (void)n;
        return (size_t)-1;
    }
}

const char *es_codec_name(es_codec_t codec)
{
    switch (codec) {
    case ES_CODEC_L16:  return "l16";
    case ES_CODEC_PCMU: return "pcmu";
    case ES_CODEC_PCMA: return "pcma";
    case ES_CODEC_OPUS: return "opus";
    default:            return "unknown";
    }
}

es_codec_t es_codec_from_name(const char *name)
{
    if (!name) return ES_CODEC_L16;
    if (!strcasecmp(name, "pcmu") || !strcasecmp(name, "mulaw") || !strcasecmp(name, "ulaw")) return ES_CODEC_PCMU;
    if (!strcasecmp(name, "pcma") || !strcasecmp(name, "alaw")) return ES_CODEC_PCMA;
    if (!strcasecmp(name, "opus")) return ES_CODEC_OPUS;
    return ES_CODEC_L16;
}
