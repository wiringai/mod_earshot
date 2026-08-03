/*
 * es_pb — a minimal protobuf codec for the Pipecat frame wire format.
 *
 * Pure C, no FreeSWITCH dependency, so it is unit-testable off-box
 * (see ../test/test_pb.c). Only the small subset Earshot needs is implemented:
 * varints and the Pipecat `Frame` message.
 *
 *   Frame            oneof { TextFrame text=1; AudioRawFrame audio=2;
 *                            TranscriptionFrame transcription=3; MessageFrame message=4;
 *                            InterruptionFrame interruption=5; }
 *   AudioRawFrame    { uint64 id=1; string name=2; bytes audio=3;
 *                      uint32 sample_rate=4; uint32 num_channels=5; uint64 pts=6; }
 *
 * Copyright (c) 2026 Varun Pratap Singh. MIT License.
 */
#ifndef ES_PB_H
#define ES_PB_H

#include <stddef.h>
#include <stdint.h>

/* Base-128 varint. es_pb_varint writes up to 10 bytes and returns the count.
 * es_pb_read_varint returns the position after the varint, or NULL on truncation. */
size_t         es_pb_varint(uint8_t *buf, uint64_t v);
const uint8_t *es_pb_read_varint(const uint8_t *p, const uint8_t *end, uint64_t *out);

/*
 * Encode a Frame{ audio: AudioRawFrame{ audio, sample_rate, num_channels=1 } } into `out`.
 * Returns the number of bytes written, or 0 if it would not fit in `cap`.
 */
size_t es_pb_encode_audio(uint8_t *out, size_t cap,
                          const uint8_t *audio, size_t audio_len, uint32_t sample_rate);

/* Parsed Frame. `audio` points into the input buffer (no copy). */
typedef struct {
    int            has_audio;
    const uint8_t *audio;
    size_t         audio_len;
    uint32_t       sample_rate;
    int            interrupted;   /* an InterruptionFrame was present */
} es_pb_frame_t;

/* Parse a Frame. Returns 0 on success (out populated), -1 on malformed/truncated input. */
int es_pb_decode_frame(const uint8_t *data, size_t len, es_pb_frame_t *out);

#endif /* ES_PB_H */
