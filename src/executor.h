/* executor.h - output-side execution registry (replier, dialog).
 *
 * An executor turns one matched TriggerEvent into side effects (usually a
 * tellraw reply over RCON). It is resolved by name from a static registry and
 * bound to its trigger once at build time, so the per-line path never looks a
 * name up. The `replier` executor never invokes pi; the `dialog` executor does.
 */
#ifndef PI_MC_BRIDGE_EXECUTOR_H
#define PI_MC_BRIDGE_EXECUTOR_H

#include <stdbool.h>
#include <stddef.h>

#include "config.h"
#include "pi_client.h"
#include "rcon_client.h"
#include "trigger.h"
#include "util.h"

/* Services the worker lends to an executor. The function pointers keep
 * executor.c free of worker internals and let unit tests substitute fakes for
 * sending and for invoking pi. */
typedef struct WorkerCtx {
    const Config *cfg;
    PiClient     *pi;
    RconClient   *rcon;
    void         *user;   /* the owning Worker (opaque here) */

    /* Send one reply segment to `player`. Returns false if the name is invalid. */
    bool (*send)(void *user, const char *player, const char *text);
    /* Send `text` in UTF-8-safe chunks of at most `max_len` code points
     * (max_len <= 0 => the global Config.max_len). */
    bool (*send_segmented)(void *user, const char *player, const char *text,
                           int max_len);
    /* Run pi for `prompt`, capturing raw stdout (unparsed) into `raw`.
     * Mirrors pi_run(): PI_OK / PI_ERR_TIMEOUT / PI_ERR_SPAWN; `exit_status`
     * receives the waitpid status when the child exited (else -1). */
    PiStatus (*run_pi)(PiClient *pi, const char *prompt, DynBuf *raw,
                       char **detail, int *exit_status);
} WorkerCtx;

typedef struct ExecutorOps {
    const char *name;
    /* Parameter keys this executor accepts, NULL-terminated. Used at build
     * time to reject keys that belong to another executor. */
    const char *const *keys;
    /* Read this executor's parameters from `t` into *state. On failure writes
     * `err` and returns -1. */
    int  (*init)(void **state, const Trigger *t, char *err, size_t errlen);
    void (*run)(void *state, const TriggerEvent *ev, WorkerCtx *ctx);
    void (*free)(void *state);
} ExecutorOps;

/* Resolve an executor by name; NULL if unknown. */
const ExecutorOps *executor_lookup(const char *name);

/* True if `key` is one of the executor's accepted parameter keys. */
bool executor_accepts_key(const ExecutorOps *ops, const char *key);

/* Bind every trigger to its executor: resolve `trigger->executor` (default
 * "dialog") via executor_lookup, call its init to build the instance state, and
 * store `exec` / `exec_state`. This is the build-time step that turns a name
 * into a dispatchable pointer, so the per-line path never looks one up.
 * Returns 0 on success; on failure writes `err` and returns -1. On failure the
 * executors bound before the error are left in place — call
 * trigger_unbind_executors to release them. */
int  trigger_bind_executors(TriggerTable *t, char *err, size_t errlen);

/* Free every bound executor's state and clear the bindings. Idempotent. */
void trigger_unbind_executors(TriggerTable *t);

#endif /* PI_MC_BRIDGE_EXECUTOR_H */
