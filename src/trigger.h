/* trigger.h - trigger table: match a log line against configured triggers. */
#ifndef PI_MC_BRIDGE_TRIGGER_H
#define PI_MC_BRIDGE_TRIGGER_H

#include <stdbool.h>
#include <stddef.h>

/* The executor registry lives in executor.h; triggers hold a resolved pointer
 * to their executor ops, bound at build time (see D2/D8 in the design). */
struct ExecutorOps;

/* Two built-in match kinds, no regex engine (see trigger-matching spec). */
typedef enum {
    MATCH_CHAT = 0,    /* chat line: keyword is the first token after the speaker */
    MATCH_KEYWORD = 1  /* literal keyword: matched as a substring anywhere */
} MatchKind;

typedef struct Trigger {
    char       *name;          /* unique, from the config section name */
    MatchKind   match;
    char      **keywords;      /* owning; non-empty */
    int         keyword_count;
    char       *extractor;     /* extractor registry name ("chat"/"keyword") */
    char       *executor;      /* executor registry name ("dialog"/"replier") */
    const struct ExecutorOps *exec;  /* resolved at build time (NULL until bound) */
    void       *exec_state;    /* executor instance data */
    int         cooldown_ms;   /* <0 => inherit the global cooldown */
    char      **allow;         /* NULL/0 => inherit the global allow-list */
    int         allow_count;

    /* Executor parameters read at build time (see executor.h). A NULL string or
     * a non-positive number means "fall back to the merged global value". */
    char       *template;      /* replier: reply template with {placeholders} */
    char       *reply_shaper;  /* dialog: shaper name ("text") */
    char       *model;         /* dialog: pi model override */
    char       *reply_prefix;  /* dialog: reply prefix override */
    int         max_len;       /* dialog: reply segment length override */
    int         timeout_ms;    /* dialog: pi timeout override */
} Trigger;

typedef struct {
    const Trigger *trigger;    /* points into the owning table */
    char          *player;     /* owning copy */
    char          *text;       /* owning copy; may be "" */
} TriggerEvent;

typedef struct {
    Trigger *items;
    int      count;
} TriggerTable;

/* Offer `line` to the triggers in table order; the FIRST match wins and fills
 * *out (player/text heap-allocated, out->trigger points into the table).
 * Returns false when no trigger matches. */
bool trigger_match(const TriggerTable *t, const char *line, TriggerEvent *out);

/* Check the table for problems that must abort startup: a missing name, an
 * empty keyword set, an unknown extractor, a duplicate name, or a match kind
 * that is incompatible with the extractor. On failure writes a message into
 * `err` and returns -1. */
int trigger_validate(const TriggerTable *t, char *err, size_t errlen);

/* Case-insensitive membership test against the trigger's own allow-list.
 * Callers that want the inherit-global behaviour must check allow_count first. */
bool trigger_allow_contains(const Trigger *t, const char *player);

/* Append a zeroed trigger and return it (table owns the returned storage).
 * Returns NULL on allocation failure. */
Trigger *trigger_table_append(TriggerTable *t);

void trigger_event_free(TriggerEvent *e);
void trigger_table_free(TriggerTable *t);

#endif /* PI_MC_BRIDGE_TRIGGER_H */
