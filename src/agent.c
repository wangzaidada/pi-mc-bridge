/* agent.c - see agent.h. Thin orchestrator (design D8): it owns the file
 * behavior, not the delivery mechanics, which the executors own. */
#include "agent.h"

#include <string.h>

#include "trigger.h"
#include "util.h"
#include "worker.h"

void agent_init(Agent *a, const Config *cfg, TaskQueue *queue, RconClient *rcon)
{
    a->cfg = cfg;
    a->queue = queue;
    a->rcon = rcon;
}

bool agent_player_allowed(const Agent *a, const struct Trigger *trigger,
                          const char *player)
{
    /* A trigger with its own allow-list overrides the global one (trigger-
     * matching spec); otherwise fall back to the global list. */
    if (trigger && trigger->allow_count > 0)
        return trigger_allow_contains(trigger, player);
    return config_player_allowed(a->cfg, player);
}

/* Best-effort one-line notice to a player (queue-full "busy" reply). */
static void notify(RconClient *rcon, const char *player, const char *text)
{
    DynBuf cmd, resp;
    dynbuf_init(&cmd);
    dynbuf_init(&resp);
    if (worker_build_tellraw(&cmd, player, text)) {
        int rc = rcon_command(rcon, cmd.buf, &resp);
        if (rc != RCON_ERR_NONE)
            LOG_WARNF("could not notify %s (rcon error %d)", player, rc);
    }
    dynbuf_free(&cmd);
    dynbuf_free(&resp);
}

void agent_on_log_line(const char *line, void *user)
{
    Agent *a = user;
    TriggerEvent ev;

    if (!trigger_match(&a->cfg->triggers, line, &ev))
        return;

    LOG_DEBUGF("line matched trigger '%s' (player %s)", ev.trigger->name,
               ev.player);

    if (!agent_player_allowed(a, ev.trigger, ev.player)) {
        LOG_INFOF("ignoring %s (not allowed for trigger '%s')", ev.player,
                  ev.trigger->name);
        trigger_event_free(&ev);
        return;
    }

    LOG_INFOF("request from %s (trigger '%s'): %.120s", ev.player,
              ev.trigger->name, ev.text);

    QueueStatus st = queue_push(a->queue, ev.trigger, ev.player, ev.text);
    if (st == QUEUE_FULL) {
        LOG_WARNF("queue full, rejecting request from %s", ev.player);
        notify(a->rcon, ev.player, "当前请求过多，请稍后再试。");
    } else if (st == QUEUE_SHUTDOWN) {
        LOG_INFOF("queue shutting down, dropping request from %s", ev.player);
    }
    trigger_event_free(&ev);
}
