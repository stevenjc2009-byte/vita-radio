#include "ringbuf.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int g_failures;
static int g_checks;

/* g_checks counts every assertion, passing or not, so the final line can use
   the same "N passed, M failed" wording as the other suites. Without it this
   file printed only "ALL PASS" and the aggregate check count silently
   excluded the one module where the concurrency bugs live. */
#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { printf("  CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } \
} while (0)

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void report(const char *name, int before)
{
    printf("%s: %s\n", g_failures == before ? "PASS" : "FAIL", name);
}

static void test_wraparound_and_partial(void)
{
    int before = g_failures;
    RingBuf rb;
    CHECK(rb_init(&rb, 16) == 0);
    unsigned char in[32], out[32];
    for (int i = 0; i < 32; i++)
        in[i] = (unsigned char)i;

    CHECK(rb_write(&rb, in, 10) == 0);
    CHECK(rb_count(&rb) == 10);
    CHECK(rb_read(&rb, out, 6, 0) == 6);
    CHECK(memcmp(out, in, 6) == 0);
    CHECK(rb_write(&rb, in + 10, 12) == 0);   /* tail wraps past cap */
    CHECK(rb_count(&rb) == 16);

    /* partial read: ask for 32, get the 16 that are there */
    CHECK(rb_read(&rb, out, sizeof(out), 0) == 16);
    CHECK(memcmp(out, in + 6, 16) == 0);
    CHECK(rb_count(&rb) == 0);

    /* small partial reads across the wrap point */
    CHECK(rb_write(&rb, in, 14) == 0);
    long got = 0;
    while (got < 14) {
        long r = rb_read(&rb, out + got, 3, 0);
        CHECK(r >= 1 && r <= 3);
        if (r <= 0)
            break;
        got += r;
    }
    CHECK(got == 14 && memcmp(out, in, 14) == 0);

    rb_reset(&rb);
    CHECK(rb_count(&rb) == 0);
    rb_free(&rb);
    report("wraparound + partial reads", before);
}

static void test_timeout(void)
{
    int before = g_failures;
    RingBuf rb;
    CHECK(rb_init(&rb, 64) == 0);
    unsigned char out[8];
    CHECK(rb_read(&rb, out, sizeof(out), 0) == 0);
    long t0 = now_ms();
    CHECK(rb_read(&rb, out, sizeof(out), 80) == 0);
    long dt = now_ms() - t0;
    CHECK(dt >= 70 && dt < 500);
    rb_free(&rb);
    printf("  timeout waited %ld ms for an 80 ms timeout\n", dt);
    report("timeout returns 0", before);
}

typedef struct {
    RingBuf *rb;
    int      delay_ms;
} LateArg;

static void *late_writer(void *p)
{
    LateArg *a = p;
    unsigned char b[4] = {9, 8, 7, 6};
    usleep((useconds_t)a->delay_ms * 1000);
    rb_write(a->rb, b, sizeof(b));
    return NULL;
}

/* rb_read re-arms its wait from a monotonic reference rather than trusting the
 * wall-clock deadline it has to hand pthread_cond_timedwait. A clock step
 * cannot be staged from a test without root, so what is checked here is the
 * part that can be: the re-arm must not return a timeout early (the caller's
 * r == 0 path has no sleep, so that would spin a core) and must not swallow a
 * wakeup that arrives mid-wait. */
static void test_timeout_is_not_early(void)
{
    int before = g_failures;
    RingBuf rb;
    CHECK(rb_init(&rb, 64) == 0);
    unsigned char out[8];

    long t0 = now_ms();
    for (int i = 0; i < 4; i++)
        CHECK(rb_read(&rb, out, sizeof(out), 150) == 0);
    long dt = now_ms() - t0;
    CHECK(dt >= 560 && dt < 1500);

    LateArg la = {&rb, 100};
    pthread_t tl;
    pthread_create(&tl, NULL, late_writer, &la);
    long t1 = now_ms();
    long r = rb_read(&rb, out, sizeof(out), 1000);
    long dw = now_ms() - t1;
    pthread_join(tl, NULL);
    CHECK(r == 4);
    CHECK(dw >= 80 && dw < 500);

    rb_free(&rb);
    printf("  4 x 150 ms timeouts took %ld ms; a late write woke a 1000 ms read in %ld ms\n", dt, dw);
    report("timed reads never return early, and still wake on a late write", before);
}

static void test_close(void)
{
    int before = g_failures;
    RingBuf rb;
    CHECK(rb_init(&rb, 64) == 0);
    unsigned char in[5] = {1, 2, 3, 4, 5}, out[8];
    CHECK(rb_write(&rb, in, 5) == 0);
    rb_close(&rb);
    CHECK(rb_write(&rb, in, 1) == -1);
    CHECK(rb_read(&rb, out, 3, 100) == 3);
    CHECK(rb_read(&rb, out + 3, 8, -1) == 2);
    CHECK(memcmp(out, in, 5) == 0);
    CHECK(rb_read(&rb, out, 8, -1) == -1);
    CHECK(rb_read(&rb, out, 8, 0) == -1);

    rb_reset(&rb);   /* clears closed */
    CHECK(rb_write(&rb, in, 5) == 0);
    CHECK(rb_read(&rb, out, 8, 0) == 5);
    rb_free(&rb);
    report("close drains then -1; reset clears it", before);
}

typedef struct {
    RingBuf *rb;
    long     result;
    long     returned_at;
} BlockArg;

static void *blocked_writer(void *p)
{
    BlockArg *a = p;
    unsigned char big[64] = {0};
    a->result = rb_write(a->rb, big, sizeof(big));   /* cap 16: blocks */
    a->returned_at = now_ms();
    return NULL;
}

static void *blocked_reader(void *p)
{
    BlockArg *a = p;
    unsigned char out[8];
    a->result = rb_read(a->rb, out, sizeof(out), -1);   /* empty: blocks */
    a->returned_at = now_ms();
    return NULL;
}

static void test_abort(void)
{
    int before = g_failures;
    RingBuf rb;

    CHECK(rb_init(&rb, 16) == 0);
    BlockArg w = {&rb, 99, 0};
    pthread_t tw;
    pthread_create(&tw, NULL, blocked_writer, &w);
    usleep(50000);
    CHECK(w.returned_at == 0);   /* still blocked */
    long t_abort = now_ms();
    rb_abort(&rb);
    pthread_join(tw, NULL);
    long dw = w.returned_at - t_abort;
    CHECK(w.result == -1);
    CHECK(dw < 100);
    unsigned char out[4];
    CHECK(rb_read(&rb, out, 4, 0) == -1);   /* aborted: -1 even with data */
    rb_free(&rb);

    CHECK(rb_init(&rb, 16) == 0);
    BlockArg r = {&rb, 99, 0};
    pthread_t tr;
    pthread_create(&tr, NULL, blocked_reader, &r);
    usleep(50000);
    CHECK(r.returned_at == 0);
    t_abort = now_ms();
    rb_abort(&rb);
    pthread_join(tr, NULL);
    long dr = r.returned_at - t_abort;
    CHECK(r.result == -1);
    CHECK(dr < 100);
    rb_free(&rb);

    printf("  abort unblocked writer in %ld ms, reader in %ld ms\n", dw, dr);
    report("abort unblocks blocked writer and reader within 100 ms", before);
}

#define STRESS_TOTAL (8u * 1024u * 1024u)

static unsigned char pattern(size_t i)
{
    return (unsigned char)(((i * 2654435761u) >> 13) ^ i);
}

typedef struct {
    RingBuf *rb;
    size_t   total;
    int      errors;
} StressArg;

static void *producer(void *p)
{
    StressArg *a = p;
    unsigned int seed = 12345;
    unsigned char chunk[20000];
    size_t pos = 0;
    while (pos < STRESS_TOTAL) {
        size_t n = 1 + (size_t)(rand_r(&seed) % sizeof(chunk));
        if (n > STRESS_TOTAL - pos)
            n = STRESS_TOTAL - pos;
        for (size_t i = 0; i < n; i++)
            chunk[i] = pattern(pos + i);
        if (rb_write(a->rb, chunk, n) != 0) {
            a->errors++;
            break;
        }
        pos += n;
    }
    a->total = pos;
    rb_close(a->rb);
    return NULL;
}

static void *consumer(void *p)
{
    StressArg *a = p;
    unsigned int seed = 67890;
    unsigned char buf[15000];
    size_t pos = 0;
    for (;;) {
        size_t want = 1 + (size_t)(rand_r(&seed) % sizeof(buf));
        int timeout = (rand_r(&seed) % 2) ? -1 : 5;
        long r = rb_read(a->rb, buf, want, timeout);
        if (r < 0)
            break;
        if ((size_t)r > want) {
            a->errors++;
            break;
        }
        for (long i = 0; i < r; i++) {
            if (buf[i] != pattern(pos + (size_t)i)) {
                a->errors++;
                break;
            }
        }
        pos += (size_t)r;
        if (a->errors)
            break;
    }
    a->total = pos;
    return NULL;
}

static void test_stress(void)
{
    int before = g_failures;
    RingBuf rb;
    CHECK(rb_init(&rb, 4096) == 0);   /* smaller than the chunks: forces blocking both ways */
    StressArg pa = {&rb, 0, 0}, ca = {&rb, 0, 0};
    pthread_t tp, tc;
    long t0 = now_ms();
    pthread_create(&tc, NULL, consumer, &ca);
    pthread_create(&tp, NULL, producer, &pa);
    pthread_join(tp, NULL);
    pthread_join(tc, NULL);
    CHECK(pa.errors == 0);
    CHECK(ca.errors == 0);
    CHECK(pa.total == STRESS_TOTAL);
    CHECK(ca.total == STRESS_TOTAL);
    rb_free(&rb);
    printf("  stress: wrote %zu, read+verified %zu bytes in %ld ms\n", pa.total, ca.total, now_ms() - t0);
    report("two-thread 8 MB stress, sequence verified", before);
}

int main(void)
{
    test_wraparound_and_partial();
    test_timeout();
    test_timeout_is_not_early();
    test_close();
    test_abort();
    test_stress();
    printf("test_ringbuf: %d passed, %d failed\n", g_checks - g_failures, g_failures);
    return g_failures ? 1 : 0;
}
