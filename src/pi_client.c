/* pi_client.c - see pi_client.h */
#include "pi_client.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---------- small helpers over the util JSON scanner ---------- */

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    return p;
}

/* True if the JSON string value at `value` (pointing at a '"') equals `lit`. */
static bool json_value_equals(const char *value, const char *lit)
{
    if (!value || *value != '"')
        return false;
    DynBuf tmp;
    dynbuf_init(&tmp);
    const char *end = json_str_decode(value, &tmp);
    bool eq = end && strcmp(tmp.buf ? tmp.buf : "", lit) == 0;
    dynbuf_free(&tmp);
    return eq;
}

/* Append the text of every {"type":"text","text":"..."} block in the JSON
 * array at `arr` (points at '['). Ignores thinking blocks and unknown types. */
static void append_text_blocks(const char *arr, DynBuf *answer)
{
    if (*arr != '[')
        return;
    const char *p = skip_ws(arr + 1);
    while (*p && *p != ']') {
        const char *after = json_skip_value(p);
        if (!after)
            return;
        if (*p == '{') {
            const char *tv = json_obj_get(p, "type");
            if (json_value_equals(tv, "text")) {
                const char *text = json_obj_get(p, "text");
                if (text && *text == '"')
                    json_str_decode(text, answer);
            }
        }
        p = skip_ws(after);
        if (*p == ',')
            p = skip_ws(p + 1);
    }
}

/* Parse one NDJSON line. If it is an assistant message_end, replace whatever
 * was accumulated with this message's text and record its stop reason. */
static void parse_line(const char *line, size_t len, DynBuf *answer,
                       bool *has_message, bool *stop_error, char **error_message)
{
    (void)len; /* line is NUL-terminated by the caller */
    const char *p = skip_ws(line);
    if (*p != '{')
        return; /* ignore non-JSON noise lines */

    const char *tv = json_obj_get(p, "type");
    if (!json_value_equals(tv, "message_end"))
        return;

    const char *mv = json_obj_get(p, "message");
    if (!mv || *mv != '{')
        return;
    const char *rv = json_obj_get(mv, "role");
    if (!json_value_equals(rv, "assistant"))
        return;

    /* This is (another) assistant message_end: start fresh, keep the last. */
    dynbuf_reset(answer);
    *has_message = true;
    *stop_error = false;
    if (*error_message) {
        free(*error_message);
        *error_message = NULL;
    }

    const char *cv = json_obj_get(mv, "content");
    if (cv && *cv == '[')
        append_text_blocks(cv, answer);

    const char *sv = json_obj_get(mv, "stopReason");
    if (json_value_equals(sv, "error")) {
        *stop_error = true;
        const char *ev = json_obj_get(mv, "errorMessage");
        if (ev && *ev == '"') {
            DynBuf em;
            dynbuf_init(&em);
            json_str_decode(ev, &em);
            *error_message = xstrdup(em.buf ? em.buf : "");
            dynbuf_free(&em);
        }
    }
}

bool pi_parse_stream(const char *data, size_t len, DynBuf *answer,
                     bool *stop_error, char **error_message)
{
    bool has_message = false;
    bool se = false;
    char *em = NULL;

    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i != len && data[i] != '\n')
            continue;
        size_t ll = i - start;
        /* Work on a NUL-terminated copy of the line (may contain escaped \n). */
        char *line = malloc(ll + 1);
        if (!line)
            break;
        memcpy(line, data + start, ll);
        line[ll] = '\0';
        parse_line(line, ll, answer, &has_message, &se, &em);
        free(line);
        start = i + 1;
    }

    if (stop_error)
        *stop_error = se;
    if (error_message)
        *error_message = em;
    else
        free(em);
    return has_message;
}

/* ---------- process execution ---------- */

void pi_client_init(PiClient *c, const char *cmd, const char *model,
                    int timeout_ms)
{
    memset(c, 0, sizeof(*c));
    c->cmd = xstrdup(cmd && *cmd ? cmd : "pi");
    c->model = (model && *model) ? xstrdup(model) : NULL;
    c->timeout_ms = timeout_ms > 0 ? timeout_ms : 60000;
    c->extra_args = NULL;
}

void pi_client_free(PiClient *c)
{
    if (!c)
        return;
    free(c->cmd);
    free(c->model);
    free(c->extra_args);
    c->cmd = c->model = c->extra_args = NULL;
}

void pi_client_set_model(PiClient *c, const char *model)
{
    if (!c)
        return;
    free(c->model);
    c->model = (model && *model) ? xstrdup(model) : NULL;
}

void pi_client_set_timeout(PiClient *c, int timeout_ms)
{
    if (c && timeout_ms > 0)
        c->timeout_ms = timeout_ms;
}

char **pi_build_argv(PiClient *c, const char *prompt)
{
    /* pi --mode json --no-session --no-tools [--model M] -p <prompt>.
     * The list is fixed: the bridge transports `<player>: <text>` as data and
     * never injects a system prompt (see design D3). Exposed for unit tests. */
    size_t n = 0;
    char **argv = malloc(sizeof(char *) * 12);
    if (!argv)
        abort();
    argv[n++] = c->cmd;
    argv[n++] = (char *)"--mode";
    argv[n++] = (char *)"json";
    argv[n++] = (char *)"--no-session";
    argv[n++] = (char *)"--no-tools";
    if (c->model) {
        argv[n++] = (char *)"--model";
        argv[n++] = c->model;
    }
    argv[n++] = (char *)"-p";
    argv[n++] = (char *)prompt;
    argv[n] = NULL;
    return argv;
}

/* Reap the child, escalating SIGTERM -> SIGKILL if it lingers. */
static int kill_and_reap(pid_t pid)
{
    kill(pid, SIGTERM);
    for (int i = 0; i < 20; i++) { /* up to ~1 s */
        int status;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid)
            return status;
        struct timespec ts = { 0, 50 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    kill(pid, SIGKILL);
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    return status;
}

PiStatus pi_run(PiClient *c, const char *prompt, DynBuf *raw, char **err_detail,
                int *exit_status)
{
    if (err_detail)
        *err_detail = NULL;
    dynbuf_reset(raw);

    int outpipe[2], errpipe[2];
    if (pipe(outpipe) != 0 || pipe(errpipe) != 0) {
        if (err_detail)
            *err_detail = xstrdup(strerror(errno));
        return PI_ERR_SPAWN;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(outpipe[0]); close(outpipe[1]);
        close(errpipe[0]); close(errpipe[1]);
        if (err_detail)
            *err_detail = xstrdup(strerror(errno));
        return PI_ERR_SPAWN;
    }

    if (pid == 0) {
        /* Child: stdin from /dev/null, stdout/stderr to our pipes. */
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }
        dup2(outpipe[1], STDOUT_FILENO);
        dup2(errpipe[1], STDERR_FILENO);
        close(outpipe[0]); close(outpipe[1]);
        close(errpipe[0]); close(errpipe[1]);
        /* Leave our signal dispositions default for the exec'd process. */
        signal(SIGTERM, SIG_DFL);
        char **argv = pi_build_argv(c, prompt);
        execvp(c->cmd, argv);
        /* exec failed */
        fprintf(stderr, "pi-mc-bridge: cannot exec %s: %s\n", c->cmd,
                strerror(errno));
        _exit(127);
    }

    /* Parent */
    close(outpipe[1]);
    close(errpipe[1]);

    long long deadline = monotonic_ms() + c->timeout_ms;
    DynBuf out, errout;
    dynbuf_init(&out);
    dynbuf_init(&errout);

    bool timed_out = false;
    struct pollfd fds[2];
    bool out_open = true, err_open = true;

    while (out_open || err_open) {
        long long now = monotonic_ms();
        int remain = (int)(deadline - now);
        if (remain <= 0) {
            timed_out = true;
            break;
        }
        fds[0].fd = out_open ? outpipe[0] : -1;
        fds[0].events = POLLIN;
        fds[1].fd = err_open ? errpipe[0] : -1;
        fds[1].events = POLLIN;

        int pr = poll(fds, 2, remain);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pr == 0) {
            timed_out = true;
            break;
        }
        for (int i = 0; i < 2; i++) {
            if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP)))
                continue;
            char buf[8192];
            ssize_t r = read(fds[i].fd, buf, sizeof(buf));
            if (r > 0) {
                if (i == 0)
                    dynbuf_append(&out, buf, (size_t)r);
                else
                    dynbuf_append(&errout, buf, (size_t)r);
            } else if (r == 0) {
                if (i == 0) { close(outpipe[0]); out_open = false; }
                else        { close(errpipe[0]); err_open = false; }
            } else if (errno != EINTR) {
                if (i == 0) { close(outpipe[0]); out_open = false; }
                else        { close(errpipe[0]); err_open = false; }
            }
        }
    }

    if (out_open) close(outpipe[0]);
    if (err_open) close(errpipe[0]);

    int status = 0;
    if (timed_out) {
        status = kill_and_reap(pid);
    } else {
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
            ;
    }

    if (err_detail && errout.len)
        *err_detail = xstrdup(errout.buf);

    if (timed_out) {
        LOG_WARNF("pi: timed out after %d ms", c->timeout_ms);
    } else if (exit_status) {
        *exit_status = status;
    }

    /* pi's stdout is handed back raw; interpretation is the reply shaper's job. */
    dynbuf_append(raw, out.buf ? out.buf : "", out.len);

    dynbuf_free(&out);
    dynbuf_free(&errout);
    return timed_out ? PI_ERR_TIMEOUT : PI_OK;
}

PiStatus pi_extract_text(const char *raw, size_t len, int exit_status,
                         DynBuf *answer, char **err_detail)
{
    if (err_detail)
        *err_detail = NULL;
    dynbuf_reset(answer);

    bool stop_error = false;
    char *error_message = NULL;
    bool has_message = pi_parse_stream(raw ? raw : "", len, answer,
                                       &stop_error, &error_message);
    if (stop_error) {
        if (err_detail)
            *err_detail = error_message ? error_message
                                        : xstrdup("pi reported an error");
        else
            free(error_message);
        return PI_ERR_STOP;
    }

    if (!has_message || answer->len == 0) {
        /* No text. Distinguish a clean-but-empty result from a crash. */
        if (exit_status > 0 && WIFEXITED(exit_status) &&
            WEXITSTATUS(exit_status) != 0)
            return PI_ERR_EXIT;
        return PI_ERR_NO_TEXT;
    }

    if (exit_status > 0 && WIFEXITED(exit_status) &&
        WEXITSTATUS(exit_status) != 0)
        LOG_DEBUGF("pi: exit status %d despite a reply", WEXITSTATUS(exit_status));
    return PI_OK;
}
