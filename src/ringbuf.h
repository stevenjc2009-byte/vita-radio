#ifndef VR_RINGBUF_H
#define VR_RINGBUF_H

#include <stddef.h>
#include <pthread.h>

/* Thread-safe byte FIFO between one producer (network thread) and one
 * consumer (decode thread). Pure C + pthreads so it builds on host for tests. */
typedef struct {
    unsigned char  *data;
    size_t          cap;
    size_t          head;     /* next read index  */
    size_t          tail;     /* next write index */
    size_t          count;    /* bytes stored     */
    int             closed;   /* producer finished: readers drain, then get -1 */
    int             aborted;  /* stop requested: reads and writes return -1 at once */
    pthread_mutex_t lock;
    pthread_cond_t  cond;
} RingBuf;

int    rb_init(RingBuf *rb, size_t capacity);   /* 0 ok, -1 allocation failure */
void   rb_free(RingBuf *rb);
void   rb_reset(RingBuf *rb);                   /* empty it, clear closed + aborted */

/* Blocks until all len bytes are stored. Returns 0, or -1 if aborted
 * (also -1 if closed). */
int    rb_write(RingBuf *rb, const unsigned char *src, size_t len);

/* Waits up to timeout_ms (<0 = forever) for at least one byte.
 * Returns bytes read (1..len), 0 on timeout, -1 if aborted or closed-and-empty. */
long   rb_read(RingBuf *rb, unsigned char *dst, size_t len, int timeout_ms);

size_t rb_count(RingBuf *rb);
void   rb_close(RingBuf *rb);   /* producer done; wakes waiters */
void   rb_abort(RingBuf *rb);   /* wakes waiters; all calls fail from now on */

#endif
