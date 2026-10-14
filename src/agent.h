/* agent.h - top-level orchestrator: log line -> trigger -> task. */
#ifndef PI_MC_BRIDGE_AGENT_H
#define PI_MC_BRIDGE_AGENT_H

#include "config.h"
#include "queue.h"
#include "rcon_client.h"

/* The orchestration context. Deliberately thin (design D8): the agent matches
 * a line against the trigger table, applies the effective allow-list, and
 * enqueues a task. Which executor runs is decided later, by the worker. */
typedef struct {
    const Config *cfg;
    TaskQueue    *queue;
    RconClient   *rcon;
} Agent;

void agent_init(Agent *a, const Config *cfg, TaskQueue *queue, RconClient *rcon);

/* logwatcher callback: offer one complete log line to the trigger table. */
void agent_on_log_line(const char *line, void *user);

/* --- exposed for unit tests --- */

/* Effective allow-list check: a trigger with its own list uses that, otherwise
 * the global one. A trigger with no list and no global list allows everyone. */
bool agent_player_allowed(const Agent *a, const struct Trigger *trigger,
                          const char *player);

#endif /* PI_MC_BRIDGE_AGENT_H */
