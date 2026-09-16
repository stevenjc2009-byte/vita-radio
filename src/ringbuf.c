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

/* Milliseconds since *from on a clock nothing can step. */
static long mono_elapsed_ms(const struct timespec *from)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long)(now.tv_sec - from->tv_sec) * 1000L
         + (now.tv_nsec - from->tv_nsec) / 1000000L;
}

/* Absolute deadline ms from now, in the clock pthread_cond_timedwait reads. */
static void wall_deadline_in(struct timespec *out, long ms)
{
    clock_gettime(CLOCK_REALTIME, out);
    out->tv_sec += ms / 1000;
    out->tv_nsec += (ms % 1000) * 1000000L;
    if (out->tv_nsec >= 1000000000L) {
        out->tv_sec += 1;
        out->tv_nsec -= 1000000000L;
    }
}

/* The deadline is on the wall clock, which a Vita time sync or the user
 * changing the date can step. Pinning the condvar to CLOCK_MONOTONIC via
 * pthread_condattr_setclock - the obvious fix - does not work on the target:
 * vitasdk's pthreads-embedded stores the clock in the attr but
 * pthread_cond_init reads only the attr's pshared field and ignores it, and
 * pthread_cond_timedwait passes abstime to sem_timedwait -> pte_relmillisecs,
 * which converts it against ftime() - the wall clock. Handing that a
 * since-boot monotonic deadline would make every wait expire instantly, so the
 * deadline has to stay where the implementation expects it.
 *
 * What that conversion does give us is immunity to the long stall: the wait is
 * turned into a relative delay once, on entry, so a step during it moves
 * nothing. The remaining exposure is a forward step landing between our
 * clock_gettime and the library's ftime, which would expire the wait early -
 * and the caller's r == 0 path has no sleep in it, so an early timeout spins a
 * core. Hence the monotonic reference below: a timeout is only reported once
 * the time really has passed, and a re-arm never asks for longer than the
 * caller did. */
long rb_read(RingBuf *rb, unsigned char *dst, size_t len, int timeout_ms)
{
    struct timespec deadline, started;
    if (timeout_ms > 0) {
        clock_gettime(CLOCK_MONOTONIC, &started);
        wall_deadline_in(&deadline, timeout_ms);
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
                long left = timeout_ms - mono_elapsed_ms(&started);
                if (left > 0) {          /* expired early: re-arm, do not spin */
                    wall_deadline_in(&deadline, left);
                    continue;
                }
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
