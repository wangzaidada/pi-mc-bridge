/* executor.c - see executor.h. Ships the two executors of this change:
 * `replier` (template reply, never invokes pi) and `dialog` (fixed-format pi
 * input, reply shaper, prefixed reply). No `command` executor here (D9). */
#include "executor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "reply.h"

/* ---------- replier ---------- */

static const char *const replier_keys[] = { "template", "executor", NULL };

typedef struct {
    char *template;
} ReplierState;

static int replier_init(void **state, const Trigger *t, char *err, size_t errlen)
{
    if (!t->template || !*t->template) {
        snprintf(err, errlen,
                 "trigger '%s' uses executor 'replier' but sets no template",
                 t->name);
        return -1;
    }
    ReplierState *s = calloc(1, sizeof(*s));
    if (!s) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    s->template = xstrdup(t->template);
    *state = s;
    return 0;
}

static void replier_run(void *state, const TriggerEvent *ev, WorkerCtx *ctx)
{
    ReplierState *s = state;
    KV vars[] = {
        { "player", ev->player },
        { "text", ev->text },
        { "trigger", ev->trigger ? ev->trigger->name : "" },
    };

    /* The template only ever composes the message sent to the player; it is
     * never forwarded to pi (see the executor-registry spec). */
    DynBuf msg;
    dynbuf_init(&msg);
    dynbuf_append_template(&msg, s->template, vars,
                           (int)(sizeof(vars) / sizeof(vars[0])));

    LOG_INFOF("replier '%s' -> %s", ev->trigger ? ev->trigger->name : "?",
              ev->player);
    ctx->send_segmented(ctx->user, ev->player, msg.buf ? msg.buf : "",
                        ev->trigger ? ev->trigger->max_len : 0);
    dynbuf_free(&msg);
}

static void replier_free(void *state)
{
    ReplierState *s = state;
    if (!s)
        return;
    free(s->template);
    free(s);
}

/* ---------- dialog ---------- */

static const char *const dialog_keys[] = {
    "executor", "model", "reply_prefix", "reply_shaper", "max_len", "timeout",
    NULL
};

typedef struct {
    const ReplyShaper *shaper;
    char *reply_prefix;   /* resolved: trigger override or global */
    int   max_len;        /* resolved: trigger override or global */
    int   timeout_ms;     /* resolved: trigger override or global */
} DialogState;

static int dialog_init(void **state, const Trigger *t, char *err, size_t errlen)
{
    /* The shaper is a build-time constant: either the configured one (default
     * `text`) or, for a synthesized trigger, whatever `t->reply_shaper` holds. */
    const char *shaper_name = (t->reply_shaper && *t->reply_shaper)
                                  ? t->reply_shaper : "text";
    const ReplyShaper *shaper = reply_shaper_lookup(shaper_name);
    if (!shaper) {
        snprintf(err, errlen,
                 "trigger '%s' references unknown reply shaper '%s'", t->name,
                 shaper_name);
        return -1;
    }
    DialogState *s = calloc(1, sizeof(*s));
    if (!s) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    s->shaper = shaper;
    s->reply_prefix = xstrdup(t->reply_prefix);
    s->max_len = t->max_len;
    s->timeout_ms = t->timeout_ms;
    *state = s;
    return 0;
}

/* The dialog executor's pi input line is a literal, code-defined format: the
 * bridge extracts data and transports it — it does not author prompts (D3). */
static void dialog_build_input(DynBuf *out, const char *player, const char *text)
{
    dynbuf_reset(out);
    dynbuf_append_str(out, player);
    dynbuf_append_str(out, ": ");
    dynbuf_append_str(out, text);
}

static void dialog_run(void *state, const TriggerEvent *ev, WorkerCtx *ctx)
{
    DialogState *s = state;
    const int max_len = s->max_len > 0 ? s->max_len : ctx->cfg->max_len;
    const char *prefix = s->reply_prefix ? s->reply_prefix
                                         : ctx->cfg->reply_prefix;

    DynBuf input;
    dynbuf_init(&input);
    dialog_build_input(&input, ev->player, ev->text ? ev->text : "");

    LOG_INFOF("asking pi for %s (trigger '%s')", ev->player,
              ev->trigger ? ev->trigger->name : "?");

    DynBuf raw;
    dynbuf_init(&raw);
    char *detail = NULL;
    int status = -1;
    PiStatus st = ctx->run_pi(ctx->pi, input.buf ? input.buf : "", &raw,
                              &detail, &status);

    if (st == PI_OK) {
        DynBuf answer;
        dynbuf_init(&answer);
        char *sdetail = NULL;
        st = s->shaper->shape(raw.buf ? raw.buf : "", raw.len, status, &answer,
                              &sdetail);
        if (st == PI_OK) {
            DynBuf full;
            dynbuf_init(&full);
            if (prefix)
                dynbuf_append_str(&full, prefix);
            dynbuf_append_str(&full, answer.buf ? answer.buf : "");
            ctx->send_segmented(ctx->user, ev->player,
                                full.buf ? full.buf : "", max_len);
            LOG_INFOF("replied to %s (%zu chars)", ev->player, full.len);
            dynbuf_free(&full);
        } else {
            free(detail);
            detail = sdetail;
        }
        dynbuf_free(&answer);
    }

    /* Failure notices mirror the pre-change worker branches exactly. */
    switch (st) {
    case PI_OK:
        break;
    case PI_ERR_TIMEOUT:
        ctx->send(ctx->user, ev->player, "抱歉，生成回复超时了，请稍后再试。");
        LOG_WARNF("pi timed out for %s", ev->player);
        break;
    case PI_ERR_SPAWN:
        ctx->send(ctx->user, ev->player, "抱歉，AI 服务暂时不可用。");
        LOG_ERRORF("pi spawn failed for %s: %s", ev->player,
                   detail ? detail : "unknown");
        break;
    case PI_ERR_EXIT:
        ctx->send(ctx->user, ev->player, "抱歉，AI 服务返回了错误。");
        LOG_ERRORF("pi exited non-zero for %s: %s", ev->player,
                   detail ? detail : "unknown");
        break;
    case PI_ERR_NO_TEXT:
        ctx->send(ctx->user, ev->player, "抱歉，AI 没有返回内容。");
        LOG_WARNF("pi returned no text for %s: %s", ev->player,
                  detail ? detail : "");
        break;
    case PI_ERR_STOP:
        ctx->send(ctx->user, ev->player, "抱歉，AI 生成失败，请稍后再试。");
        LOG_ERRORF("pi reported an error for %s: %s", ev->player,
                   detail ? detail : "unknown");
        break;
    }

    free(detail);
    dynbuf_free(&raw);
    dynbuf_free(&input);
}

static void dialog_free(void *state)
{
    DialogState *s = state;
    if (!s)
        return;
    free(s->reply_prefix);
    free(s);
}

/* ---------- registry ---------- */

static const ExecutorOps g_executors[] = {
    { "replier", replier_keys, replier_init, replier_run, replier_free },
    { "dialog",  dialog_keys,  dialog_init,  dialog_run,  dialog_free  },
};

const ExecutorOps *executor_lookup(const char *name)
{
    if (!name)
        return NULL;
    for (size_t i = 0; i < sizeof(g_executors) / sizeof(g_executors[0]); i++) {
        if (strcmp(g_executors[i].name, name) == 0)
            return &g_executors[i];
    }
    return NULL;
}

bool executor_accepts_key(const ExecutorOps *ops, const char *key)
{
    if (!ops || !ops->keys || !key)
        return false;
    for (const char *const *k = ops->keys; *k; k++) {
        if (strcmp(*k, key) == 0)
            return true;
    }
    return false;
}

/* ---------- build-time binding ---------- */

int trigger_bind_executors(TriggerTable *t, char *err, size_t errlen)
{
    if (!t)
        return 0;
    for (int i = 0; i < t->count; i++) {
        Trigger *tr = &t->items[i];
        const char *name = (tr->executor && *tr->executor) ? tr->executor
                                                           : "dialog";
        const ExecutorOps *ops = executor_lookup(name);
        if (!ops) {
            snprintf(err, errlen,
                     "trigger '%s' references unknown executor '%s'", tr->name,
                     name);
            return -1;
        }
        void *state = NULL;
        if (ops->init(&state, tr, err, errlen) != 0)
            return -1;
        tr->exec = ops;
        tr->exec_state = state;
    }
    return 0;
}

void trigger_unbind_executors(TriggerTable *t)
{
    if (!t)
        return;
    for (int i = 0; i < t->count; i++) {
        Trigger *tr = &t->items[i];
        if (tr->exec && tr->exec->free)
            tr->exec->free(tr->exec_state);
        tr->exec = NULL;
        tr->exec_state = NULL;
    }
}
