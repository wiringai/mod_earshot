/* Control-channel argument guards — see es_cmdguard.h for the threat model. */
#include "es_cmdguard.h"
#include <string.h>
#include <strings.h>

static int has_ctrl(const char *s)
{
    if (!s) return 0;
    for (; *s; s++)
        if ((unsigned char) *s < 0x20 || (unsigned char) *s == 0x7f) return 1;
    return 0;
}

/* Whitespace is forbidden in positional token arguments: the dispatcher space-joins fields
 * into one uuid_* argument string, so a space inside a field forges the NEXT positional
 * argument (e.g. a `transfer` destination "system:id inline" smuggles the `inline` dialplan,
 * whose app/data separator is a single colon that the "::" check never sees). */
static int has_space(const char *s)
{
    return s && strpbrk(s, " \t\n\r\v\f") != 0;
}

/* FreeSWITCH's "application::arguments" form turns a file/destination argument into an
 * application invocation (system::, lua::, bgsystem::, playback::, …) — i.e. code exec. */
static int has_appexec(const char *s)
{
    return s && strstr(s, "::") != 0;
}

/* A plain FreeSWITCH channel-variable identifier: letters, digits, underscore. Anything
 * else (colon, dash, dot, space) is rejected — this blocks nolocal:, sip_h_X-Header, and
 * similar injection shapes before the exec-family check even runs. */
static int is_identifier(const char *s)
{
    if (!s || !*s) return 0;
    for (; *s; s++) {
        char c = *s;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_')) return 0;
    }
    return 1;
}

/* Channel variables that make FreeSWITCH execute an app/API or run a hook. Deny by broad
 * substring so the whole family is covered: "exec" catches execute_on_*, *_pre_execute_*_app
 * and *_post_process_exec_api; "hook" catches every *_hangup_hook; "api_on" catches api_on_*.
 * An agent must never be able to set any of these. */
static int is_exec_var(const char *name)
{
    static const char *const bad[] = { "exec", "hook", "api_on", "nolocal", "sip_h", "xml_cdr", 0 };
    int i;
    const char *h;
    size_t nl;
    for (i = 0; bad[i]; i++) {
        nl = strlen(bad[i]);
        for (h = name; *h; h++)
            if (!strncasecmp(h, bad[i], nl)) return 1;
    }
    return 0;
}

int es_cmd_text_ok(const char *s)     { return !has_ctrl(s); }   /* value/cause: spaces allowed, control chars not */

int es_cmd_token_ok(const char *s)    { return s && *s && !has_ctrl(s) && !has_space(s) && !has_appexec(s); }

int es_cmd_recpath_ok(const char *s)  { return es_cmd_token_ok(s) && !strstr(s, ".."); }

int es_cmd_leg_ok(const char *s)
{
    if (!s || !*s) return 1;
    return !strcasecmp(s, "aleg") || !strcasecmp(s, "bleg") || !strcasecmp(s, "both");
}

int es_cmd_dialplan_ok(const char *s)
{
    if (!s || !*s) return 1;
    if (has_ctrl(s) || has_space(s) || has_appexec(s)) return 0;  /* space would smuggle a later positional arg */
    return strcasecmp(s, "inline") != 0;   /* the `inline` dialplan executes its extension string as apps */
}

int es_cmd_varname_ok(const char *s)
{
    return is_identifier(s) && !is_exec_var(s);   /* identifier-shaped AND not an exec-triggering var */
}

int es_cmd_name_in_list(const char *csv, const char *name)
{
    size_t nl = name ? strlen(name) : 0;
    const char *p = csv;
    if (!nl || !csv) return 0;                     /* fail closed: empty allowlist => nothing settable */
    while (*p) {
        size_t seg, t;
        while (*p == ',' || *p == ' ') p++;        /* skip separators/leading space */
        seg = strcspn(p, ",");
        t = seg;
        while (t && p[t - 1] == ' ') t--;          /* trim trailing space in the segment */
        if (t == nl && !strncmp(p, name, nl)) return 1;   /* exact match: FS variable names are case-sensitive */
        p += seg;
        if (*p == ',') p++;
    }
    return 0;
}

/* Case-insensitive membership of the first `nl` bytes of `name` in a comma-separated list. */
static int in_list_ci(const char *csv, const char *name, size_t nl)
{
    const char *p = csv;
    if (!nl || !csv) return 0;
    while (*p) {
        size_t seg, t;
        while (*p == ',' || *p == ' ') p++;
        seg = strcspn(p, ",");
        t = seg;
        while (t && p[t - 1] == ' ') t--;
        if (t == nl && !strncasecmp(p, name, nl)) return 1;
        p += seg;
        if (*p == ',') p++;
    }
    return 0;
}

int es_cmd_dialplan_allowed(const char *operator_csv, const char *dialplan)
{
    if (!dialplan || !*dialplan) return 1;
    if (!es_cmd_dialplan_ok(dialplan)) return 0;               /* inline, spaces, app::args: never */
    if (!strcasecmp(dialplan, "XML")) return 1;
    return in_list_ci(operator_csv, dialplan, strlen(dialplan));
}

/* The scheme of "scheme://rest" as a length, 0 when `s` is not of that form. */
static size_t scheme_len(const char *s)
{
    const char *sep = s ? strstr(s, "://") : 0;
    return sep ? (size_t) (sep - s) : 0;
}

static int scheme_allowed(const char *operator_csv, const char *s, size_t n, int builtin_ok)
{
    static const char *const harmless[] = { "tone_stream", "silence_stream", "local_stream", 0 };
    int i;
    if (builtin_ok)
        for (i = 0; harmless[i]; i++)
            if (strlen(harmless[i]) == n && !strncasecmp(s, harmless[i], n)) return 1;
    return in_list_ci(operator_csv, s, n);
}

int es_cmd_media_ok(const char *operator_schemes_csv, const char *file)
{
    size_t n;
    if (!es_cmd_token_ok(file)) return 0;
    if (strstr(file, "://") == file) return 0;                 /* "://x": no scheme at all */
    n = scheme_len(file);
    return n ? scheme_allowed(operator_schemes_csv, file, n, 1) : 1;
}

int es_cmd_record_ok(const char *operator_dirs_csv, const char *operator_schemes_csv, const char *file)
{
    size_t n;
    const char *p = operator_dirs_csv;
    if (!es_cmd_recpath_ok(file)) return 0;
    if (strstr(file, "://") == file) return 0;
    n = scheme_len(file);
    if (n) return scheme_allowed(operator_schemes_csv, file, n, 0);   /* nothing is harmless to record to */
    if (file[0] != '/') return 1;                              /* relative: FreeSWITCH's recordings directory */
    if (!p) return 0;
    while (*p) {                                               /* absolute: under a named directory */
        size_t seg, t;
        while (*p == ',' || *p == ' ') p++;
        seg = strcspn(p, ",");
        t = seg;
        while (t && (p[t - 1] == ' ' || p[t - 1] == '/')) t--; /* "/var/rec/" and "/var/rec" are the same directory */
        if (t && p[0] == '/' && !strncmp(file, p, t) && file[t] == '/') return 1;
        p += seg;
        if (*p == ',') p++;
    }
    return 0;
}
