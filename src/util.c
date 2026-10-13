/* util.c - see util.h */
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>

/* ---------- logging ---------- */

static LogLevel g_level = LOG_INFO;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

void log_set_level(LogLevel lvl) { g_level = lvl; }
LogLevel log_get_level(void) { return g_level; }

static const char *level_name(LogLevel lvl)
{
    switch (lvl) {
    case LOG_DEBUG: return "DEBUG";
    case LOG_INFO:  return "INFO";
    case LOG_WARN:  return "WARN";
    case LOG_ERROR: return "ERROR";
    default:        return "?";
    }
}

void log_msg(LogLevel lvl, const char *fmt, ...)
{
    if (lvl < g_level)
        return;

    FILE *out = (lvl >= LOG_WARN) ? stderr : stdout;

    char stamp[16];
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(stamp, sizeof(stamp), "%H:%M:%S", &tmv);

    /* One lock around the whole line so concurrent workers can't interleave. */
    pthread_mutex_lock(&g_log_lock);
    fprintf(out, "[%s] %-5s ", stamp, level_name(lvl));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fputc('\n', out);
    fflush(out);
    pthread_mutex_unlock(&g_log_lock);
}

/* ---------- misc ---------- */

char *xstrdup(const char *s)
{
    if (!s)
        return NULL;
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (!p) {
        fprintf(stderr, "fatal: out of memory\n");
        abort();
    }
    memcpy(p, s, n);
    return p;
}

long long monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

/* ---------- dynamic buffer ---------- */

void dynbuf_init(DynBuf *d)
{
    d->buf = NULL;
    d->len = 0;
    d->cap = 0;
}

void dynbuf_free(DynBuf *d)
{
    free(d->buf);
    d->buf = NULL;
    d->len = d->cap = 0;
}

void dynbuf_reset(DynBuf *d)
{
    d->len = 0;
    if (d->buf)
        d->buf[0] = '\0';
}

void dynbuf_reserve(DynBuf *d, size_t need)
{
    /* need is the required capacity *including* the trailing NUL. */
    if (need <= d->cap)
        return;
    size_t cap = d->cap ? d->cap : 32;
    while (cap < need)
        cap *= 2;
    char *p = realloc(d->buf, cap);
    if (!p) {
        fprintf(stderr, "fatal: out of memory\n");
        abort();
    }
    if (!d->buf)
        p[0] = '\0';
    d->buf = p;
    d->cap = cap;
}

void dynbuf_append(DynBuf *d, const char *data, size_t n)
{
    if (n == 0)
        return;
    dynbuf_reserve(d, d->len + n + 1);
    memcpy(d->buf + d->len, data, n);
    d->len += n;
    d->buf[d->len] = '\0';
}

void dynbuf_append_str(DynBuf *d, const char *s)
{
    if (s)
        dynbuf_append(d, s, strlen(s));
}

void dynbuf_append_char(DynBuf *d, char c)
{
    dynbuf_reserve(d, d->len + 2);
    d->buf[d->len++] = c;
    d->buf[d->len] = '\0';
}

void dynbuf_printf(DynBuf *d, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);

    int need = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (need < 0) {
        va_end(ap);
        return;
    }
    dynbuf_reserve(d, d->len + (size_t)need + 1);
    vsnprintf(d->buf + d->len, (size_t)need + 1, fmt, ap);
    va_end(ap);
    d->len += (size_t)need;
}

/* ---------- JSON escaping ---------- */

void json_escape_append(DynBuf *out, const char *in)
{
    if (!in)
        return;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        unsigned char c = *p;
        switch (c) {
        case '"':  dynbuf_append_str(out, "\\\""); break;
        case '\\': dynbuf_append_str(out, "\\\\"); break;
        case '\b': dynbuf_append_str(out, "\\b");  break;
        case '\f': dynbuf_append_str(out, "\\f");  break;
        case '\n': dynbuf_append_str(out, "\\n");  break;
        case '\r': dynbuf_append_str(out, "\\r");  break;
        case '\t': dynbuf_append_str(out, "\\t");  break;
        default:
            if (c < 0x20) {
                char tmp[8];
                snprintf(tmp, sizeof(tmp), "\\u%04x", c);
                dynbuf_append_str(out, tmp);
            } else {
                dynbuf_append_char(out, (char)c);
            }
        }
    }
}

/* ---------- minimal JSON scanner ---------- */

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    return p;
}

static const char *skip_string(const char *p)
{
    /* p points at the opening quote. */
    p++;
    while (*p) {
        if (*p == '\\') {
            p++;
            if (!*p)
                return NULL;
            p++;
        } else if (*p == '"') {
            return p + 1;
        } else {
            p++;
        }
    }
    return NULL;
}

const char *json_skip_value(const char *p)
{
    p = skip_ws(p);
    if (*p == '"')
        return skip_string(p);
    if (*p == '{' || *p == '[') {
        char open = *p, close = (open == '{') ? '}' : ']';
        int depth = 0;
        while (*p) {
            if (*p == '"') {
                p = skip_string(p);
                if (!p)
                    return NULL;
                continue;
            }
            if (*p == open)
                depth++;
            else if (*p == close) {
                depth--;
                if (depth == 0)
                    return p + 1;
            }
            p++;
        }
        return NULL;
    }
    /* number / true / false / null: run to a delimiter */
    const char *start = p;
    while (*p && *p != ',' && *p != '}' && *p != ']' &&
           *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
        p++;
    return (p == start) ? NULL : p;
}

static void append_utf8(DynBuf *out, unsigned cp)
{
    if (cp <= 0x7F) {
        dynbuf_append_char(out, (char)cp);
    } else if (cp <= 0x7FF) {
        dynbuf_append_char(out, (char)(0xC0 | (cp >> 6)));
        dynbuf_append_char(out, (char)(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        dynbuf_append_char(out, (char)(0xE0 | (cp >> 12)));
        dynbuf_append_char(out, (char)(0x80 | ((cp >> 6) & 0x3F)));
        dynbuf_append_char(out, (char)(0x80 | (cp & 0x3F)));
    } else {
        dynbuf_append_char(out, (char)(0xF0 | (cp >> 18)));
        dynbuf_append_char(out, (char)(0x80 | ((cp >> 12) & 0x3F)));
        dynbuf_append_char(out, (char)(0x80 | ((cp >> 6) & 0x3F)));
        dynbuf_append_char(out, (char)(0x80 | (cp & 0x3F)));
    }
}

static int hex4(const char *p, unsigned *out)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9')      v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return -1;
    }
    *out = v;
    return 0;
}

const char *json_str_decode(const char *p, DynBuf *out)
{
    if (*p != '"')
        return NULL;
    p++;
    while (*p) {
        if (*p == '"')
            return p + 1;
        if (*p != '\\') {
            if (out)
                dynbuf_append_char(out, *p);
            p++;
            continue;
        }
        /* escape */
        p++;
        char e = *p;
        if (!e)
            return NULL;
        switch (e) {
        case '"':  if (out) dynbuf_append_char(out, '"');  break;
        case '\\': if (out) dynbuf_append_char(out, '\\'); break;
        case '/':  if (out) dynbuf_append_char(out, '/');  break;
        case 'b':  if (out) dynbuf_append_char(out, '\b'); break;
        case 'f':  if (out) dynbuf_append_char(out, '\f'); break;
        case 'n':  if (out) dynbuf_append_char(out, '\n'); break;
        case 'r':  if (out) dynbuf_append_char(out, '\r'); break;
        case 't':  if (out) dynbuf_append_char(out, '\t'); break;
        case 'u': {
            unsigned cp;
            if (hex4(p + 1, &cp) != 0)
                return NULL;
            p += 4;
            /* surrogate pair */
            if (cp >= 0xD800 && cp <= 0xDBFF &&
                p[1] == '\\' && p[2] == 'u') {
                unsigned lo;
                if (hex4(p + 3, &lo) == 0 && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                }
            }
            if (out)
                append_utf8(out, cp);
            break;
        }
        default:
            return NULL;
        }
        p++;
    }
    return NULL;
}

const char *json_obj_get(const char *obj, const char *key)
{
    if (*obj != '{')
        return NULL;
    const char *p = skip_ws(obj + 1);
    while (*p && *p != '}') {
        if (*p != '"')
            return NULL;
        /* Read this key into a small stack buffer for comparison. */
        const char *key_start = p;
        const char *after = skip_string(p);
        if (!after)
            return NULL;
        size_t klen = (size_t)(after - key_start - 2); /* minus quotes */
        const char *klit = key_start + 1;

        p = skip_ws(after);
        if (*p != ':')
            return NULL;
        p = skip_ws(p + 1);

        if (strlen(key) == klen && strncmp(klit, key, klen) == 0)
            return p; /* direct child value */

        p = json_skip_value(p);
        if (!p)
            return NULL;
        p = skip_ws(p);
        if (*p == ',') {
            p = skip_ws(p + 1);
            continue;
        }
        break;
    }
    return NULL;
}

/* ---------- UTF-8 ---------- */

size_t utf8_segment_len(const char *s, size_t max_chars)
{
    size_t chars = 0;
    size_t i = 0;
    while (s[i] && chars < max_chars) {
        unsigned char c = (unsigned char)s[i];
        size_t adv;
        if (c < 0x80)
            adv = 1;
        else if ((c & 0xE0) == 0xC0)
            adv = 2;
        else if ((c & 0xF0) == 0xE0)
            adv = 3;
        else if ((c & 0xF8) == 0xF0)
            adv = 4;
        else
            adv = 1; /* invalid lead byte: treat as 1, never advance past NUL */

        /* Don't run past the end of the string on a truncated tail. */
        size_t j;
        for (j = 1; j < adv && s[i + j]; j++)
            ;
        i += j;
        chars++;
    }
    return i;
}

size_t utf8_count(const char *s)
{
    size_t n = 0;
    while (*s) {
        unsigned char c = (unsigned char)*s;
        size_t adv = 1;
        n++;
        if (c < 0x80) adv = 1;
        else if ((c & 0xE0) == 0xC0) adv = 2;
        else if ((c & 0xF0) == 0xE0) adv = 3;
        else if ((c & 0xF8) == 0xF0) adv = 4;
        size_t j;
        for (j = 1; j < adv && s[j]; j++)
            ;
        s += j;
    }
    return n;
}
