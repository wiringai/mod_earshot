#ifndef ES_CMDGUARD_H
#define ES_CMDGUARD_H

/*
 * Argument guards for the agent control channel.
 *
 * The uuid_* action whitelist is NOT a security boundary on its own: uuid_broadcast,
 * uuid_transfer and uuid_setvar can each execute a dialplan application, run a shell
 * command, or trigger a hook when handed hostile arguments (FreeSWITCH's "app::args"
 * form, the `inline` dialplan, and execute_on_ or api_on_ channel variables). The agent
 * supplies these arguments over the wire and may itself be adversarial or
 * prompt-injected, so the arguments are validated here before ever reaching
 * switch_api_execute().
 *
 * Every predicate returns 1 when the value is SAFE to use and 0 when it must be
 * rejected. Pure C (no FreeSWITCH, no cJSON) so it is unit-tested with the portable core.
 */

int es_cmd_text_ok(const char *s);      /* no control chars (spaces + empty allowed)                */
int es_cmd_token_ok(const char *s);     /* non-empty, no control chars, no whitespace, no "::"      */
int es_cmd_recpath_ok(const char *s);   /* token-safe and no ".." path traversal                   */
int es_cmd_leg_ok(const char *s);       /* empty, or exactly one of aleg|bleg|both                 */
int es_cmd_dialplan_ok(const char *s);  /* shape only: empty, or token-safe and not `inline`; see es_cmd_dialplan_allowed */
int es_cmd_varname_ok(const char *s);   /* identifier-shaped [A-Za-z0-9_] and not an exec var      */

/* Exact membership in a comma-separated allowlist (case-sensitive — FS variable names are —
 * whitespace-tolerant around commas).
 * Returns 0 for an empty/NULL list — i.e. setvar is fail-closed: nothing is settable unless the
 * operator names it in `setvars=`. A name denylist cannot secure setvar (many identifier-clean
 * variables such as `transfer_after_bridge` reach code execution through their value), so the
 * variable name must be an operator-chosen allowlist, not a filtered free field. */
int es_cmd_name_in_list(const char *csv, const char *name);

/* The three below are allowlists, where the checks above only refuse known-bad shapes.
 * Each takes what the operator configured on `start` (may be NULL or empty).
 *
 * Transfer dialplan: empty (FreeSWITCH's default) or XML, plus any the operator named in
 * `dialplans=`. Any other dialplan module runs code of its own with the agent's destination
 * as input (`LUA` runs a script as the dialplan); `inline` is refused even when named. */
int es_cmd_dialplan_allowed(const char *operator_csv, const char *dialplan);

/* A file argument for play: a plain path, or a stream whose scheme is harmless by itself
 * (tone_stream, silence_stream, local_stream) or one the operator named in `media_schemes=`.
 * Without that, http://, https://, shout:// and the like are refused: they make FreeSWITCH
 * fetch a URL of the agent's choosing from inside the network. */
int es_cmd_media_ok(const char *operator_schemes_csv, const char *file);

/* A file argument for record: as es_cmd_recpath_ok, and never a stream unless its scheme is in
 * `media_schemes=` (recording to shout:// sends the caller's audio off the box). A relative
 * path is left to FreeSWITCH's recordings directory. An absolute path must lie under one of
 * the directories the operator named in `record_dir=`; with none named, absolute paths are
 * refused, since a recording can overwrite any file FreeSWITCH can write. */
int es_cmd_record_ok(const char *operator_dirs_csv, const char *operator_schemes_csv, const char *file);

#endif /* ES_CMDGUARD_H */
