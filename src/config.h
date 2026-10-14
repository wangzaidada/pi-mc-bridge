/* config.h - runtime configuration (CLI > env > default). */
#ifndef PI_MC_BRIDGE_CONFIG_H
#define PI_MC_BRIDGE_CONFIG_H

#include <stdbool.h>
#include <stdio.h>
#include "trigger.h"
#include "util.h"

typedef struct Config {
    /* RCON */
    char *rcon_host;      /* default 127.0.0.1        (RCON_HOST) */
    int   rcon_port;      /* default 25575            (RCON_PORT) */
    char *rcon_pass;      /* REQUIRED                 (RCON_PASSWORD) */

    /* log */
    char *log_file;       /* default logs/latest.log  (LOG_FILE) */

    /* chat trigger */
    char *prefix;         /* default "@pi"            (CHAT_PREFIX) */
    char *reply_prefix;   /* default "[AI] "          (REPLY_PREFIX) */

    /* pi */
    char *pi_cmd;         /* default "pi"             (PI_CMD) */
    char *pi_model;       /* default NULL             (PI_MODEL) */
    int   pi_timeout_ms;  /* default 60000            (PI_TIMEOUT, seconds) */

    /* behaviour */
    int   cooldown_ms;    /* default 10000            (COOLDOWN, seconds) */
    int   max_len;        /* default 200 chars        (MAX_LEN) */
    int   poll_ms;        /* default 500              (POLL_MS) */
    int   queue_max;      /* default 64               (QUEUE_MAX) */
    char *allow;          /* default NULL (all)       (ALLOW_PLAYERS, CSV) */
    LogLevel log_level;   /* default INFO             (LOG_LEVEL) */

    /* configuration file; NULL => none (synthesize the default trigger) */
    char *config_file;

    /* Scratch list of allowed names, built from `allow`. */
    char **allow_list;
    int    allow_count;

    /* Trigger table. Without a configuration file this holds exactly one
     * synthesized compatibility trigger (see config_load). */
    TriggerTable triggers;
} Config;

/* Returns 0 on success, -1 on error (message already printed), -2 if --help
 * was requested (usage printed). */
int  config_load(Config *cfg, int argc, char **argv);

void config_free(Config *cfg);

void config_print_usage(FILE *out);

/* True if `player` may use the bridge (always true when no list is set). */
bool config_player_allowed(const Config *cfg, const char *player);

#endif /* PI_MC_BRIDGE_CONFIG_H */
