/* parser.c - see parser.h */
#include "parser.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

bool is_valid_player_name(const char *name)
{
    if (!name)
        return false;
    size_t n = strlen(name);
    if (n == 0 || n > 16)
        return false;
    for (const char *p = name; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '_'))
            return false;
    }
    return true;
}

void parsed_request_free(ParsedRequest *r)
{
    if (!r)
        return;
    free(r->player);
    free(r->prompt);
    r->player = r->prompt = NULL;
}

/* Validate a "<Name>" token at `lt` (points at '<'); on success set *name_out
 * (heap copy) and return the '>' position, else return NULL. */
static const char *try_name_token(const char *lt, char **name_out)
{
    const char *gt = strchr(lt + 1, '>');
    if (!gt)
        return NULL;
    size_t len = (size_t)(gt - (lt + 1));
    if (len < 1 || len > 16)
        return NULL;
    char tmp[17];
    memcpy(tmp, lt + 1, len);
    tmp[len] = '\0';
    if (!is_valid_player_name(tmp))
        return NULL;
    *name_out = xstrdup(tmp);
    return gt;
}

/* Locate the speaker token. Vanilla log lines look like:
 *   [12:34:56] [Server thread/INFO]: <Steve> hello
 * We prefer the "<Name>" that follows the "]" closing the thread/level tag, so
 * a '<' that appears inside the message text cannot be mistaken for the
 * speaker. If that marker is absent (unusual log formats) we fall back to the
 * first "<Name>" token in the line. */
static const char *find_name_token(const char *line, char **name_out)
{
    const char *p = strstr(line, "]:");
    if (p) {
        p += 2;
        for (;;) {
            while (*p == ' ' || *p == '\t')
                p++;
            /* Optional bracketed tags between the prefix and the speaker, e.g.
             * "[Not Secure]". Skip them one at a time. */
            if (*p != '[')
                break;
            const char *close = strchr(p, ']');
            if (!close)
                break;
            p = close + 1;
        }
        if (*p == '<') {
            const char *gt = try_name_token(p, name_out);
            if (gt)
                return p;
        }
    }

    p = line;
    while ((p = strchr(p, '<')) != NULL) {
        if (try_name_token(p, name_out))
            return p;
        p++;
    }
    return NULL;
}

/* Trim ASCII whitespace in place; returns pointer to first non-space char. */
static char *trim_inplace(char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1]))
        s[--n] = '\0';
    return s;
}

bool parse_chat_line(const char *line, const char *prefix, ParsedRequest *out)
{
    if (!line || !prefix || !out)
        return false;
    out->player = NULL;
    out->prompt = NULL;

    char *name = NULL;
    const char *lt = find_name_token(line, &name);
    if (!lt) {
        free(name);
        return false;
    }

    const char *after = strchr(lt + 1, '>'); /* the '>' of this name token */
    if (!after) {
        free(name);
        return false;
    }
    after++;

    /* Skip the single separating space (and any extra spacing typed by hand). */
    while (*after == ' ' || *after == '\t')
        after++;

    /* First token must equal the trigger, case-insensitively. */
    size_t plen = strlen(prefix);
    if (plen == 0 || strncasecmp(after, prefix, plen) != 0) {
        free(name);
        return false;
    }
    /* The trigger must be a whole token: followed by end, space, or tab. */
    char next = after[plen];
    if (next != '\0' && next != ' ' && next != '\t') {
        free(name);
        return false;
    }

    /* Extract the prompt (everything after the trigger) and trim it. */
    char *prompt = xstrdup(after + plen);
    char *trimmed = trim_inplace(prompt);

    out->player = name;
    /* Copy the trimmed prompt into a fresh allocation so ownership is simple. */
    out->prompt = xstrdup(trimmed);
    free(prompt);
    return true;
}
