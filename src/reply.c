/* reply.c - see reply.h */
#include "reply.h"

#include <string.h>

/* The `text` shaper is exactly the existing pi NDJSON parsing: take the LAST
 * assistant message_end, keep only type=="text" blocks, ignore thinking, and
 * surface a pi error stop reason as a failure. */
static PiStatus text_shape(const char *raw, size_t len, int exit_status,
                           DynBuf *out, char **err)
{
    return pi_extract_text(raw, len, exit_status, out, err);
}

static const ReplyShaper g_shapers[] = {
    { "text", text_shape },
};

const ReplyShaper *reply_shaper_lookup(const char *name)
{
    if (!name)
        return NULL;
    for (size_t i = 0; i < sizeof(g_shapers) / sizeof(g_shapers[0]); i++) {
        if (strcmp(g_shapers[i].name, name) == 0)
            return &g_shapers[i];
    }
    return NULL;
}
