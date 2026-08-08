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
