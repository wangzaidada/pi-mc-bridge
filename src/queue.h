/* queue.h - bounded blocking FIFO of bridge tasks. */
#ifndef PI_MC_BRIDGE_QUEUE_H
#define PI_MC_BRIDGE_QUEUE_H

#include <stdbool.h>
#include <pthread.h>

typedef enum {
    QUEUE_OK = 0,
    QUEUE_FULL,      /* push rejected: capacity reached */
    QUEUE_SHUTDOWN   /* operation rejected/aborted: queue is shutting down */
} QueueStatus;

typedef struct {
    char *player;   /* owning copy */
    char *prompt;   /* owning copy */
} Task;

typedef struct {
    Task           *slots;
    int             capacity;
    int             head;
    int             count;
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
    bool            shutdown;
} TaskQueue;

void   queue_init(TaskQueue *q, int capacity);
void   queue_free(TaskQueue *q);

/* Non-blocking on a full queue: returns QUEUE_FULL rather than waiting. */
QueueStatus queue_push(TaskQueue *q, const char *player, const char *prompt);

/* Blocks until a task is available or the queue is shut down. On QUEUE_OK the
 * task's strings are transferred to the caller (free them with task_free). */
QueueStatus queue_pop(TaskQueue *q, Task *out);

/* Wake every consumer; subsequent pushes are rejected. Idempotent. */
void   queue_shutdown(TaskQueue *q);
bool   queue_is_shutdown(TaskQueue *q);

/* Number of tasks currently queued (for tests/stats). */
int    queue_size(TaskQueue *q);

void   task_free(Task *t);

#endif /* PI_MC_BRIDGE_QUEUE_H */
