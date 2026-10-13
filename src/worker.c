/* worker.c - see worker.h */
#include "worker.h"

#include <stdlib.h>
#include <string.h>

/* ---------- rate limiting ---------- */

static unsigned hash_player(const char *s)
{
    unsigned h = 2166136261u;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 16777619u;
    }
    return h;
}

static RateSlot *rate_find(Worker *w, const char *player)
{
    unsigned mask = RATE_SLOTS - 1; /* RATE_SLOTS is a power of two */
    unsigned i = hash_player(player) & mask;
    for (unsigned probe = 0; probe < RATE_SLOTS; probe++) {
        RateSlot *s = &w->slots[(i + probe) & mask];
        if (!s->player)
            return NULL;                    /* empty slot ends the cluster */
        if (strcmp(s->player, player) == 0)
            return s;
    }
    return NULL;
}

/* Evict the least-recently-inserted entry to make room. Caller holds the lock. */
static void rate_evict_one(Worker *w)
{
    int victim = -1;
    for (int i = 0; i < RATE_SLOTS; i++) {
        if (!w->slots[i].player)
            continue;
        if (victim < 0 || w->slots[i].seq < w->slots[victim].seq)
            victim = i;
    }
    if (victim >= 0) {
        free(w->slots[victim].player);
        w->slots[victim].player = NULL;
        w->used--;
    }
}

bool worker_rate_allow(Worker *w, const char *player)
{
    long long now = monotonic_ms();
    bool allowed;

    pthread_mutex_lock(&w->rate_lock);
    RateSlot *s = rate_find(w, player);
    if (s) {
        allowed = (now - s->last_ms) >= w->cfg->cooldown_ms;
        if (allowed) {
            s->last_ms = now;
            s->seq = ++w->seq_counter;
        }
    } else {
        if (w->used >= RATE_SLOTS)
            rate_evict_one(w);
        unsigned mask = RATE_SLOTS - 1;
        unsigned i = hash_player(player) & mask;
        for (unsigned probe = 0; probe < RATE_SLOTS; probe++) {
            RateSlot *slot = &w->slots[(i + probe) & mask];
            if (!slot->player) {
                slot->player = xstrdup(player);
                slot->last_ms = now;
                slot->seq = ++w->seq_counter;
                w->used++;
                break;
            }
        }
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

/* ---------- sending ---------- */

static void send_text(Worker *w, const char *player, const char *text)
{
    DynBuf cmd, resp;
    dynbuf_init(&cmd);
    dynbuf_init(&resp);

    if (!worker_build_tellraw(&cmd, player, text)) {
        LOG_ERRORF("refusing to send to invalid player name '%s'", player);
        goto done;
    }
    int rc = rcon_command(w->rcon, cmd.buf, &resp);
    if (rc != RCON_ERR_NONE)
        LOG_ERRORF("failed to deliver reply to %s (rcon error %d)", player, rc);
    else
        LOG_DEBUGF("rcon <- %.*s", (int)(cmd.len > 200 ? 200 : cmd.len), cmd.buf);

done:
    dynbuf_free(&cmd);
    dynbuf_free(&resp);
}

/* Send `text` in UTF-8-safe chunks of at most cfg->max_len code points. */
static void send_text_segmented(Worker *w, const char *player, const char *text)
{
    size_t max = (size_t)w->cfg->max_len;
    const char *p = text;
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
        send_text(w, player, chunk);
        free(chunk);
        p += n;
        segment++;
    }
    if (segment > 1)
        LOG_INFOF("reply to %s split into %d segment(s)", player, segment);
}

/* ---------- task handling ---------- */

static void handle_prompt(Worker *w, const char *player, const char *prompt)
{
    DynBuf answer;
    dynbuf_init(&answer);
    char *detail = NULL;

    LOG_INFOF("asking pi for %s: %.80s%s", player, prompt,
              strlen(prompt) > 80 ? "..." : "");

    PiStatus st = pi_ask(w->pi, prompt, &answer, &detail);

    switch (st) {
    case PI_OK: {
        DynBuf full;
        dynbuf_init(&full);
        if (w->cfg->reply_prefix)
            dynbuf_append_str(&full, w->cfg->reply_prefix);
        dynbuf_append_str(&full, answer.buf ? answer.buf : "");
        send_text_segmented(w, player, full.buf ? full.buf : "");
        LOG_INFOF("replied to %s (%zu chars)", player, full.len);
        dynbuf_free(&full);
        break;
    }
    case PI_ERR_TIMEOUT:
        send_text(w, player, "抱歉，生成回复超时了，请稍后再试。");
        LOG_WARNF("pi timed out for %s", player);
        break;
    case PI_ERR_SPAWN:
        send_text(w, player, "抱歉，AI 服务暂时不可用。");
        LOG_ERRORF("pi spawn failed for %s: %s", player,
                   detail ? detail : "unknown");
        break;
    case PI_ERR_EXIT:
        send_text(w, player, "抱歉，AI 服务返回了错误。");
        LOG_ERRORF("pi exited non-zero for %s: %s", player,
                   detail ? detail : "unknown");
        break;
    case PI_ERR_NO_TEXT:
        send_text(w, player, "抱歉，AI 没有返回内容。");
        LOG_WARNF("pi returned no text for %s: %s", player,
                  detail ? detail : "");
        break;
    case PI_ERR_STOP:
        send_text(w, player, "抱歉，AI 生成失败，请稍后再试。");
        LOG_ERRORF("pi reported an error for %s: %s", player,
                   detail ? detail : "unknown");
        break;
    }

    free(detail);
    dynbuf_free(&answer);
}

static void handle_task(Worker *w, Task *t)
{
    /* Empty prompt => usage hint, no rate-limit cost. */
    if (!t->prompt || !*t->prompt) {
        DynBuf hint;
        dynbuf_init(&hint);
        dynbuf_printf(&hint, "用法：%s <你的问题>", w->cfg->prefix);
        send_text(w, t->player, hint.buf ? hint.buf : "");
        dynbuf_free(&hint);
        task_free(t);
        return;
    }

    if (!worker_rate_allow(w, t->player)) {
        LOG_INFOF("rate-limited %s (cooldown %d ms)", t->player,
                  w->cfg->cooldown_ms);
        task_free(t);
        return;
    }

    handle_prompt(w, t->player, t->prompt);
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
        free(w->slots[i].player);
    pthread_mutex_destroy(&w->rate_lock);
}
