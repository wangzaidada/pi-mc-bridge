/* pi_client.h - invoke the `pi` CLI and extract its reply text from NDJSON. */
#ifndef PI_MC_BRIDGE_PI_CLIENT_H
#define PI_MC_BRIDGE_PI_CLIENT_H

#include <stdbool.h>
#include "util.h"

typedef enum {
    PI_OK = 0,
    PI_ERR_TIMEOUT,   /* pi did not finish within the deadline */
    PI_ERR_SPAWN,     /* fork/exec/pipe failure */
    PI_ERR_EXIT,      /* pi exited non-zero with no usable reply */
    PI_ERR_NO_TEXT,   /* stream had no assistant text block */
    PI_ERR_STOP       /* assistant message ended with stopReason "error" */
} PiStatus;

typedef struct {
    char *cmd;         /* path/binary of pi, e.g. "pi" */
    char *model;       /* optional "<provider>/<model>", may be NULL */
    int   timeout_ms;
    char *extra_args;  /* reserved; currently unused (NULL) */
} PiClient;

void pi_client_init(PiClient *c, const char *cmd, const char *model,
                    int timeout_ms);
void pi_client_free(PiClient *c);

/* Run pi for `prompt` and put the reply text into `answer` (reset first).
 * Returns PI_OK on success; otherwise a PI_ERR_* code. `err_detail`, when
 * non-NULL and on failure, is set to a heap string with the underlying reason
 * (caller frees) — useful for logging. */
PiStatus pi_ask(PiClient *c, const char *prompt, DynBuf *answer,
                char **err_detail);

/* --- exposed for unit tests --- */

/* Parse a whole NDJSON blob, appending the LAST assistant message_end's text
 * blocks to `answer`. Sets *stop_error / *error_message if that message ended
 * with stopReason "error". Returns true if any assistant message_end was seen. */
bool pi_parse_stream(const char *data, size_t len, DynBuf *answer,
                     bool *stop_error, char **error_message);

#endif /* PI_MC_BRIDGE_PI_CLIENT_H */
