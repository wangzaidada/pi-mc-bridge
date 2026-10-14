/* worker.c - see worker.h */
#include "worker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "trigger.h"

/* ---------- rate limiting ---------- */

static unsigned hash_key(const char *s)
{
    unsigned h = 2166136261u;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 16777619u;
    }
    return h;
}

/* The rate-limit key is "<trigger-name>\x1f<player>" (unit separator, which
 * cannot appear in a player name or a trigger name), so each trigger cools
 * down independently. A NULL trigger (tests) keys on the player alone. */
static char *rate_key_make(const struct Trigger *trigger, const char *player)
{
    const char *p = player ? player : "";
    if (!trigger || !trigger->name)
        return xstrdup(p);
    size_t n = strlen(trigger->name) + 1 + strlen(p);
    char *k = malloc(n + 1);
    if (!k)
        return NULL;
    snprintf(k, n + 1, "%s\x1f%s", trigger->name, p);
    return k;
}

static RateSlot *rate_find(Worker *w, const char *key)
{
    unsigned mask = RATE_SLOTS - 1; /* RATE_SLOTS is a power of two */
    unsigned i = hash_key(key) & mask;
    for (unsigned probe = 0; probe < RATE_SLOTS; probe++) {
        RateSlot *s = &w->slots[(i + probe) & mask];
        if (!s->key)
            return NULL;                    /* empty slot ends the cluster */
        if (strcmp(s->key, key) == 0)
            return s;
    }
    return NULL;
}

/* Probe for a free slot and install `key` (taking ownership). Caller holds the
 * lock. If the table is somehow full the key is dropped. */
static void rate_insert(Worker *w, char *key, long long last_ms,
                        unsigned long long seq)
{
    unsigned mask = RATE_SLOTS - 1;
    unsigned i = hash_key(key) & mask;
    for (unsigned probe = 0; probe < RATE_SLOTS; probe++) {
        RateSlot *slot = &w->slots[(i + probe) & mask];
        if (!slot->key) {
            slot->key = key;
            slot->last_ms = last_ms;
            slot->seq = seq;
            w->used++;
            return;
        }
    }
    free(key);
}

/* Evict the least-recently-inserted entry to make room. Caller holds the lock. */
static void rate_evict_one(Worker *w)
{
    int victim = -1;
    for (int i = 0; i < RATE_SLOTS; i++) {
        if (!w->slots[i].key)
            continue;
        if (victim < 0 || w->slots[i].seq < w->slots[victim].seq)
            victim = i;
    }
    if (victim < 0)
        return;
    free(w->slots[victim].key);
    w->slots[victim].key = NULL;
    w->used--;

    /* Removing a slot can break the probe chain of entries that follow it: any
     * entry whose probe path crosses the hole becomes unfindable (cooldown
     * bypassed, duplicate rows). Re-insert the contiguous run after the hole,
     * stopping at the first empty slot — that slot terminates every probe path
     * through this region, so nothing past it can be displaced. */
    unsigned mask = RATE_SLOTS - 1;
    unsigned i = ((unsigned)victim + 1) & mask;
    while (w->slots[i].key) {
        RateSlot tmp = w->slots[i];
        w->slots[i].key = NULL;
        w->used--;
        rate_insert(w, tmp.key, tmp.last_ms, tmp.seq);
        i = (i + 1) & mask;
    }
}

bool worker_rate_allow(Worker *w, const struct Trigger *trigger,
                       const char *player)
{
    int cooldown = (trigger && trigger->cooldown_ms >= 0)
                       ? trigger->cooldown_ms
                       : w->cfg->cooldown_ms;

    char *key = rate_key_make(trigger, player);
    if (!key)
        return true; /* out of memory: fail open rather than stall the bridge */

    long long now = monotonic_ms();
    bool allowed;

    pthread_mutex_lock(&w->rate_lock);
    RateSlot *s = rate_find(w, key);
    if (s) {
        allowed = (now - s->last_ms) >= cooldown;
        if (allowed) {
            s->last_ms = now;
            s->seq = ++w->seq_counter;
        }
        free(key);
    } else {
        if (w->used >= RATE_SLOTS)
            rate_evict_one(w);
        rate_insert(w, key, now, ++w->seq_counter); /* takes ownership */
        allowed = true;
    }
    pthread_mutex_unlock(&w->rate_lock);
    return allowed;
}

/* ---------- tellraw ---------- */

bool worker_build_tellraw(DynBuf *out, const char *player, const char *text)
{
    if (!rcon_valid_player(player))
        return false;
    dynbuf_reset(out);
    dynbuf_append_str(out, "tellraw ");
    dynbuf_append_str(out, player);
    dynbuf_append_str(out, " {\"text\":\"");
    json_escape_append(out, text ? text : "");
    dynbuf_append_str(out, "\"}");
    return true;
}

/* ---------- sending (WorkerCtx callbacks) ---------- */

static bool worker_send(void *user, const char *player, const char *text)
{
    Worker *w = user;
    DynBuf cmd, resp;
    dynbuf_init(&cmd);
    dynbuf_init(&resp);

    bool ok = worker_build_tellraw(&cmd, player, text);
    if (!ok) {
        LOG_ERRORF("refusing to send to invalid player name '%s'", player);
    } else {
        int rc = rcon_command(w->rcon, cmd.buf, &resp);
        if (rc != RCON_ERR_NONE)
            LOG_ERRORF("failed to deliver reply to %s (rcon error %d)", player,
                       rc);
        else
            LOG_DEBUGF("rcon <- %.*s", (int)(cmd.len > 200 ? 200 : cmd.len),
                       cmd.buf);
    }

    dynbuf_free(&cmd);
    dynbuf_free(&resp);
    return ok;
}

/* Send `text` in UTF-8-safe chunks of at most `max_len` code points
 * (max_len <= 0 => the global Config.max_len). */
static bool worker_send_segmented(void *user, const char *player,
                                  const char *text, int max_len)
{
    Worker *w = user;
    int lim = max_len > 0 ? max_len : w->cfg->max_len;
    size_t max = (size_t)lim;
    const char *p = text ? text : "";
    int segment = 0;
    while (*p) {
        size_t n = utf8_segment_len(p, max);
        if (n == 0)
            break;
        char *chunk = malloc(n + 1);
        if (!chunk)
            break;
        memcpy(chunk, p, n);
        chunk[n] = '\0';
        worker_send(w, player, chunk);
        free(chunk);
        p += n;
        segment++;
    }
    if (segment > 1)
        LOG_INFOF("reply to %s split into %d segment(s)", player, segment);
    return true;
}

/* ---------- task handling ---------- */

static void handle_task(Worker *w, Task *t)
{
    const Trigger *tr = t->trigger;
    if (!tr || !tr->exec) {
        LOG_WARNF("dropping a task with no bound executor");
        task_free(t);
        return;
    }

    /* A chat trigger used bare (e.g. "@pi" with no message) earns a usage hint
     * and costs no cooldown, exactly as before the change. The hint names the
     * trigger's keyword so multi-keyword configs read naturally. */
    if (tr->match == MATCH_CHAT && (!t->prompt || !*t->prompt)) {
        const char *kw = tr->keyword_count > 0 ? tr->keywords[0] : "";
        DynBuf hint;
        dynbuf_init(&hint);
        dynbuf_printf(&hint, "用法：%s <你的问题>", kw);
        w->ctx.send(w, t->player, hint.buf ? hint.buf : "");
        dynbuf_free(&hint);
        task_free(t);
        return;
    }

    if (!worker_rate_allow(w, tr, t->player)) {
        LOG_INFOF("rate-limited %s (trigger '%s', cooldown %d ms)", t->player,
                  tr->name,
                  tr->cooldown_ms >= 0 ? tr->cooldown_ms : w->cfg->cooldown_ms);
        task_free(t);
        return;
    }

    /* The event borrows the task's strings; the executor only reads them. */
    TriggerEvent ev = { .trigger = tr, .player = t->player, .text = t->prompt };
    tr->exec->run(tr->exec_state, &ev, &w->ctx);
    task_free(t);
}

static void *worker_main(void *arg)
{
    Worker *w = arg;
    for (;;) {
        Task t;
        QueueStatus st = queue_pop(w->queue, &t);
        if (st == QUEUE_SHUTDOWN)
            break;
        handle_task(w, &t);
    }
    LOG_INFOF("worker thread exiting");
    return NULL;
}

/* ---------- lifecycle ---------- */

void worker_init(Worker *w, TaskQueue *q, PiClient *pi, RconClient *rcon,
                 const Config *cfg)
{
    memset(w, 0, sizeof(*w));
    w->queue = q;
    w->pi = pi;
    w->rcon = rcon;
    w->cfg = cfg;
    w->started = false;
    pthread_mutex_init(&w->rate_lock, NULL);

    w->ctx.cfg = cfg;
    w->ctx.pi = pi;
    w->ctx.rcon = rcon;
    w->ctx.user = w;
    w->ctx.send = worker_send;
    w->ctx.send_segmented = worker_send_segmented;
    w->ctx.run_pi = pi_run;
}

int worker_start(Worker *w)
{
    if (pthread_create(&w->thread, NULL, worker_main, w) != 0)
        return -1;
    w->started = true;
    return 0;
}

void worker_join(Worker *w)
{
    if (w->started) {
        pthread_join(w->thread, NULL);
        w->started = false;
    }
}

void worker_free(Worker *w)
{
    if (!w)
        return;
    for (int i = 0; i < RATE_SLOTS; i++)
        free(w->slots[i].key);
    pthread_mutex_destroy(&w->rate_lock);
}
