#include "ringbuf.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int rb_init(RingBuf *rb, size_t capacity)
{
    memset(rb, 0, sizeof(*rb));
    if (capacity == 0)
        return -1;
    rb->data = malloc(capacity);
    if (!rb->data)
        return -1;
    rb->cap = capacity;
    if (pthread_mutex_init(&rb->lock, NULL) != 0) {
        free(rb->data);
        rb->data = NULL;
        return -1;
    }
    if (pthread_cond_init(&rb->cond, NULL) != 0) {
        pthread_mutex_destroy(&rb->lock);
        free(rb->data);
        rb->data = NULL;
        return -1;
    }
    return 0;
}

void rb_free(RingBuf *rb)
{
    if (!rb->data)
        return;
    pthread_cond_destroy(&rb->cond);
    pthread_mutex_destroy(&rb->lock);
    free(rb->data);
    rb->data = NULL;
    rb->cap = 0;
}

void rb_reset(RingBuf *rb)
{
    pthread_mutex_lock(&rb->lock);
    rb->head = rb->tail = rb->count = 0;
    rb->closed = 0;
    rb->aborted = 0;
    pthread_cond_broadcast(&rb->cond);
    pthread_mutex_unlock(&rb->lock);
}

int rb_write(RingBuf *rb, const unsigned char *src, size_t len)
{
    pthread_mutex_lock(&rb->lock);
    while (len > 0) {
        while (rb->count == rb->cap && !rb->aborted && !rb->closed)
            pthread_cond_wait(&rb->cond, &rb->lock);
        if (rb->aborted || rb->closed) {
            pthread_mutex_unlock(&rb->lock);
            return -1;
        }
        size_t space = rb->cap - rb->count;
        size_t n = len < space ? len : space;
        size_t first = rb->cap - rb->tail;
        if (first > n)
            first = n;
        memcpy(rb->data + rb->tail, src, first);
        if (n > first)
            memcpy(rb->data, src + first, n - first);
        rb->tail = (rb->tail + n) % rb->cap;
        rb->count += n;
        src += n;
        len -= n;
        pthread_cond_broadcast(&rb->cond);
    }
    pthread_mutex_unlock(&rb->lock);
    return 0;
}

long rb_read(RingBuf *rb, unsigned char *dst, size_t len, int timeout_ms)
{
    struct timespec deadline;
    if (timeout_ms > 0) {
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += timeout_ms / 1000;
        deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec += 1;
            deadline.tv_nsec -= 1000000000L;
        }
    }

    pthread_mutex_lock(&rb->lock);
    for (;;) {
        if (rb->aborted) {
            pthread_mutex_unlock(&rb->lock);
            return -1;
        }
        if (rb->count > 0)
            break;
        if (rb->closed) {
            pthread_mutex_unlock(&rb->lock);
            return -1;
        }
        if (timeout_ms == 0) {
            pthread_mutex_unlock(&rb->lock);
            return 0;
        }
        if (timeout_ms < 0) {
            pthread_cond_wait(&rb->cond, &rb->lock);
        } else if (pthread_cond_timedwait(&rb->cond, &rb->lock, &deadline) == ETIMEDOUT) {
            if (rb->aborted || (rb->count == 0 && rb->closed)) {
                pthread_mutex_unlock(&rb->lock);
                return -1;
            }
            if (rb->count == 0) {
                pthread_mutex_unlock(&rb->lock);
                return 0;
            }
        }
    }

    if (len == 0) {
        pthread_mutex_unlock(&rb->lock);
        return 0;
    }
    size_t n = len < rb->count ? len : rb->count;
    size_t first = rb->cap - rb->head;
    if (first > n)
        first = n;
    memcpy(dst, rb->data + rb->head, first);
    if (n > first)
        memcpy(dst + first, rb->data, n - first);
    rb->head = (rb->head + n) % rb->cap;
    rb->count -= n;
    pthread_cond_broadcast(&rb->cond);
    pthread_mutex_unlock(&rb->lock);
    return (long)n;
}

size_t rb_count(RingBuf *rb)
{
    pthread_mutex_lock(&rb->lock);
    size_t n = rb->count;
    pthread_mutex_unlock(&rb->lock);
    return n;
}

void rb_close(RingBuf *rb)
{
    pthread_mutex_lock(&rb->lock);
    rb->closed = 1;
    pthread_cond_broadcast(&rb->cond);
    pthread_mutex_unlock(&rb->lock);
}

void rb_abort(RingBuf *rb)
{
    pthread_mutex_lock(&rb->lock);
    rb->aborted = 1;
    pthread_cond_broadcast(&rb->cond);
    pthread_mutex_unlock(&rb->lock);
}
