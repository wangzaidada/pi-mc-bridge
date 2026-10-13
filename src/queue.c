/* queue.c - see queue.h */
#include "queue.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void task_free(Task *t)
{
    if (!t)
        return;
    free(t->player);
    free(t->prompt);
    t->player = t->prompt = NULL;
}

void queue_init(TaskQueue *q, int capacity)
{
    if (capacity < 1)
        capacity = 1;
    q->slots = calloc((size_t)capacity, sizeof(Task));
    if (!q->slots) {
        fprintf(stderr, "fatal: out of memory\n");
        abort();
    }
    q->capacity = capacity;
    q->head = 0;
    q->count = 0;
    q->shutdown = false;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
}

void queue_free(TaskQueue *q)
{
    if (!q)
        return;
    for (int i = 0; i < q->count; i++) {
        int idx = (q->head + i) % q->capacity;
        task_free(&q->slots[idx]);
    }
    free(q->slots);
    q->slots = NULL;
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
}

QueueStatus queue_push(TaskQueue *q, const char *player, const char *prompt)
{
    pthread_mutex_lock(&q->lock);
    if (q->shutdown) {
        pthread_mutex_unlock(&q->lock);
        return QUEUE_SHUTDOWN;
    }
    if (q->count >= q->capacity) {
        pthread_mutex_unlock(&q->lock);
        return QUEUE_FULL;
    }
    int idx = (q->head + q->count) % q->capacity;
    q->slots[idx].player = xstrdup(player);
    q->slots[idx].prompt = xstrdup(prompt);
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
    return QUEUE_OK;
}

QueueStatus queue_pop(TaskQueue *q, Task *out)
{
    pthread_mutex_lock(&q->lock);
    while (q->count == 0 && !q->shutdown)
        pthread_cond_wait(&q->not_empty, &q->lock);

    if (q->count == 0 && q->shutdown) {
        pthread_mutex_unlock(&q->lock);
        return QUEUE_SHUTDOWN;
    }

    *out = q->slots[q->head];
    q->slots[q->head].player = NULL;
    q->slots[q->head].prompt = NULL;
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->lock);
    return QUEUE_OK;
}

void queue_shutdown(TaskQueue *q)
{
    pthread_mutex_lock(&q->lock);
    q->shutdown = true;
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->lock);
}

bool queue_is_shutdown(TaskQueue *q)
{
    pthread_mutex_lock(&q->lock);
    bool s = q->shutdown;
    pthread_mutex_unlock(&q->lock);
    return s;
}

int queue_size(TaskQueue *q)
{
    pthread_mutex_lock(&q->lock);
    int n = q->count;
    pthread_mutex_unlock(&q->lock);
    return n;
}
