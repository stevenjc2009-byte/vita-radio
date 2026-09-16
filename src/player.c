#include "player.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "audio_out.h"
#include "decoder.h"
#include "hls.h"
#include "http_stream.h"
#include "playlist.h"
#include "ringbuf.h"
#include "sniff.h"

#define RB_CAPACITY      (256 * 1024)
#define PREBUFFER_BYTES  (64 * 1024)
#define REBUFFER_BYTES   (32 * 1024)
#define READ_CHUNK       8192
#define READ_TIMEOUT_MS  100
#define UNDERRUN_MS      1000
#define POLL_US          20000
#define THREAD_STACK     (256 * 1024)
#define REAPER_STACK     (32 * 1024)
#define MAX_REAPERS      4       /* concurrent background stops; see player_play */
#define SHUTDOWN_WAIT_MS 35000   /* covers http_get's fixed 30 s timeout; see player_shutdown */
#define MAX_SOURCE_HOPS  2       /* .pls -> .m3u8 -> media is the deepest real case */

/* One connection: its own ring buffer, so an abandoned connection that is
 * still stuck in DNS/connect can be reaped in the background while the next
 * station starts on a fresh buffer. */
typedef struct {
    RingBuf     rb;
    HttpStream *http;   /* exactly one of these two is set */
    HlsStream  *hls;
} Conn;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;       /* guards g_st, g_gen */
/* Guards the live source handles AND g_conn: the decode thread swaps the source
 * in place (switch_source) while the UI thread may be stopping, and the two must
 * not interleave. */
static pthread_mutex_t g_http_lock = PTHREAD_MUTEX_INITIALIZER;
static PlayerStatus    g_st;
static unsigned        g_gen;             /* bumped on every stop AND source swap; stale callbacks ignored */
static int             g_inited;
static Conn           *g_conn;            /* current connection (under g_http_lock) */
static RingBuf        *g_rb;              /* &g_conn->rb, read by the decode thread */
static atomic_int      g_reapers;         /* background stops still running */
static AudioOut       *g_audio;
static HttpStream     *g_http;
static HlsStream      *g_hls;             /* set instead of g_http for an HLS source */
static pthread_t       g_thread;
static int             g_thread_running;
static Decoder        *g_dec;             /* owned by the decode thread until joined */
static atomic_int      g_stop;
static char            g_ua[128];
static char            g_ca[256];
static int             g_have_ca;

static void conn_release(Conn *c);

/* ---- helpers ---------------------------------------------------------- */

static void copy_str(char *dst, size_t dstsz, const char *src)
{
    if (!src)
        src = "";
    snprintf(dst, dstsz, "%s", src);
}

/* int64 rather than long: vitasdk does not document whether CLOCK_MONOTONIC is
 * since-boot or epoch-based, and on a 32-bit long an epoch-based tv_sec would
 * overflow the * 1000 on the very first call, feeding garbage to the underrun
 * detector and the shutdown deadline. Widening removes the dependency on an
 * assumption nobody has checked. */
static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void set_error(const char *fmt, ...)
{
    if (atomic_load(&g_stop))
        return;
    pthread_mutex_lock(&g_lock);
    g_st.state = PLAYER_ERROR;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_st.error, sizeof(g_st.error), fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&g_lock);
}

static void set_state(PlayerState s)
{
    pthread_mutex_lock(&g_lock);
    g_st.state = s;
    pthread_mutex_unlock(&g_lock);
}

/* "Is the live source done?" over whichever kind is attached; treats a
 * stopped/cleared handle as finished. */
static int stream_finished(void)
{
    int fin = 1;
    pthread_mutex_lock(&g_http_lock);
    if (!atomic_load(&g_stop)) {
        if (g_http)      fin = http_stream_finished(g_http);
        else if (g_hls)  fin = hls_stream_finished(g_hls);
    }
    pthread_mutex_unlock(&g_http_lock);
    return fin;
}

static int stream_result(char *err, size_t errsz)
{
    int res = 0;
    err[0] = '\0';
    pthread_mutex_lock(&g_http_lock);
    if (!atomic_load(&g_stop)) {
        if (g_http)      res = http_stream_result(g_http, err, errsz);
        else if (g_hls)  res = hls_stream_result(g_hls, err, errsz);
    }
    pthread_mutex_unlock(&g_http_lock);
    return res;
}

static void update_progress(unsigned long consumed)
{
    size_t cnt = rb_count(g_rb);
    pthread_mutex_lock(&g_lock);
    g_st.buffer_pct = (unsigned)(cnt * 100 / RB_CAPACITY);
    g_st.bytes_received = consumed + (unsigned long)cnt;
    if (g_dec && g_st.state == PLAYER_PLAYING) {
        copy_str(g_st.codec, sizeof(g_st.codec), decoder_profile_name(g_dec));
        g_st.in_rate = decoder_input_rate(g_dec);
        g_st.in_channels = decoder_input_channels(g_dec);
    }
    pthread_mutex_unlock(&g_lock);
}

/* Waits for the stream to finish (after the ring buffer reported closed),
 * then reports its outcome as the error. */
static void report_stream_end(void)
{
    while (!atomic_load(&g_stop) && !stream_finished())
        usleep(POLL_US);
    if (atomic_load(&g_stop))
        return;
    char err[200];
    int res = stream_result(err, sizeof(err));
    if (res == 0 || err[0] == '\0')
        set_error("%s", res == 0 ? "Stream ended" : "Connection failed");
    else
        set_error("%s", err);
}

/* ---- http callbacks (network worker thread) --------------------------- */

static void on_headers(void *user, const char *content_type, long http_status)
{
    pthread_mutex_lock(&g_lock);
    if ((uintptr_t)user != g_gen) {   /* from an abandoned connection */
        pthread_mutex_unlock(&g_lock);
        return;
    }
    copy_str(g_st.content_type, sizeof(g_st.content_type), content_type);
    g_st.http_status = http_status;
    if (g_st.state == PLAYER_CONNECTING)
        g_st.state = PLAYER_BUFFERING;
    pthread_mutex_unlock(&g_lock);
}

static void on_title(void *user, const char *title)
{
    pthread_mutex_lock(&g_lock);
    if ((uintptr_t)user != g_gen) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    copy_str(g_st.title, sizeof(g_st.title), title);
    pthread_mutex_unlock(&g_lock);
}

/* ---- hls callbacks (hls worker thread) --------------------------------- */

static void on_hls_ready(void *user, VrCodec codec, long http_status)
{
    pthread_mutex_lock(&g_lock);
    if ((uintptr_t)user != g_gen) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    g_st.http_status = http_status;
    copy_str(g_st.content_type, sizeof(g_st.content_type),
             codec == VR_CODEC_AAC ? "HLS (AAC)" : "HLS (MPEG audio)");
    if (g_st.state == PLAYER_CONNECTING)
        g_st.state = PLAYER_BUFFERING;
    pthread_mutex_unlock(&g_lock);
}

/* HLS has no ICY title. The notes worth showing - chosen variant, a skipped
 * segment, a discontinuity - go in the same field so the panel stays useful. */
static void on_hls_note(void *user, const char *text)
{
    pthread_mutex_lock(&g_lock);
    if ((uintptr_t)user != g_gen) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    copy_str(g_st.title, sizeof(g_st.title), text);
    pthread_mutex_unlock(&g_lock);
}

/* ---- decode thread ----------------------------------------------------- */

typedef struct {
    int playing;   /* PCM has reached the audio port since the last (re)buffer */
    int failed;    /* audio output error already reported */
} PcmCtx;

static int on_pcm(void *user, const int16_t *pcm, int frames)
{
    PcmCtx *ctx = user;
    if (atomic_load(&g_stop))
        return -1;
    if (!ctx->playing) {
        ctx->playing = 1;
        pthread_mutex_lock(&g_lock);
        g_st.state = PLAYER_PLAYING;
        copy_str(g_st.codec, sizeof(g_st.codec), decoder_profile_name(g_dec));
        g_st.in_rate = decoder_input_rate(g_dec);
        g_st.in_channels = decoder_input_channels(g_dec);
        pthread_mutex_unlock(&g_lock);
    }
    if (audio_out_write(g_audio, pcm, frames) < 0) {
        if (!ctx->failed)          /* report it once, not once per grain */
            set_error("Audio output failed");
        ctx->failed = 1;
        return -1;
    }
    return 0;
}

/* Feeds bytes; returns 0 to keep going, -1 to leave the thread. */
static int feed(const unsigned char *buf, size_t len, PcmCtx *ctx)
{
    int r = decoder_feed(g_dec, buf, len, on_pcm, ctx);
    if (r == 0)
        return 0;
    if (r < 0)
        set_error("Decode failed (%s)", decoder_profile_name(g_dec));
    return -1;   /* r == 1: stop requested or audio failure already reported */
}

/* Replaces the connection's source, keeping the player running.
 *
 * A fresh Conn is used rather than reusing the current ring buffer: the old
 * worker may still be writing into it, and rb_reset would both interleave its
 * bytes and clear an abort that player_stop had just set. conn_release retires
 * the old one in the background, so a stuck DNS lookup cannot stall the switch.
 * Runs on the decode thread, under g_http_lock so it cannot interleave with
 * player_stop. Returns 0 with *pc repointed, or -1 (error already reported).  */
static int switch_source(Conn **pc, const char *url, int to_hls)
{
    Conn *old = *pc;
    Conn *nc;
    char target[sizeof(g_st.url)];
    unsigned gen;
    int ok;

    pthread_mutex_lock(&g_lock);
    if (url)
        copy_str(g_st.url, sizeof(g_st.url), url);
    copy_str(target, sizeof(target), g_st.url);
    g_st.state = PLAYER_CONNECTING;
    g_st.buffer_pct = 0;
    /* Everything describing the OLD body has to go. The classifier reads
     * g_st.content_type back for the new source, and the prebuffer loop can
     * exit before the new headers arrive (it also exits on stream_finished),
     * so a leftover "audio/x-scpls" would re-classify the new body as a
     * playlist and burn a hop on a redirection that is not there. */
    g_st.content_type[0] = '\0';
    g_st.http_status = 0;
    /* A new generation retires the old source's callbacks. It stays alive
     * until its reaper joins it, and sharing a generation would let its
     * on_title overwrite the new station's status line mid-swap. */
    gen = ++g_gen;
    pthread_mutex_unlock(&g_lock);

    nc = calloc(1, sizeof(*nc));
    if (!nc || rb_init(&nc->rb, RB_CAPACITY) != 0) {
        free(nc);
        set_error("Out of memory");
        return -1;
    }

    pthread_mutex_lock(&g_http_lock);
    if (atomic_load(&g_stop)) {         /* stopped while we were setting up */
        pthread_mutex_unlock(&g_http_lock);
        rb_free(&nc->rb);
        free(nc);
        return -1;
    }
    if (to_hls) {
        HlsStreamConfig hc;
        memset(&hc, 0, sizeof(hc));
        hc.url = target;
        hc.user_agent = g_ua;
        hc.ca_file = g_have_ca ? g_ca : NULL;
        hc.out = &nc->rb;
        hc.on_ready = on_hls_ready;
        hc.on_note = on_hls_note;
        hc.user = (void *)(uintptr_t)gen;
        nc->hls = hls_stream_start(&hc);
        ok = nc->hls != NULL;
    } else {
        HttpStreamConfig hc;
        memset(&hc, 0, sizeof(hc));
        hc.url = target;
        hc.user_agent = g_ua;
        hc.ca_file = g_have_ca ? g_ca : NULL;
        hc.out = &nc->rb;
        hc.on_headers = on_headers;
        hc.on_title = on_title;
        hc.user = (void *)(uintptr_t)gen;
        nc->http = http_stream_start(&hc);
        ok = nc->http != NULL;
    }
    if (ok) {
        g_conn = nc;
        g_rb = &nc->rb;
        g_http = nc->http;
        g_hls = nc->hls;
    }
    pthread_mutex_unlock(&g_http_lock);

    if (!ok) {
        rb_free(&nc->rb);
        free(nc);
        set_error(to_hls ? "Could not start HLS stream" : "Could not follow playlist");
        return -1;
    }
    conn_release(old);
    *pc = nc;
    return 0;
}

static void *decode_thread(void *arg)
{
    Conn *c = arg;
    unsigned long consumed = 0;
    unsigned char *pre = NULL;
    unsigned char *buf = NULL;
    int hops = 0;
    size_t n = 0;

    pre = malloc(PREBUFFER_BYTES);
    buf = malloc(READ_CHUNK);
    if (!pre || !buf) {
        set_error("Out of memory");
        goto out;
    }

open_source:
    /* 1. pre-buffer */
    while (!atomic_load(&g_stop) && rb_count(g_rb) < PREBUFFER_BYTES && !stream_finished()) {
        update_progress(consumed);
        usleep(POLL_US);
    }
    if (atomic_load(&g_stop))
        goto out;

    n = 0;
    while (n < PREBUFFER_BYTES) {
        long r = rb_read(g_rb, pre + n, PREBUFFER_BYTES - n, 0);
        if (r <= 0)
            break;
        n += (size_t)r;
    }
    if (atomic_load(&g_stop))
        goto out;
    consumed += n;
    update_progress(consumed);
    if (n == 0) {
        report_stream_end();
        goto out;
    }

    /* 2. classify */
    char ct[sizeof(g_st.content_type)];
    pthread_mutex_lock(&g_lock);
    memcpy(ct, g_st.content_type, sizeof(ct));
    pthread_mutex_unlock(&g_lock);
    const char *ctp = ct[0] ? ct : NULL;

    VrBodyKind kind = sniff_body_kind(ctp);
    if (kind == VR_BODY_AUDIO)
        kind = sniff_body_kind_from_bytes(pre, n);
    switch (kind) {
    case VR_BODY_HLS:
        if (hops++ >= MAX_SOURCE_HOPS) {
            set_error("Too many playlist redirections");
            goto out;
        }
        /* The bytes we pre-buffered are the playlist itself; the HLS worker
         * re-fetches it, so they are simply discarded with the old source. */
        if (switch_source(&c, NULL, 1) != 0)
            goto out;
        consumed = 0;
        goto open_source;
    case VR_BODY_PLS:
    case VR_BODY_M3U: {
        if (hops++ >= MAX_SOURCE_HOPS) {
            set_error("Too many playlist redirections");
            goto out;
        }
        char base[sizeof(g_st.url)];
        char target[sizeof(g_st.url)];
        pthread_mutex_lock(&g_lock);
        copy_str(base, sizeof(base), g_st.url);
        pthread_mutex_unlock(&g_lock);
        if (playlist_first_url((const char *)pre, n, base, target, sizeof(target)) != 0) {
            set_error("Playlist had no usable stream URL");
            goto out;
        }
        if (switch_source(&c, target, 0) != 0)
            goto out;
        consumed = 0;
        goto open_source;
    }
    case VR_BODY_TEXT:
        set_error("Not an audio stream (%s)", ct);
        goto out;
    case VR_BODY_AUDIO:
    default:
        break;
    }

    VrCodec codec = sniff_codec_from_content_type(ctp);
    if (codec == VR_CODEC_UNKNOWN)
        codec = sniff_codec_from_bytes(pre, n);
    if (codec == VR_CODEC_UNKNOWN) {
        set_error("Unknown audio format (%s)", ct);
        goto out;
    }

    Decoder *d = decoder_open(codec);
    if (!d) {
        set_error("Decoder init failed (%s)", codec == VR_CODEC_MP3 ? "MP3" : "AAC");
        goto out;
    }
    pthread_mutex_lock(&g_lock);
    g_dec = d;
    pthread_mutex_unlock(&g_lock);

    /* 3. play */
    PcmCtx ctx = {0};
    if (feed(pre, n, &ctx) < 0)
        goto out;
    free(pre);
    pre = NULL;

    int64_t last_data = now_ms();
    for (;;) {
        if (atomic_load(&g_stop))
            break;
        long r = rb_read(g_rb, buf, READ_CHUNK, READ_TIMEOUT_MS);
        if (r < 0) {
            if (!atomic_load(&g_stop))
                report_stream_end();
            break;
        }
        if (r > 0) {
            consumed += (unsigned long)r;
            last_data = now_ms();
            if (feed(buf, (size_t)r, &ctx) < 0)
                break;
        } else if (now_ms() - last_data > UNDERRUN_MS && !stream_finished()) {
            set_state(PLAYER_BUFFERING);
            audio_out_discard(g_audio);
            ctx.playing = 0;
            while (!atomic_load(&g_stop) && rb_count(g_rb) < REBUFFER_BYTES && !stream_finished()) {
                update_progress(consumed);
                usleep(POLL_US);
            }
            last_data = now_ms();
        }
        update_progress(consumed);
    }

out:
    /* Leaving for our own reason - a decode error, the end of the stream, a
     * failed swap - used to leave the connection installed. Its worker would
     * keep downloading until the 256 KB ring filled and then park in rb_write
     * with nothing reading it, pinning a thread, a socket and the buffer until
     * the user next pressed play or stop. The failed-swap path was worse: it
     * has already bumped g_gen and cleared content_type/http_status, so the
     * still-live old source's callbacks were being discarded and the reported
     * status no longer described the connection that was actually running.
     *
     * When g_stop is set the Conn belongs to player_stop, which takes it under
     * the same lock - so exactly one of us retires it, never both. */
    pthread_mutex_lock(&g_http_lock);
    Conn *dead = NULL;
    if (!atomic_load(&g_stop)) {
        dead = g_conn;
        g_conn = NULL;
        g_http = NULL;
        g_hls = NULL;
        g_rb = NULL;
    }
    pthread_mutex_unlock(&g_http_lock);
    conn_release(dead);   /* outside the lock: it may spawn a reaper */

    free(pre);
    free(buf);
    return NULL;
}

/* ---- public API -------------------------------------------------------- */

int player_init(const char *user_agent, const char *ca_file)
{
    if (g_inited)
        return 0;
    copy_str(g_ua, sizeof(g_ua), user_agent ? user_agent : "VitaRadio/0.1");
    g_have_ca = ca_file != NULL;
    copy_str(g_ca, sizeof(g_ca), ca_file);
    g_audio = audio_out_open();   /* NULL is tolerated; play reports it */
    memset(&g_st, 0, sizeof(g_st));
    atomic_store(&g_stop, 0);
    g_inited = 1;
    return 0;
}

static void *reap_thread(void *arg)
{
    Conn *c = arg;
    if (c->http)
        http_stream_stop(c->http);   /* may wait out a blocking DNS lookup or connect */
    if (c->hls)
        hls_stream_stop(c->hls);
    rb_free(&c->rb);
    free(c);
    /* Last statement on purpose: every resource this Conn owned is gone before
     * the count drops, so a shutdown that sees zero is not waiting on one of
     * them. It still is not proof the thread has terminated - it has its own
     * unwind to do after this - but by here it owns nothing and is no longer
     * inside libcurl. */
    atomic_fetch_sub(&g_reapers, 1);
    return NULL;
}

/* Frees a connection without making the caller wait for its worker: the
 * blocking http_stream_stop runs on a detached thread. */
static void conn_release(Conn *c)
{
    if (!c)
        return;
    rb_abort(&c->rb);
    if (c->http || c->hls) {
        pthread_attr_t attr;
        pthread_t t;
        atomic_fetch_add(&g_reapers, 1);
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, REAPER_STACK);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        int rc = pthread_create(&t, &attr, reap_thread, c);
        pthread_attr_destroy(&attr);
        if (rc == 0)
            return;
        atomic_fetch_sub(&g_reapers, 1);
        /* No thread available. Stopping inline is not an option here: this can
         * run on the UI thread (player_stop), and the blocking stop is the one
         * thing the reaper exists to keep off it - it would freeze the UI for
         * the whole DNS/connect time. The ring is already aborted, so the
         * worker's writes fail and it winds itself down; the Conn is leaked
         * deliberately rather than trading a leak for a frozen UI. Reaching
         * this at all needs the thread cap to be exhausted, which player_play
         * now bounds. */
        return;
    }
    rb_free(&c->rb);
    free(c);
}

void player_play(const char *url)
{
    if (!g_inited)
        return;
    player_stop();
    /* Clear the stop flag before any early exit, or set_error drops the message. */
    atomic_store(&g_stop, 0);

    pthread_mutex_lock(&g_lock);
    memset(&g_st, 0, sizeof(g_st));
    g_st.state = PLAYER_CONNECTING;
    copy_str(g_st.url, sizeof(g_st.url), url);
    unsigned gen = g_gen;
    pthread_mutex_unlock(&g_lock);

    if (!g_audio) {
        set_error("audio port open failed");
        return;
    }
    if (!url || !url[0]) {
        set_error("No URL");
        return;
    }
    /* Bound the fan-out. Every background stop still owns a curl worker, a
     * socket and a 256 KB ring, and against a dead host it lives for the whole
     * connect timeout - twenty rapid station changes would otherwise stack up
     * twenty of each. Refusing is deliberate: waiting here for room would put
     * the stall back on the UI thread, which is exactly what the reapers
     * exist to prevent. */
    if (atomic_load(&g_reapers) >= MAX_REAPERS) {
        set_error("Too many connections still closing");
        return;
    }

    Conn *c = calloc(1, sizeof(*c));
    if (!c || rb_init(&c->rb, RB_CAPACITY) != 0) {
        free(c);
        set_error("Out of memory");
        return;
    }

    HttpStreamConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.url = url;
    cfg.user_agent = g_ua;
    cfg.ca_file = g_have_ca ? g_ca : NULL;
    cfg.out = &c->rb;
    cfg.on_headers = on_headers;
    cfg.on_title = on_title;
    cfg.user = (void *)(uintptr_t)gen;

    HttpStream *h = http_stream_start(&cfg);
    if (!h) {
        rb_free(&c->rb);
        free(c);
        set_error("Could not start connection");
        return;
    }
    c->http = h;
    g_rb = &c->rb;
    pthread_mutex_lock(&g_http_lock);
    g_conn = c;
    g_http = h;
    g_hls = NULL;
    pthread_mutex_unlock(&g_http_lock);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, THREAD_STACK);
    int rc = pthread_create(&g_thread, &attr, decode_thread, c);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        player_stop();
        pthread_mutex_lock(&g_lock);
        g_st.state = PLAYER_ERROR;
        copy_str(g_st.error, sizeof(g_st.error), "Could not start decode thread");
        pthread_mutex_unlock(&g_lock);
        return;
    }
    g_thread_running = 1;
}

void player_stop(void)
{
    if (!g_inited)
        return;

    atomic_store(&g_stop, 1);

    /* Detach the handles and take the connection under g_http_lock: the decode
     * thread may be swapping the source right now (switch_source), and it holds
     * the same lock, so we either see the old Conn or the new one - never a
     * half-swapped pair, and never a Conn it is about to replace. */
    pthread_mutex_lock(&g_http_lock);
    g_http = NULL;
    g_hls = NULL;
    Conn *c = g_conn;
    g_conn = NULL;
    pthread_mutex_unlock(&g_http_lock);

    if (c)
        rb_abort(&c->rb);   /* unblocks the decode thread's read and the worker's write */

    /* The decode thread only ever waits on the ring buffer, a 20 ms poll or one
     * audio grain, so this join is short. The network worker has no such bound
     * - a synchronous DNS lookup ignores CURLOPT_CONNECTTIMEOUT entirely - so
     * it is reaped in the background instead of joined here. */
    if (g_thread_running) {
        pthread_join(g_thread, NULL);
        g_thread_running = 0;
    }

    audio_out_discard(g_audio);

    pthread_mutex_lock(&g_lock);
    Decoder *d = g_dec;
    g_dec = NULL;
    memset(&g_st, 0, sizeof(g_st));
    g_st.state = PLAYER_IDLE;
    g_gen++;   /* callbacks still in flight from c are now ignored */
    pthread_mutex_unlock(&g_lock);
    decoder_close(d);

    g_rb = NULL;
    conn_release(c);
}

void player_get_status(PlayerStatus *out)
{
    if (!out)
        return;
    pthread_mutex_lock(&g_lock);
    *out = g_st;
    pthread_mutex_unlock(&g_lock);
}

void player_shutdown(void)
{
    if (!g_inited)
        return;
    player_stop();
    /* Let background stops finish before curl and the network are torn down.
     *
     * This is a best-effort wait, NOT a guarantee. 35 s covers http_get's fixed
     * 30 s timeout, but the vendored resolver is synchronous, so
     * CURLOPT_CONNECTTIMEOUT does not bound a DNS lookup at all and there is no
     * abort hook to cut one short: a reaper can still outlive this deadline. If
     * it does, we return anyway and the caller tears down curl while a detached
     * thread is still inside it. Making that impossible needs the reapers to be
     * joinable, which is a redesign, not a constant. */
    int64_t end = now_ms() + SHUTDOWN_WAIT_MS;
    while (atomic_load(&g_reapers) > 0 && now_ms() < end)
        usleep(POLL_US);
    audio_out_close(g_audio);
    g_audio = NULL;
    g_inited = 0;
}
