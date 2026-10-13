/* util.h - dynamic buffers, JSON escaping/scanning, UTF-8 helpers, logging. */
#ifndef PI_MC_BRIDGE_UTIL_H
#define PI_MC_BRIDGE_UTIL_H

#include <stddef.h>
#include <stdarg.h>
#include <time.h>

/* ---------- logging ---------- */

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO  = 1,
    LOG_WARN  = 2,
    LOG_ERROR = 3
} LogLevel;

void log_set_level(LogLevel lvl);
LogLevel log_get_level(void);
/* INFO/DEBUG go to stdout, WARN/ERROR to stderr. Adds a [HH:MM:SS] prefix. */
void log_msg(LogLevel lvl, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define LOG_DEBUGF(...) log_msg(LOG_DEBUG, __VA_ARGS__)
#define LOG_INFOF(...)  log_msg(LOG_INFO,  __VA_ARGS__)
#define LOG_WARNF(...)  log_msg(LOG_WARN,  __VA_ARGS__)
#define LOG_ERRORF(...) log_msg(LOG_ERROR, __VA_ARGS__)

/* ---------- misc ---------- */

/* Duplicate a string; aborts on OOM. */
char *xstrdup(const char *s);
/* Monotonic milliseconds (CLOCK_MONOTONIC). */
long long monotonic_ms(void);

/* ---------- dynamic buffer ---------- */

typedef struct {
    char  *buf;   /* always NUL-terminated */
    size_t len;   /* bytes used, excluding NUL */
    size_t cap;
} DynBuf;

void dynbuf_init(DynBuf *d);
void dynbuf_free(DynBuf *d);
void dynbuf_reset(DynBuf *d);           /* keep capacity, len = 0 */
void dynbuf_reserve(DynBuf *d, size_t need);
void dynbuf_append(DynBuf *d, const char *data, size_t n);
void dynbuf_append_str(DynBuf *d, const char *s);
void dynbuf_append_char(DynBuf *d, char c);
void dynbuf_printf(DynBuf *d, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* ---------- JSON escaping ---------- */

/* Append `in` to `out` as the *contents* of a JSON string (no surrounding
 * quotes). Escapes ", \, and control characters; UTF-8 passes through. */
void json_escape_append(DynBuf *out, const char *in);

/* ---------- minimal JSON scanner (sufficient for pi's NDJSON) ---------- */

/* Skip a JSON value starting at p (whitespace already skipped by caller).
 * Returns pointer just past the value, or NULL on malformed input. */
const char *json_skip_value(const char *p);

/* Decode a JSON string literal at p (p must point at '"'). Returns pointer just
 * past the closing quote, or NULL on error. If out != NULL, the decoded bytes
 * are appended. */
const char *json_str_decode(const char *p, DynBuf *out);

/* Find direct child `key` of the JSON object at obj (obj points at '{').
 * Returns pointer to the value (surrounding whitespace skipped), or NULL. */
const char *json_obj_get(const char *obj, const char *key);

/* ---------- UTF-8 ---------- */

/* Byte length of the longest prefix of `s` covering at most `max_chars` code
 * points. Never splits a multi-byte sequence. Returns 0 for an empty string. */
size_t utf8_segment_len(const char *s, size_t max_chars);

/* Number of code points in a NUL-terminated string (invalid bytes count as 1). */
size_t utf8_count(const char *s);

#endif /* PI_MC_BRIDGE_UTIL_H */
