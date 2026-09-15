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
#include "http_stream.h"
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
#define SHUTDOWN_WAIT_MS 20000   /* > curl's 15 s connect timeout */

/* One connection: its own ring buffer, so an abandoned connection that is
 * still stuck in DNS/connect can be reaped in the background while the next
 * station starts on a fresh buffer. */
typedef struct {
    RingBuf     rb;
    HttpStream *http;
} Conn;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;       /* guards g_st, g_gen */
static pthread_mutex_t g_http_lock = PTHREAD_MUTEX_INITIALIZER;  /* guards g_http use */
static PlayerStatus    g_st;
static unsigned        g_gen;             /* bumped on every stop; stale callbacks are ignored */
static int             g_inited;
static Conn           *g_conn;            /* current connection (UI thread only) */
static RingBuf        *g_rb;              /* &g_conn->rb, read by the decode thread */
static atomic_int      g_reapers;         /* background stops still running */
static AudioOut       *g_audio;
static HttpStream     *g_http;
static pthread_t       g_thread;
static int             g_thread_running;
static Decoder        *g_dec;             /* owned by the decode thread until joined */
static atomic_int      g_stop;
static char            g_ua[128];
static char            g_ca[256];
static int             g_have_ca;

/* ---- helpers ---------------------------------------------------------- */

static void copy_str(char *dst, size_t dstsz, const char *src)
{
    if (!src)
        src = "";
    snprintf(dst, dstsz, "%s", src);
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
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

/* http_stream_finished on the live handle; treats a stopped/cleared handle as finished. */
static int stream_finished(void)
{
    int fin = 1;
    pthread_mutex_lock(&g_http_lock);
    if (g_http && !atomic_load(&g_stop))
        fin = http_stream_finished(g_http);
    pthread_mutex_unlock(&g_http_lock);
    return fin;
}

static int stream_result(char *err, size_t errsz)
{
    int res = 0;
    err[0] = '\0';
    pthread_mutex_lock(&g_http_lock);
    if (g_http && !atomic_load(&g_stop))
        res = http_stream_result(g_http, err, errsz);
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
        ctx->failed = 1;
        set_error("Audio output failed");
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

static void *decode_thread(void *arg)
{
    (void)arg;
    unsigned long consumed = 0;
    unsigned char *pre = NULL;
    unsigned char *buf = NULL;

    /* 1. pre-buffer */
    while (!atomic_load(&g_stop) && rb_count(g_rb) < PREBUFFER_BYTES && !stream_finished()) {
        update_progress(consumed);
        usleep(POLL_US);
    }
    if (atomic_load(&g_stop))
        goto out;

    pre = malloc(PREBUFFER_BYTES);
    buf = malloc(READ_CHUNK);
    if (!pre || !buf) {
        set_error("Out of memory");
        goto out;
    }
    size_t n = 0;
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
        set_error("HLS stations are not supported yet");
        goto out;
    case VR_BODY_PLS:
    case VR_BODY_M3U:
        set_error("Playlist link (.pls/.m3u) not supported yet");
        goto out;
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

    long last_data = now_ms();
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
    http_stream_stop(c->http);   /* may wait out a blocking DNS lookup or connect */
    rb_free(&c->rb);
    free(c);
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
    if (c->http) {
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
        http_stream_stop(c->http);   /* no thread available: stop inline */
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
    g_conn = c;
    g_rb = &c->rb;
    pthread_mutex_lock(&g_http_lock);
    g_http = h;
    pthread_mutex_unlock(&g_http_lock);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, THREAD_STACK);
    int rc = pthread_create(&g_thread, &attr, decode_thread, NULL);
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

    /* Detach the handle under g_http_lock so the decode thread can never touch
     * it after it is freed. */
    pthread_mutex_lock(&g_http_lock);
    g_http = NULL;
    pthread_mutex_unlock(&g_http_lock);

    Conn *c = g_conn;
    g_conn = NULL;
    if (c)
        rb_abort(&c->rb);   /* unblocks the decode thread's read and the worker's write */

    /* The decode thread only ever waits on the ring buffer, a 20 ms poll or one
     * audio grain, so this join is short. The network worker may be stuck in
     * DNS/connect for up to 15 s, so it is reaped in the background. */
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
    /* Let background stops finish before curl and the network are torn down. */
    long end = now_ms() + SHUTDOWN_WAIT_MS;
    while (atomic_load(&g_reapers) > 0 && now_ms() < end)
        usleep(POLL_US);
    audio_out_close(g_audio);
    g_audio = NULL;
    g_inited = 0;
}
