/* logwatcher.h - tail a growing log file with rotation/truncation detection. */
#ifndef PI_MC_BRIDGE_LOGWATCHER_H
#define PI_MC_BRIDGE_LOGWATCHER_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* Called once per complete line (without the trailing newline), in order.
 * The line is NUL-terminated and valid only for the duration of the call. */
typedef void (*LogLineCb)(const char *line, void *user);

typedef struct {
    char      *path;
    int        fd;
    ino_t      inode;
    off_t      offset;
    char      *partial;   /* bytes after the last newline, carried over */
    size_t     partial_len;
    size_t     partial_cap;
    int        missing_warned; /* avoid spamming when the file is absent */
    LogLineCb  cb;
    void      *user;
    size_t     max_line;  /* lines longer than this are dropped, not buffered */
} LogWatcher;

/* Open `path`, record its inode, and seek to the end (pre-existing content is
 * NOT replayed). `max_line` bounds the carry-over buffer (0 => default 64 KiB). */
int  logwatcher_init(LogWatcher *w, const char *path, size_t max_line,
                     LogLineCb cb, void *user);
void logwatcher_free(LogWatcher *w);

/* One non-blocking poll: read whatever is new, invoke the callback per line.
 * Returns the number of complete lines delivered, or -1 on an unrecoverable
 * setup error (the watcher keeps trying on the next call for transient ones). */
int  logwatcher_poll(LogWatcher *w);

#endif /* PI_MC_BRIDGE_LOGWATCHER_H */
