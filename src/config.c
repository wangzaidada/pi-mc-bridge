/* config.c - see config.h */
#include "config.h"
#include "parser.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* ---------- defaults ---------- */

static void set_defaults(Config *c)
{
    memset(c, 0, sizeof(*c));
    c->rcon_host = xstrdup("127.0.0.1");
    c->rcon_port = 25575;
    c->rcon_pass = NULL;
    c->log_file = xstrdup("logs/latest.log");
    c->prefix = xstrdup("@pi");
    c->reply_prefix = xstrdup("[AI] ");
    c->pi_cmd = xstrdup("pi");
    c->pi_model = NULL;
    c->pi_timeout_ms = 60000;
    c->cooldown_ms = 10000;
    c->max_len = 200;
    c->poll_ms = 500;
    c->queue_max = 64;
    c->allow = NULL;
    c->log_level = LOG_INFO;
    c->allow_list = NULL;
    c->allow_count = 0;
}

static void replace(char **slot, const char *val)
{
    free(*slot);
    *slot = val ? xstrdup(val) : NULL;
}

/* ---------- env ---------- */

static void apply_env(Config *c)
{
    const char *v;
    if ((v = getenv("RCON_HOST")))     replace(&c->rcon_host, v);
    if ((v = getenv("RCON_PORT")))     c->rcon_port = atoi(v);
    if ((v = getenv("RCON_PASSWORD"))) replace(&c->rcon_pass, v);
    if ((v = getenv("LOG_FILE")))      replace(&c->log_file, v);
    if ((v = getenv("CHAT_PREFIX")))   replace(&c->prefix, v);
    if ((v = getenv("REPLY_PREFIX")))  replace(&c->reply_prefix, v);
    if ((v = getenv("PI_CMD")))        replace(&c->pi_cmd, v);
    if ((v = getenv("PI_MODEL")))      replace(&c->pi_model, v);
    if ((v = getenv("PI_TIMEOUT")))    c->pi_timeout_ms = atoi(v) * 1000;
    if ((v = getenv("COOLDOWN")))      c->cooldown_ms = atoi(v) * 1000;
    if ((v = getenv("MAX_LEN")))       c->max_len = atoi(v);
    if ((v = getenv("POLL_MS")))       c->poll_ms = atoi(v);
    if ((v = getenv("QUEUE_MAX")))     c->queue_max = atoi(v);
    if ((v = getenv("ALLOW_PLAYERS"))) replace(&c->allow, v);
    if ((v = getenv("LOG_LEVEL"))) {
        if (strcasecmp(v, "debug") == 0) c->log_level = LOG_DEBUG;
        else if (strcasecmp(v, "info") == 0) c->log_level = LOG_INFO;
        else if (strcasecmp(v, "warn") == 0) c->log_level = LOG_WARN;
        else if (strcasecmp(v, "error") == 0) c->log_level = LOG_ERROR;
    }
}

/* Strip trailing whitespace from a string in place. */
static void rstrip(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1]))
        s[--n] = '\0';
}

/* ---------- CLI ---------- */

/* Consume the value for an option that requires one. */
static const char *need_value(int argc, char **argv, int *i, const char *opt)
{
    if (*i + 1 >= argc) {
        fprintf(stderr, "error: option %s requires a value\n", opt);
        return NULL;
    }
    return argv[++(*i)];
}

static int parse_cli(Config *c, int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *val;

        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            config_print_usage(stdout);
            return -2;
        } else if (strcmp(a, "--rcon-host") == 0) {
            if (!(val = need_value(argc, argv, &i, a))) return -1;
            replace(&c->rcon_host, val);
        } else if (strcmp(a, "--rcon-port") == 0) {
            if (!(val = need_value(argc, argv, &i, a))) return -1;
            c->rcon_port = atoi(val);
        } else if (strcmp(a, "--rcon-pass") == 0) {
            if (!(val = need_value(argc, argv, &i, a))) return -1;
            replace(&c->rcon_pass, val);
        } else if (strcmp(a, "--log-file") == 0) {
            if (!(val = need_value(argc, argv, &i, a))) return -1;
            replace(&c->log_file, val);
        } else if (strcmp(a, "--prefix") == 0) {
            if (!(val = need_value(argc, argv, &i, a))) return -1;
            replace(&c->prefix, val);
        } else if (strcmp(a, "--reply-prefix") == 0) {
            if (!(val = need_value(argc, argv, &i, a))) return -1;
            replace(&c->reply_prefix, val);
        } else if (strcmp(a, "--pi-cmd") == 0) {
            if (!(val = need_value(argc, argv, &i, a))) return -1;
            replace(&c->pi_cmd, val);
        } else if (strcmp(a, "--pi-model") == 0) {
            if (!(val = need_value(argc, argv, &i, a))) return -1;
            replace(&c->pi_model, val);
        } else if (strcmp(a, "--timeout") == 0) {
            if (!(val = need_value(argc, argv, &i, a))) return -1;
            c->pi_timeout_ms = atoi(val) * 1000;
        } else if (strcmp(a, "--cooldown") == 0) {
            if (!(val = need_value(argc, argv, &i, a))) return -1;
            c->cooldown_ms = atoi(val) * 1000;
        } else if (strcmp(a, "--max-len") == 0) {
            if (!(val = need_value(argc, argv, &i, a))) return -1;
            c->max_len = atoi(val);
        } else if (strcmp(a, "--poll-ms") == 0) {
            if (!(val = need_value(argc, argv, &i, a))) return -1;
            c->poll_ms = atoi(val);
        } else if (strcmp(a, "--queue-max") == 0) {
            if (!(val = need_value(argc, argv, &i, a))) return -1;
            c->queue_max = atoi(val);
        } else if (strcmp(a, "--allow") == 0) {
            if (!(val = need_value(argc, argv, &i, a))) return -1;
            replace(&c->allow, val);
        } else if (strcmp(a, "--log-level") == 0) {
            if (!(val = need_value(argc, argv, &i, a))) return -1;
            if (strcasecmp(val, "debug") == 0) c->log_level = LOG_DEBUG;
            else if (strcasecmp(val, "info") == 0) c->log_level = LOG_INFO;
            else if (strcasecmp(val, "warn") == 0) c->log_level = LOG_WARN;
            else if (strcasecmp(val, "error") == 0) c->log_level = LOG_ERROR;
            else {
                fprintf(stderr, "error: invalid --log-level '%s'\n", val);
                return -1;
            }
        } else {
            fprintf(stderr, "error: unknown option '%s'\n", a);
            fprintf(stderr, "try --help\n");
            return -1;
        }
    }
    return 0;
}

/* ---------- validation & derived state ---------- */

static int validate(Config *c)
{
    if (!c->rcon_pass || !*c->rcon_pass) {
        fprintf(stderr,
                "error: RCON password is required "
                "(--rcon-pass or RCON_PASSWORD)\n");
        return -1;
    }
    if (c->rcon_port < 1 || c->rcon_port > 65535) {
        fprintf(stderr, "error: --rcon-port must be 1..65535 (got %d)\n",
                c->rcon_port);
        return -1;
    }
    if (c->pi_timeout_ms < 1000) {
        fprintf(stderr, "error: --timeout must be >= 1 second (got %d ms)\n",
                c->pi_timeout_ms);
        return -1;
    }
    if (c->cooldown_ms < 0) {
        fprintf(stderr, "error: --cooldown must be >= 0\n");
        return -1;
    }
    if (c->max_len < 1) {
        fprintf(stderr, "error: --max-len must be >= 1 (got %d)\n", c->max_len);
        return -1;
    }
    if (c->poll_ms < 20) {
        fprintf(stderr, "error: --poll-ms must be >= 20 (got %d)\n", c->poll_ms);
        return -1;
    }
    if (c->queue_max < 1) {
        fprintf(stderr, "error: --queue-max must be >= 1 (got %d)\n",
                c->queue_max);
        return -1;
    }
    if (!c->prefix || !*c->prefix) {
        fprintf(stderr, "error: --prefix must not be empty\n");
        return -1;
    }

    /* The trigger is matched as a whole token, so trailing spaces in the
     * configured prefix would break matching. Normalise them away. */
    rstrip(c->prefix);
    if (!*c->prefix) {
        fprintf(stderr, "error: --prefix must contain a non-space character\n");
        return -1;
    }
    return 0;
}

static void build_allow_list(Config *c)
{
    c->allow_list = NULL;
    c->allow_count = 0;
    if (!c->allow || !*c->allow)
        return;

    char *work = xstrdup(c->allow);
    size_t cap = 8;
    c->allow_list = malloc(sizeof(char *) * cap);
    if (!c->allow_list) {
        free(work);
        return;
    }
    char *save = NULL;
    for (char *tok = strtok_r(work, ",", &save); tok;
         tok = strtok_r(NULL, ",", &save)) {
        while (*tok && isspace((unsigned char)*tok))
            tok++;
        rstrip(tok);
        if (!*tok)
            continue;
        if ((size_t)c->allow_count >= cap) {
            cap *= 2;
            char **nl = realloc(c->allow_list, sizeof(char *) * cap);
            if (!nl)
                break;
            c->allow_list = nl;
        }
        c->allow_list[c->allow_count++] = xstrdup(tok);
    }
    free(work);
}

int config_load(Config *cfg, int argc, char **argv)
{
    set_defaults(cfg);
    apply_env(cfg);
    int rc = parse_cli(cfg, argc, argv);
    if (rc != 0)
        return rc; /* -1 error, -2 help */
    if (validate(cfg) != 0)
        return -1;
    build_allow_list(cfg);
    return 0;
}

bool config_player_allowed(const Config *cfg, const char *player)
{
    if (cfg->allow_count == 0)
        return true;
    for (int i = 0; i < cfg->allow_count; i++) {
        if (strcasecmp(cfg->allow_list[i], player) == 0)
            return true;
    }
    return false;
}

void config_free(Config *cfg)
{
    if (!cfg)
        return;
    free(cfg->rcon_host);
    free(cfg->rcon_pass);
    free(cfg->log_file);
    free(cfg->prefix);
    free(cfg->reply_prefix);
    free(cfg->pi_cmd);
    free(cfg->pi_model);
    free(cfg->allow);
    if (cfg->allow_list) {
        for (int i = 0; i < cfg->allow_count; i++)
            free(cfg->allow_list[i]);
        free(cfg->allow_list);
    }
    memset(cfg, 0, sizeof(*cfg));
}

void config_print_usage(FILE *out)
{
    fprintf(out,
"pi-mc-bridge - bridge Minecraft chat (@pi) to the Pi Coding Agent\n"
"\n"
"Usage: pi-mc-bridge [options]\n"
"\n"
"Options (env var, default):\n"
"  --rcon-host HOST     RCON host           (RCON_HOST, 127.0.0.1)\n"
"  --rcon-port PORT     RCON port           (RCON_PORT, 25575)\n"
"  --rcon-pass PASS     RCON password       (RCON_PASSWORD, REQUIRED)\n"
"  --log-file PATH      server log to tail  (LOG_FILE, logs/latest.log)\n"
"  --prefix TEXT        chat trigger token  (CHAT_PREFIX, @pi)\n"
"  --reply-prefix TEXT  reply prefix        (REPLY_PREFIX, \"[AI] \")\n"
"  --pi-cmd PATH        pi executable       (PI_CMD, pi)\n"
"  --pi-model MODEL     pi model id         (PI_MODEL, pi default)\n"
"  --timeout SECONDS    pi timeout          (PI_TIMEOUT, 60)\n"
"  --cooldown SECONDS   per-player cooldown (COOLDOWN, 10)\n"
"  --max-len CHARS      max chars per reply (MAX_LEN, 200)\n"
"  --poll-ms MS         log poll interval   (POLL_MS, 500)\n"
"  --queue-max N        task queue capacity (QUEUE_MAX, 64)\n"
"  --allow LIST         comma-separated allowed players (ALLOW_PLAYERS, all)\n"
"  --log-level LEVEL    debug|info|warn|error (LOG_LEVEL, info)\n"
"  -h, --help           show this help\n");
}
