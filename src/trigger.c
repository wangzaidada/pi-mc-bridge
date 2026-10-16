/* trigger.c - see trigger.h */
#include "trigger.h"
#include "extractor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

Trigger *trigger_table_append(TriggerTable *t)
{
    Trigger *items = realloc(t->items, sizeof(Trigger) * (size_t)(t->count + 1));
    if (!items)
        return NULL;
    t->items = items;
    Trigger *tr = &t->items[t->count];
    memset(tr, 0, sizeof(*tr));
    tr->cooldown_ms = -1;   /* inherit global by default */
    t->count++;
    return tr;
}

/* Map a match kind to the only extractor that implements it. */
static const char *extractor_for(MatchKind m)
{
    return m == MATCH_CHAT ? "chat" : "keyword";
}

int trigger_validate(const TriggerTable *t, char *err, size_t errlen)
{
    for (int i = 0; i < t->count; i++) {
        const Trigger *tr = &t->items[i];

        if (!tr->name || !*tr->name) {
            snprintf(err, errlen, "trigger #%d has no name", i + 1);
            return -1;
        }
        if (tr->keyword_count < 1) {
            snprintf(err, errlen, "trigger '%s' has no keywords", tr->name);
            return -1;
        }
        if (!tr->extractor || !extractor_lookup(tr->extractor)) {
            snprintf(err, errlen,
                     "trigger '%s' references unknown extractor '%s'",
                     tr->name, tr->extractor ? tr->extractor : "(none)");
            return -1;
        }
        /* The match kind and the extractor must agree: the extractor is what
         * actually scans the line, so a mismatch would silently misbehave. */
        if (strcmp(tr->extractor, extractor_for(tr->match)) != 0) {
            snprintf(err, errlen,
                     "trigger '%s' has match kind %s but extractor '%s'",
                     tr->name,
                     tr->match == MATCH_CHAT ? "chat" : "keyword",
                     tr->extractor);
            return -1;
        }
        /* Names must be unique: they key rate limiting and the debug logs. */
        for (int j = 0; j < i; j++) {
            if (strcmp(t->items[j].name, tr->name) == 0) {
                snprintf(err, errlen, "duplicate trigger name '%s'", tr->name);
                return -1;
            }
        }
    }
    return 0;
}

bool trigger_match(const TriggerTable *t, const char *line, TriggerEvent *out)
{
    if (!t || !line || !out)
        return false;

    for (int i = 0; i < t->count; i++) {
        const Trigger *tr = &t->items[i];
        const Extractor *ex = extractor_lookup(tr->extractor);
        if (!ex)
            continue; /* unknown extractor is rejected at load */

        TriggerEvent ev = { .trigger = tr, .player = NULL, .text = NULL };
        if (ex->extract(tr, line, &ev)) {
            *out = ev;
            return true; /* first match wins */
        }
    }
    return false;
}

bool trigger_allow_contains(const Trigger *t, const char *player)
{
    if (!player)
        return false;
    for (int i = 0; i < t->allow_count; i++) {
        if (strcasecmp(t->allow[i], player) == 0)
            return true;
    }
    return false;
}

void trigger_event_free(TriggerEvent *e)
{
    if (!e)
        return;
    free(e->player);
    free(e->text);
    e->player = e->text = NULL;
    e->trigger = NULL;
}

void trigger_table_free(TriggerTable *t)
{
    if (!t)
        return;
    for (int i = 0; i < t->count; i++) {
        Trigger *tr = &t->items[i];
        free(tr->name);
        for (int k = 0; k < tr->keyword_count; k++)
            free(tr->keywords[k]);
        free(tr->keywords);
        free(tr->extractor);
        free(tr->executor);
        free(tr->template);
        free(tr->reply_shaper);
        free(tr->model);
        free(tr->reply_prefix);
        for (int k = 0; k < tr->allow_count; k++)
            free(tr->allow[k]);
        free(tr->allow);
        /* exec_state is owned by the executor; the assembly frees it. */
    }
    free(t->items);
    t->items = NULL;
    t->count = 0;
}
