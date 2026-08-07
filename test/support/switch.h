/*
 * TEST-ONLY stub of <switch.h>.
 *
 * This is NOT FreeSWITCH. It exists so the portable unit-test build can compile the real
 * src/es_proto.c (which includes <switch.h> and uses FreeSWITCH's cJSON) without the
 * FreeSWITCH dev headers, so the control-channel dispatch path can be tested in CI.
 *
 * It is on the include path ONLY for the test_command_dispatch target (see CMakeLists.txt),
 * never for the real module build. If es_proto.c starts using a new FreeSWITCH symbol,
 * add a matching declaration here and (if it is called on the command path) a stub
 * implementation in test/support/command_dispatch_stubs.c.
 */
#ifndef EARSHOT_TEST_SWITCH_STUB_H
#define EARSHOT_TEST_SWITCH_STUB_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>

#define SWITCH_UUID_FORMATTED_LENGTH 36
#define SWITCH_RESAMPLE_QUALITY 2
#define SWITCH_RECOMMENDED_BUFFER_SIZE 8192

typedef int switch_bool_t;
typedef unsigned long switch_size_t;
typedef struct switch_audio_resampler { int from_rate, to_rate; int16_t *to; unsigned to_len; } switch_audio_resampler_t;
typedef struct { unsigned char b[16]; } switch_uuid_t;

typedef struct cJSON {
    struct cJSON *next, *prev, *child;
    int type; char *valuestring; double valuedouble; int valueint; char *string;
} cJSON;
cJSON *cJSON_Parse(const char *value);
void   cJSON_Delete(cJSON *item);
cJSON *cJSON_GetObjectItem(const cJSON *object, const char *key);
int    cJSON_IsTrue(const cJSON *item);
#define cJSON_ArrayForEach(e, arr) for ((e) = (arr) ? (arr)->child : 0; (e); (e) = (e)->next)

size_t switch_copy_string(char *dst, const char *src, size_t n);
void   switch_uuid_get(switch_uuid_t *u);
void   switch_uuid_format(char *buf, switch_uuid_t *u);
int    switch_resample_create(switch_audio_resampler_t **r, int a, int b, int c, int d, int e);
void   switch_resample_destroy(switch_audio_resampler_t **r);
int    switch_resample_process(switch_audio_resampler_t *r, int16_t *x, int n);
switch_size_t switch_b64_encode(unsigned char *in, switch_size_t ilen, unsigned char *out, switch_size_t olen);
switch_size_t switch_b64_decode(char *in, char *out, switch_size_t olen);

#endif /* EARSHOT_TEST_SWITCH_STUB_H */
