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

/* Override the model on an already-initialised client (NULL/empty clears it).
 * Used to bind a per-trigger `model` without re-creating the client. */
void pi_client_set_model(PiClient *c, const char *model);

/* Override the timeout on an already-initialised client (ms; <=0 keeps the
 * current value). */
void pi_client_set_timeout(PiClient *c, int timeout_ms);

/* Run pi for `prompt` and capture its raw stdout (unparsed NDJSON) into `raw`
 * (reset first). Returns PI_OK when the child ran to completion (whatever its
 * exit status), or the transport failure code (PI_ERR_SPAWN / PI_ERR_TIMEOUT).
 * `err_detail`, when non-NULL and set, is a heap string with the underlying
 * reason (caller frees). When `exit_status` is non-NULL it receives the
 * waitpid status if the child exited, else it is left untouched — callers must
 * initialise it to -1.
 *
 * This does NOT interpret pi's output: the reply shaper (reply.h) does that. */
PiStatus pi_run(PiClient *c, const char *prompt, DynBuf *raw, char **err_detail,
                int *exit_status);

/* Interpret captured pi stdout: append the LAST assistant message_end's text
 * blocks to `answer` (reset first). Returns PI_OK on success, or
 * PI_ERR_NO_TEXT / PI_ERR_STOP. `err_detail`, when non-NULL and set, is a heap
 * string (caller frees). `exit_status` disambiguates an empty stream that came
 * from a crash (PI_ERR_EXIT) from a clean-but-empty one (PI_ERR_NO_TEXT). */
PiStatus pi_extract_text(const char *raw, size_t len, int exit_status,
                         DynBuf *answer, char **err_detail);

/* --- exposed for unit tests --- */

/* Parse a whole NDJSON blob, appending the LAST assistant message_end's text
 * blocks to `answer`. Sets *stop_error / *error_message if that message ended
 * with stopReason "error". Returns true if any assistant message_end was seen. */
bool pi_parse_stream(const char *data, size_t len, DynBuf *answer,
                     bool *stop_error, char **error_message);

/* Build the argv pi is executed with:
 *   <cmd> --mode json --no-session --no-tools [--model M] -p <prompt>
 * The list is a fixed literal (no system-prompt argument) — the bridge carries
 * stored data, it does not author prompts (design D3). Returns a NULL-terminated
 * heap array (caller frees the array, not the borrowed strings). */
char **pi_build_argv(PiClient *c, const char *prompt);

#endif /* PI_MC_BRIDGE_PI_CLIENT_H */
