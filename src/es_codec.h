/*
 * es_codec — audio (de)coding for Earshot. Pure C, no FreeSWITCH dependency,
 * so it is unit-testable off-box (see ../test/test_codec.c).
 *
 * FreeSWITCH channels carry 16-bit linear PCM (L16). On the wire we may send
 * L16, or G.711 (mu-law / a-law) to halve the bytes and match telephony.
 * Opus is a later tier and lives in its own unit.
 *
 * Copyright (c) 2026 Varun Pratap Singh. MIT License.
 */
#ifndef ES_CODEC_H
#define ES_CODEC_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    ES_CODEC_L16 = 0,   /* 16-bit linear PCM, host byte order (passthrough) */
    ES_CODEC_PCMU,      /* G.711 mu-law, 8-bit */
    ES_CODEC_PCMA,      /* G.711 a-law, 8-bit */
    ES_CODEC_OPUS       /* handled elsewhere; not implemented here */
} es_codec_t;

/* Single-sample conversions (branch-friendly, no allocation). */
uint8_t es_l16_to_ulaw(int16_t pcm);
int16_t es_ulaw_to_l16(uint8_t u);
uint8_t es_l16_to_alaw(int16_t pcm);
int16_t es_alaw_to_l16(uint8_t a);

/*
 * Frame encode: L16 samples -> codec bytes.
 *   in     : nsamples int16 samples
 *   out    : buffer of at least es_encoded_size(codec, nsamples) bytes
 * Returns the number of bytes written, or (size_t)-1 on unsupported codec.
 */
size_t es_encode(es_codec_t codec, const int16_t *in, size_t nsamples, uint8_t *out);

/*
 * Frame decode: codec bytes -> L16 samples.
 *   in     : nbytes codec bytes
 *   out    : buffer of at least es_decoded_samples(codec, nbytes) int16 samples
 * Returns the number of samples written, or (size_t)-1 on unsupported codec.
 */
size_t es_decode(es_codec_t codec, const uint8_t *in, size_t nbytes, int16_t *out);

/* Sizing helpers. */
size_t es_encoded_size(es_codec_t codec, size_t nsamples);
size_t es_decoded_samples(es_codec_t codec, size_t nbytes);

const char *es_codec_name(es_codec_t codec);
es_codec_t  es_codec_from_name(const char *name); /* defaults to ES_CODEC_L16 */

#endif /* ES_CODEC_H */
