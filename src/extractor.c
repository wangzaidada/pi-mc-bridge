/* extractor.c - input-side extractors: log line -> {player, text}. */
#include "extractor.h"
#include "parser.h"
#include "util.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ---------- chat ---------- */

/* Wrap the existing chat parser unchanged: for each keyword, let
 * parse_chat_line() decide whether the line is chat and the keyword is the
 * first token after the speaker. Ownership of player/prompt moves to `out`. */
static bool chat_extract(const Trigger *t, const char *line, TriggerEvent *out)
{
    for (int i = 0; i < t->keyword_count; i++) {
        ParsedRequest req;
        if (!parse_chat_line(line, t->keywords[i], &req))
            continue;
        out->player = req.player;
        out->text = req.prompt;
        return true;
    }
    return false;
}

/* ---------- keyword ---------- */

/* Return the single token immediately preceding `hit` on `line`, validated as
 * a player name; NULL when there is no valid token. Token chars are the same
 * set the player-name rule allows. */
static char *name_before(const char *line, const char *hit)
{
    const char *p = hit;
    while (p > line && (p[-1] == ' ' || p[-1] == '\t'))
        p--;
    const char *end = p;
    while (p > line && (isalnum((unsigned char)p[-1]) || p[-1] == '_'))
        p--;

    size_t n = (size_t)(end - p);
    if (n == 0 || n > 16)
        return NULL;
    char tmp[17];
    memcpy(tmp, p, n);
    tmp[n] = '\0';
    if (!is_valid_player_name(tmp))
        return NULL;
    return xstrdup(tmp);
}

/* Match the keyword as a substring; the player is the token just before it.
 * No valid name is not a failure: the event is still produced with player
 * "none" (design D2 / text-extraction spec). */
static bool keyword_extract(const Trigger *t, const char *line, TriggerEvent *out)
{
    for (int i = 0; i < t->keyword_count; i++) {
        const char *hit = strstr(line, t->keywords[i]);
        if (!hit)
            continue;
        char *name = name_before(line, hit);
        out->player = name ? name : xstrdup("none");
        out->text = xstrdup("");
        return true;
    }
    return false;
}

/* ---------- registry ---------- */

static const Extractor g_extractors[] = {
    { "chat",    chat_extract    },
    { "keyword", keyword_extract },
};

const Extractor *extractor_lookup(const char *name)
{
    if (!name)
        return NULL;
    for (size_t i = 0; i < sizeof(g_extractors) / sizeof(g_extractors[0]); i++) {
        if (strcmp(g_extractors[i].name, name) == 0)
            return &g_extractors[i];
    }
    return NULL;
}
