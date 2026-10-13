/* main.c - wire everything together: config -> queue -> worker -> watcher. */
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "logwatcher.h"
#include "parser.h"
#include "pi_client.h"
#include "queue.h"
#include "rcon_client.h"
#include "util.h"
#include "worker.h"

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* Sleep `ms` milliseconds in short slices so signals are noticed promptly. */
static void interruptible_sleep(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (!g_stop && nanosleep(&ts, &ts) != 0)
        ; /* EINTR: ts holds the remaining time */
}

/* ---------- log line -> queue ---------- */

typedef struct {
    const Config *cfg;
    TaskQueue    *queue;
    RconClient   *rcon;
} BridgeCtx;

static void notify(Worker *w_unused, RconClient *rcon, const char *player,
                   const char *text)
{
    (void)w_unused;
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

static void on_log_line(const char *line, void *user)
{
    BridgeCtx *ctx = user;
    ParsedRequest req;

    if (!parse_chat_line(line, ctx->cfg->prefix, &req))
        return;

    LOG_INFOF("request from %s: %.120s", req.player, req.prompt);

    if (!config_player_allowed(ctx->cfg, req.player)) {
        LOG_INFOF("ignoring %s (not on the allow-list)", req.player);
        parsed_request_free(&req);
        return;
    }

    QueueStatus st = queue_push(ctx->queue, req.player, req.prompt);
    if (st == QUEUE_FULL) {
        LOG_WARNF("queue full, rejecting request from %s", req.player);
        notify(NULL, ctx->rcon, req.player, "当前请求过多，请稍后再试。");
    } else if (st == QUEUE_SHUTDOWN) {
        LOG_INFOF("queue shutting down, dropping request from %s", req.player);
    }
    parsed_request_free(&req);
}

/* ---------- main ---------- */

int main(int argc, char **argv)
{
    Config cfg;
    int rc = config_load(&cfg, argc, argv);
    if (rc == -2)
        return 0;   /* --help */
    if (rc != 0)
        return 2;   /* usage/validation error */

    log_set_level(cfg.log_level);
    LOG_INFOF("pi-mc-bridge starting (prefix '%s', log %s)", cfg.prefix,
              cfg.log_file);
    LOG_DEBUGF("pi: %s%s%s | rcon %s:%d | cooldown %d ms | max-len %d | queue %d",
               cfg.pi_cmd, cfg.pi_model ? " --model " : "",
               cfg.pi_model ? cfg.pi_model : "", cfg.rcon_host, cfg.rcon_port,
               cfg.cooldown_ms, cfg.max_len, cfg.queue_max);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    TaskQueue queue;
    queue_init(&queue, cfg.queue_max);

    PiClient pi;
    pi_client_init(&pi, cfg.pi_cmd, cfg.pi_model, cfg.pi_timeout_ms);

    RconClient rcon;
    int rrc = rcon_connect(&rcon, cfg.rcon_host, cfg.rcon_port, cfg.rcon_pass,
                           cfg.pi_timeout_ms < 5000 ? 5000 : 5000);
    if (rrc != RCON_ERR_NONE)
        LOG_WARNF("initial RCON connect failed (%d); will retry on demand", rrc);

    Worker worker;
    worker_init(&worker, &queue, &pi, &rcon, &cfg);
    if (worker_start(&worker) != 0) {
        LOG_ERRORF("failed to start worker thread");
        goto cleanup_no_worker;
    }

    BridgeCtx ctx = { .cfg = &cfg, .queue = &queue, .rcon = &rcon };
    LogWatcher watcher;
    logwatcher_init(&watcher, cfg.log_file, 0, on_log_line, &ctx);

    LOG_INFOF("bridge is running; Ctrl-C to stop");

    while (!g_stop) {
        logwatcher_poll(&watcher);
        interruptible_sleep(cfg.poll_ms);
    }

    LOG_INFOF("shutting down...");
    logwatcher_free(&watcher);
    queue_shutdown(&queue);   /* wakes the worker */
    worker_join(&worker);
    worker_free(&worker);

cleanup_no_worker:
    rcon_free(&rcon);
    pi_client_free(&pi);
    queue_free(&queue);
    config_free(&cfg);
    LOG_INFOF("stopped");
    return 0;
}
