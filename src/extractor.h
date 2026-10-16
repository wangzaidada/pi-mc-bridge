/* extractor.h - input-side text extraction registry. */
#ifndef PI_MC_BRIDGE_EXTRACTOR_H
#define PI_MC_BRIDGE_EXTRACTOR_H

#include <stdbool.h>
#include "trigger.h"

/* An extractor both matches a line (using the trigger's keywords) and produces
 * the structured {player, text}. Filling `out` transfers ownership of player
 * and text to the caller; `out->trigger` is set by trigger_match, not here. */
typedef struct {
    const char *name;
    bool (*extract)(const Trigger *t, const char *line, TriggerEvent *out);
} Extractor;

/* Resolve an extractor by name; NULL if unknown. */
const Extractor *extractor_lookup(const char *name);

#endif /* PI_MC_BRIDGE_EXTRACTOR_H */
