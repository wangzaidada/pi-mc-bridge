/* logwatcher.c - see logwatcher.h */
#include "logwatcher.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DEFAULT_MAX_LINE (64 * 1024)

/* Re-open the file at w->path, seek to end, refresh inode/offset. Used both at
 * init and after a rotation is detected. */
static int reopen_at_end(LogWatcher *w)
{
    int fd = open(w->path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;

    struct stat st;
    if (fstat(fd, &st) != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }

    /* open() leaves the file position at 0, so we must seek explicitly to the
     * end; otherwise the first read() replays the whole file. */
    if (lseek(fd, st.st_size, SEEK_SET) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }

    if (w->fd >= 0)
        close(w->fd);
    w->fd = fd;
    w->inode = st.st_ino;
    w->offset = st.st_size;   /* start from the end: don't replay history */
    return 0;
}

int logwatcher_init(LogWatcher *w, const char *path, size_t max_line,
                    LogLineCb cb, void *user)
{
    memset(w, 0, sizeof(*w));
    w->fd = -1;
    w->path = xstrdup(path);
    w->cb = cb;
    w->user = user;
    w->max_line = max_line ? max_line : DEFAULT_MAX_LINE;

    if (reopen_at_end(w) != 0) {
        /* Not fatal: the file may not exist yet (server not started). */
        LOG_WARNF("log file %s not readable yet: %s (will keep retrying)",
                  w->path, strerror(errno));
        w->fd = -1;
        w->offset = 0;
    } else {
        LOG_INFOF("watching %s (inode=%llu, offset=%lld)",
                  w->path, (unsigned long long)w->inode, (long long)w->offset);
    }
    return 0;
}

void logwatcher_free(LogWatcher *w)
{
    if (!w)
        return;
    if (w->fd >= 0)
        close(w->fd);
    free(w->path);
    free(w->partial);
    w->path = NULL;
    w->partial = NULL;
}

/* Append `n` bytes to the carry-over buffer. Returns 0 on success, -1 if the
 * line has grown past max_line (caller should drop it). */
static int partial_append(LogWatcher *w, const char *data, size_t n)
{
    if (w->partial_len + n > w->max_line)
        return -1;
    if (w->partial_len + n + 1 > w->partial_cap) {
        size_t cap = w->partial_cap ? w->partial_cap : 256;
        while (cap < w->partial_len + n + 1)
            cap *= 2;
        char *p = realloc(w->partial, cap);
        if (!p) {
            fprintf(stderr, "fatal: out of memory\n");
            abort();
        }
        w->partial = p;
        w->partial_cap = cap;
    }
    memcpy(w->partial + w->partial_len, data, n);
    w->partial_len += n;
    w->partial[w->partial_len] = '\0';
    return 0;
}

/* Emit the current partial buffer as a line (stripping a trailing CR) and
 * reset it. Returns the delivered count (1) or 0 if there was nothing. */
static int flush_partial(LogWatcher *w)
{
    if (w->partial_len == 0)
        return 0;
    if (w->partial[w->partial_len - 1] == '\r')
        w->partial[--w->partial_len] = '\0';
    w->cb(w->partial, w->user);
    w->partial_len = 0;
    if (w->partial)
        w->partial[0] = '\0';
    return 1;
}

/* Feed a chunk of newly read bytes through the line splitter. */
static int process_chunk(LogWatcher *w, const char *data, size_t n)
{
    int delivered = 0;
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        if (data[i] != '\n')
            continue;
        /* Complete line: partial + data[start..i) */
        int dropped = 0;
        if (w->partial_len && partial_append(w, data + start, i - start) != 0) {
            dropped = 1;
        } else if (!w->partial_len) {
            /* Fast path: no carry-over, no need to copy through `partial`. */
            size_t len = i - start;
            if (len > w->max_line) {
                dropped = 1;
            } else {
                if (partial_append(w, data + start, len) != 0)
                    dropped = 1;
            }
        }
        if (dropped) {
            LOG_WARNF("dropped an over-long log line (limit %zu bytes)",
                      w->max_line);
            w->partial_len = 0;
            if (w->partial)
                w->partial[0] = '\0';
        } else {
            delivered += flush_partial(w);
        }
        start = i + 1;
    }
    /* Carry over the unterminated tail. */
    if (start < n) {
        size_t tail = n - start;
        if (partial_append(w, data + start, tail) != 0) {
            LOG_WARNF("dropped an over-long log line (limit %zu bytes)",
                      w->max_line);
            w->partial_len = 0;
            if (w->partial)
                w->partial[0] = '\0';
        }
    }
    return delivered;
}

int logwatcher_poll(LogWatcher *w)
{
    struct stat st;

    if (stat(w->path, &st) != 0) {
        if (!w->missing_warned) {
            LOG_WARNF("log file %s disappeared: %s", w->path, strerror(errno));
            w->missing_warned = 1;
        }
        return 0;
    }

    /* Rotation: a different inode means the old file was renamed away. */
    if (w->fd < 0 || st.st_ino != w->inode) {
        LOG_INFOF("log rotation detected on %s (inode %llu -> %llu), reopening",
                  w->path, (unsigned long long)w->inode,
                  (unsigned long long)st.st_ino);
        w->partial_len = 0;
        if (reopen_at_end(w) != 0) {
            LOG_WARNF("failed to reopen %s: %s", w->path, strerror(errno));
            return 0;
        }
        w->missing_warned = 0;
        return 0;
    }

    /* Truncation: same inode but the file shrank below our offset. */
    if (st.st_size < w->offset) {
        LOG_INFOF("log file %s truncated (%lld -> %lld), seeking to start",
                  w->path, (long long)w->offset, (long long)st.st_size);
        w->offset = 0;
        w->partial_len = 0;
        if (lseek(w->fd, 0, SEEK_SET) < 0)
            return -1;
    }

    if (st.st_size == w->offset)
        return 0; /* nothing new */

    w->missing_warned = 0;

    int delivered = 0;
    char buf[8192];
    while (true) {
        ssize_t r = read(w->fd, buf, sizeof(buf));
        if (r < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            LOG_WARNF("read error on %s: %s", w->path, strerror(errno));
            break;
        }
        if (r == 0)
            break;
        w->offset += r;
        delivered += process_chunk(w, buf, (size_t)r);
        if ((size_t)r < sizeof(buf))
            break; /* drained */
    }
    return delivered;
}
