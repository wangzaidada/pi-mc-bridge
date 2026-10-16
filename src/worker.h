/* worker.h - consumes tasks, dispatches to the trigger's executor. */
#ifndef PI_MC_BRIDGE_WORKER_H
#define PI_MC_BRIDGE_WORKER_H

#include <pthread.h>
#include <stdbool.h>
#include "config.h"
#include "executor.h"
#include "pi_client.h"
#include "queue.h"
#include "rcon_client.h"
#include "util.h"

/* Small fixed-capacity open-addressing table for rate limiting. The key is
 * `<trigger-name>\x1f<player>`, so each trigger cools down independently. */
#define RATE_SLOTS 128

typedef struct {
    char              *key;      /* owning: "<trigger>\\x1f<player>" */
    long long          last_ms;
    unsigned long long seq;      /* insertion order, for LRU eviction */
} RateSlot;

typedef struct {
    TaskQueue      *queue;
    PiClient       *pi;
    RconClient     *rcon;
    const Config   *cfg;

    /* Services handed to executors (see executor.h). */
    WorkerCtx       ctx;

    pthread_t       thread;
    bool            started;

    /* rate limiting */
    RateSlot        slots[RATE_SLOTS];
    int             used;
    unsigned long long seq_counter;
    pthread_mutex_t rate_lock;
} Worker;

void worker_init(Worker *w, TaskQueue *q, PiClient *pi, RconClient *rcon,
                 const Config *cfg);
int  worker_start(Worker *w);
void worker_join(Worker *w);
void worker_free(Worker *w);

/* --- exposed for unit tests --- */

/* Returns true if the (trigger, player) pair may act now; on true it records
 * the current time. Uses the trigger's cooldown, or the global one when the
 * trigger does not override it (<0). Returns false inside the cooldown. */
bool worker_rate_allow(Worker *w, const struct Trigger *trigger,
                       const char *player);

/* Build `tellraw <player> {"text":"<escaped>"}` into `out` (reset first).
 * Returns false if the player name is invalid. */
bool worker_build_tellraw(DynBuf *out, const char *player, const char *text);

#endif /* PI_MC_BRIDGE_WORKER_H */
