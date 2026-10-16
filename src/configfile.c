/* configfile.c - see configfile.h. A hand-written INI-subset reader: no
 * third-party parser, unknown keys fail fast (design D5). */
#include "configfile.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "executor.h"
#include "util.h"

/* One parsed `key = value` pair. */
typedef struct {
    char *key;
    char *val;
} PLine;

typedef struct {
    PLine *items;
    int    count;
    int    cap;
} PSect;

static void sect_free(PSect *s)
{
    for (int i = 0; i < s->count; i++) {
        free(s->items[i].key);
        free(s->items[i].val);
    }
    free(s->items);
    s->items = NULL;
    s->count = s->cap = 0;
}

static int sect_add(PSect *s, const char *key, const char *val)
{
    if (s->count == s->cap) {
        int nc = s->cap ? s->cap * 2 : 8;
        PLine *ni = realloc(s->items, sizeof(PLine) * (size_t)nc);
        if (!ni)
            return -1;
        s->items = ni;
        s->cap = nc;
    }
    s->items[s->count].key = xstrdup(key);
    s->items[s->count].val = xstrdup(val);
    s->count++;
    return 0;
}

static const char *sect_get(const PSect *s, const char *key)
{
    for (int i = 0; i < s->count; i++) {
        if (strcmp(s->items[i].key, key) == 0)
            return s->items[i].val;
    }
    return NULL;
}

/* Trim leading/trailing whitespace; returns a pointer into `s`. */
static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1]))
        s[--n] = '\0';
    return s;
}

/* Parse a non-negative integer; returns -1 on garbage/overflow. */
static int parse_int(const char *s, int *out)
{
    errno = 0;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (errno || end == s || *trim(end) != '\0' || v < 0 || v > INT_MAX)
        return -1;
    *out = (int)v;
    return 0;
}

/* Split a comma-separated value into a freshly-allocated NULL-terminated list. */
static char **split_csv(const char *val, int *count_out)
{
    char *work = xstrdup(val);
    char **list = NULL;
    int n = 0, cap = 0;
    char *save = NULL;
    for (char *tok = strtok_r(work, ",", &save); tok;
         tok = strtok_r(NULL, ",", &save)) {
        tok = trim(tok);
        if (!*tok)
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 4;
            char **nl = realloc(list, sizeof(char *) * (size_t)cap);
            if (!nl)
                break;
            list = nl;
        }
        list[n++] = xstrdup(tok);
    }
    free(work);
    *count_out = n;
    return list;
}

static void free_csv(char **list, int n)
{
    for (int i = 0; i < n; i++)
        free(list[i]);
    free(list);
}

/* ---------- [global] ---------- */

static int apply_global(Config *cfg, const PSect *s, char *err, size_t errlen)
{
    for (int i = 0; i < s->count; i++) {
        const char *k = s->items[i].key;
        const char *v = s->items[i].val;
        int iv;

        if (strcmp(k, "rcon_host") == 0) {
            free(cfg->rcon_host); cfg->rcon_host = xstrdup(v);
        } else if (strcmp(k, "rcon_port") == 0) {
            if (parse_int(v, &iv)) goto bad_int;
            cfg->rcon_port = iv;
        } else if (strcmp(k, "rcon_pass") == 0) {
            free(cfg->rcon_pass); cfg->rcon_pass = xstrdup(v);
        } else if (strcmp(k, "log_file") == 0) {
            free(cfg->log_file); cfg->log_file = xstrdup(v);
        } else if (strcmp(k, "prefix") == 0) {
            free(cfg->prefix); cfg->prefix = xstrdup(v);
        } else if (strcmp(k, "reply_prefix") == 0) {
            free(cfg->reply_prefix); cfg->reply_prefix = xstrdup(v);
        } else if (strcmp(k, "pi_cmd") == 0) {
            free(cfg->pi_cmd); cfg->pi_cmd = xstrdup(v);
        } else if (strcmp(k, "pi_model") == 0) {
            free(cfg->pi_model); cfg->pi_model = xstrdup(v);
        } else if (strcmp(k, "timeout") == 0) {
            if (parse_int(v, &iv)) goto bad_int;
            cfg->pi_timeout_ms = iv * 1000;
        } else if (strcmp(k, "cooldown") == 0) {
            if (parse_int(v, &iv)) goto bad_int;
            cfg->cooldown_ms = iv * 1000;
        } else if (strcmp(k, "max_len") == 0) {
            if (parse_int(v, &iv)) goto bad_int;
            cfg->max_len = iv;
        } else if (strcmp(k, "poll_ms") == 0) {
            if (parse_int(v, &iv)) goto bad_int;
            cfg->poll_ms = iv;
        } else if (strcmp(k, "queue_max") == 0) {
            if (parse_int(v, &iv)) goto bad_int;
            cfg->queue_max = iv;
        } else if (strcmp(k, "allow") == 0) {
            free(cfg->allow); cfg->allow = xstrdup(v);
        } else if (strcmp(k, "log_level") == 0) {
            if (strcasecmp(v, "debug") == 0) cfg->log_level = LOG_DEBUG;
            else if (strcasecmp(v, "info") == 0) cfg->log_level = LOG_INFO;
            else if (strcasecmp(v, "warn") == 0) cfg->log_level = LOG_WARN;
            else if (strcasecmp(v, "error") == 0) cfg->log_level = LOG_ERROR;
            else {
                snprintf(err, errlen,
                         "invalid log_level '%s' in [global] (want "
                         "debug|info|warn|error)", v);
                return -1;
            }
        } else {
            snprintf(err, errlen, "unknown key '%s' in [global]", k);
            return -1;
        }
        continue;
bad_int:
        snprintf(err, errlen, "invalid integer for '%s' in [global]: '%s'", k,
                 v);
        return -1;
    }
    return 0;
}

/* ---------- [trigger:<name>] ---------- */

/* Map an executor-parameter key to its Trigger field. Returns false if `key`
 * is not an executor parameter. */
static bool set_trigger_param(Trigger *tr, const char *k, const char *v)
{
    if (strcmp(k, "template") == 0) {
        free(tr->template); tr->template = xstrdup(v);
    } else if (strcmp(k, "model") == 0) {
        free(tr->model); tr->model = xstrdup(v);
    } else if (strcmp(k, "reply_prefix") == 0) {
        free(tr->reply_prefix); tr->reply_prefix = xstrdup(v);
    } else if (strcmp(k, "reply_shaper") == 0) {
        free(tr->reply_shaper); tr->reply_shaper = xstrdup(v);
    } else {
        return false; /* max_len and timeout handled by the caller (numeric) */
    }
    return true;
}

static int apply_trigger(Config *cfg, const char *name, const PSect *s,
                         char *err, size_t errlen)
{
    Trigger *tr = trigger_table_append(&cfg->triggers);
    if (!tr) {
        snprintf(err, errlen, "out of memory adding trigger '%s'", name);
        return -1;
    }
    tr->name = xstrdup(name);
    tr->cooldown_ms = -1; /* inherit unless the section sets it */

    /* Resolve the executor first so parameter keys can be validated against
     * the executor that will actually consume them. */
    const char *exec_name = sect_get(s, "executor");
    const ExecutorOps *ops = executor_lookup(exec_name ? exec_name : "dialog");
    if (!ops) {
        snprintf(err, errlen,
                 "trigger '%s' references unknown executor '%s'", name,
                 exec_name ? exec_name : "(none)");
        return -1;
    }
    free(tr->executor);
    tr->executor = xstrdup(ops->name);

    /* match kind (chat unless stated). */
    const char *mv = sect_get(s, "match");
    if (!mv || strcasecmp(mv, "chat") == 0)
        tr->match = MATCH_CHAT;
    else if (strcasecmp(mv, "keyword") == 0)
        tr->match = MATCH_KEYWORD;
    else {
        snprintf(err, errlen, "trigger '%s' has invalid match '%s' "
                 "(want chat|keyword)", name, mv);
        return -1;
    }

    for (int i = 0; i < s->count; i++) {
        const char *k = s->items[i].key;
        const char *v = s->items[i].val;
        int iv;

        if (strcmp(k, "match") == 0 || strcmp(k, "executor") == 0) {
            continue; /* handled above */
        } else if (strcmp(k, "keywords") == 0) {
            tr->keywords = split_csv(v, &tr->keyword_count);
            if (tr->keyword_count == 0) {
                snprintf(err, errlen, "trigger '%s' has an empty keywords list",
                         name);
                return -1;
            }
        } else if (strcmp(k, "extractor") == 0) {
            free(tr->extractor); tr->extractor = xstrdup(v);
        } else if (strcmp(k, "cooldown") == 0) {
            if (parse_int(v, &iv)) goto bad_int;
            tr->cooldown_ms = iv * 1000;
        } else if (strcmp(k, "allow") == 0) {
            free_csv(tr->allow, tr->allow_count);
            tr->allow = split_csv(v, &tr->allow_count);
        } else if (strcmp(k, "max_len") == 0) {
            if (!executor_accepts_key(ops, k)) goto not_this_exec;
            if (parse_int(v, &iv)) goto bad_int;
            tr->max_len = iv;
        } else if (strcmp(k, "timeout") == 0) {
            if (!executor_accepts_key(ops, k)) goto not_this_exec;
            if (parse_int(v, &iv)) goto bad_int;
            tr->timeout_ms = iv * 1000;
        } else if (set_trigger_param(tr, k, v)) {
            if (!executor_accepts_key(ops, k)) goto not_this_exec;
        } else {
            snprintf(err, errlen, "unknown key '%s' in section [trigger:%s]", k,
                     name);
            return -1;
        }
        continue;
not_this_exec:
        snprintf(err, errlen,
                 "key '%s' is not valid for executor '%s' (trigger '%s')", k,
                 ops->name, name);
        return -1;
bad_int:
        snprintf(err, errlen,
                 "invalid integer for '%s' in [trigger:%s]: '%s'", k, name, v);
        return -1;
    }

    /* Default the extractor to the one that implements the chosen match kind;
     * trigger_validate then enforces consistency. */
    if (!tr->extractor)
        tr->extractor = xstrdup(tr->match == MATCH_CHAT ? "chat" : "keyword");
    return 0;
}

/* ---------- driver ---------- */

int configfile_load(const char *path, Config *cfg, char *err, size_t errlen)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(err, errlen, "cannot open config file '%s'", path);
        return -1;
    }

    enum { SEC_NONE, SEC_GLOBAL, SEC_TRIGGER } kind = SEC_NONE;
    char trigger_name[128] = "";
    PSect sect = { 0 };
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    int rc = 0;
    int lineno = 0;

    while ((n = getline(&line, &cap, f)) != -1) {
        lineno++;
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';')
            continue;

        if (*s == '[') {
            /* Close the previous section. */
            if (kind == SEC_GLOBAL) {
                if (apply_global(cfg, &sect, err, errlen) != 0) { rc = -1; break; }
            } else if (kind == SEC_TRIGGER) {
                if (apply_trigger(cfg, trigger_name, &sect, err, errlen) != 0) {
                    rc = -1; break;
                }
            }
            sect_free(&sect);

            char *close = strchr(s, ']');
            if (!close) {
                snprintf(err, errlen, "line %d: malformed section header", lineno);
                rc = -1; break;
            }
            *close = '\0';
            char *inner = trim(s + 1);
            if (strcmp(inner, "global") == 0) {
                kind = SEC_GLOBAL;
            } else if (strncmp(inner, "trigger:", 8) == 0) {
                const char *nm = trim(inner + 8);
                if (!*nm) {
                    snprintf(err, errlen,
                             "line %d: section [trigger:] needs a name", lineno);
                    rc = -1; break;
                }
                snprintf(trigger_name, sizeof(trigger_name), "%s", nm);
                kind = SEC_TRIGGER;
            } else {
                snprintf(err, errlen,
                         "line %d: unknown section '[%s]' (want [global] or "
                         "[trigger:<name>])", lineno, inner);
                rc = -1; break;
            }
            continue;
        }

        /* key = value */
        char *eq = strchr(s, '=');
        if (!eq) {
            snprintf(err, errlen, "line %d: expected 'key = value'", lineno);
            rc = -1; break;
        }
        *eq = '\0';
        char *key = trim(s);
        char *val = trim(eq + 1);
        if (!*key) {
            snprintf(err, errlen, "line %d: empty key", lineno);
            rc = -1; break;
        }
        if (kind == SEC_NONE) {
            snprintf(err, errlen, "line %d: key '%s' appears before any section",
                     lineno, key);
            rc = -1; break;
        }
        if (sect_add(&sect, key, val) != 0) {
            snprintf(err, errlen, "out of memory parsing config");
            rc = -1; break;
        }
    }

    if (rc == 0) {
        if (kind == SEC_GLOBAL) {
            if (apply_global(cfg, &sect, err, errlen) != 0)
                rc = -1;
        } else if (kind == SEC_TRIGGER) {
            if (apply_trigger(cfg, trigger_name, &sect, err, errlen) != 0)
                rc = -1;
        }
    }

    sect_free(&sect);
    free(line);
    fclose(f);
    return rc;
}
