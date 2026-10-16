/* reply.h - output-side reply shaping registry.
 *
 * A reply shaper turns pi's raw stdout into the text the bridge will speak.
 * This round ships only `text`, which reuses the existing pi NDJSON parsing
 * (D10). The registry exists so a future shaper (e.g. a command list) can be
 * added without touching the executors.
 */
#ifndef PI_MC_BRIDGE_REPLY_H
#define PI_MC_BRIDGE_REPLY_H

#include "pi_client.h"
#include "util.h"

typedef struct ReplyShaper {
    const char *name;
    /* Append the reply text for pi's raw stdout (`raw`, `len`) to `out`
     * (reset first). `exit_status` is the child's waitpid status, or -1, used
     * to tell a crash apart from a clean-but-empty result. Returns PI_OK, or a
     * PI_ERR_* describing why no usable reply was extracted; on failure `err`
     * may be set to a heap detail string (caller frees). */
    PiStatus (*shape)(const char *raw, size_t len, int exit_status, DynBuf *out,
                      char **err);
} ReplyShaper;

/* Resolve a shaper by name; NULL if unknown. */
const ReplyShaper *reply_shaper_lookup(const char *name);

#endif /* PI_MC_BRIDGE_REPLY_H */
