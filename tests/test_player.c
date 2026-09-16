/* Host test for player.c with the network, decoder and audio port stubbed.
 * Proves: switching/stopping never waits on a connection stuck in DNS or
 * connect; callbacks from an abandoned connection cannot touch the new
 * status; callbacks from the current one still land; early errors reach the
 * status; shutdown frees every connection. */
#include "audio_out.h"
#include "decoder.h"
#include "hls.h"
#include "http_stream.h"
#include "player.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int passed, failed;

static void check(int ok, const char *fmt, ...)
{
    if (ok) {
        passed++;
        return;
    }
    failed++;
    va_list ap;
    va_start(ap, fmt);
    printf("FAIL: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* ---- http_stream stub -------------------------------------------------- */

static atomic_int fake_block_ms;   /* how long the next connection "resolves" */
static atomic_int fake_live;       /* stub streams started and not yet freed */

/* Body the next http worker writes before closing, or NULL for none. This is
 * what lets a test drive the classifier: a playlist body here is what makes
 * player.c swap its source. */
static const char *fake_body;

/* Separate from fake_block_ms on purpose: the swap tests need the FIRST source
 * to answer instantly and the swapped-to source to hang, and one shared knob
 * cannot express that without racing the swap. */
static atomic_int fake_hls_block_ms;

struct HttpStream {
    HttpStreamConfig cfg;
    int              block_ms;
    pthread_t        thread;
    atomic_int       finished;
};

static void *fake_worker(void *arg)
{
    HttpStream *s = arg;
    usleep((useconds_t)s->block_ms * 1000);   /* like getaddrinfo: ignores stop */
    if (s->cfg.on_headers)
        s->cfg.on_headers(s->cfg.user, "audio/mpeg", 200);
    if (s->cfg.on_title)
        s->cfg.on_title(s->cfg.user, "title from worker");
    if (fake_body)
        rb_write(s->cfg.out, (const unsigned char *)fake_body, strlen(fake_body));
    rb_close(s->cfg.out);
    atomic_store(&s->finished, 1);
    return NULL;
}

HttpStream *http_stream_start(const HttpStreamConfig *cfg)
{
    HttpStream *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->cfg = *cfg;
    s->block_ms = atomic_load(&fake_block_ms);
    atomic_init(&s->finished, 0);
    if (pthread_create(&s->thread, NULL, fake_worker, s) != 0) {
        free(s);
        return NULL;
    }
    atomic_fetch_add(&fake_live, 1);
    return s;
}

void http_stream_stop(HttpStream *s)
{
    if (!s)
        return;
    rb_abort(s->cfg.out);
    pthread_join(s->thread, NULL);
    free(s);
    atomic_fetch_sub(&fake_live, 1);
}

int http_stream_finished(HttpStream *s)
{
    return s ? atomic_load(&s->finished) : 1;
}

int http_stream_result(HttpStream *s, char *err, size_t errsz)
{
    (void)s;
    if (err && errsz)
        err[0] = '\0';
    return 0;
}

/* ---- hls stub ----------------------------------------------------------- */

/* hls.c pulls in libcurl, so it is stubbed here exactly as http_stream is.
 * playlist.c is pure C and player.c links the real one. This stub is shaped
 * like the http one on purpose: an HLS source must be just as abandonable as
 * an HTTP one, and it shares fake_live so the shutdown leak check covers both. */

struct HlsStream {
    HlsStreamConfig cfg;
    int             block_ms;
    pthread_t       thread;
    atomic_int      finished;
};

static void *fake_hls_worker(void *arg)
{
    HlsStream *s = arg;
    usleep((useconds_t)s->block_ms * 1000);   /* like a stuck segment fetch */
    if (s->cfg.on_ready)
        s->cfg.on_ready(s->cfg.user, VR_CODEC_AAC, 200);
    if (s->cfg.on_note)
        s->cfg.on_note(s->cfg.user, "variant 128k");
    rb_close(s->cfg.out);
    atomic_store(&s->finished, 1);
    return NULL;
}

HlsStream *hls_stream_start(const HlsStreamConfig *cfg)
{
    HlsStream *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->cfg = *cfg;
    s->block_ms = atomic_load(&fake_hls_block_ms);
    atomic_init(&s->finished, 0);
    if (pthread_create(&s->thread, NULL, fake_hls_worker, s) != 0) {
        free(s);
        return NULL;
    }
    atomic_fetch_add(&fake_live, 1);
    return s;
}

void hls_stream_stop(HlsStream *s)
{
    if (!s)
        return;
    rb_abort(s->cfg.out);
    pthread_join(s->thread, NULL);
    free(s);
    atomic_fetch_sub(&fake_live, 1);
}

int hls_stream_finished(HlsStream *s)
{
    return s ? atomic_load(&s->finished) : 1;
}

int hls_stream_result(HlsStream *s, char *err, size_t errsz)
{
    (void)s;
    if (err && errsz)
        err[0] = '\0';
    return 0;
}

/* ---- decoder + audio stubs (no audio bytes ever arrive) ----------------- */

Decoder *decoder_open(VrCodec codec) { (void)codec; return (Decoder *)malloc(1); }
void decoder_close(Decoder *d) { free(d); }
int decoder_feed(Decoder *d, const unsigned char *data, size_t len, PcmFn fn, void *user)
{
    (void)d; (void)data; (void)len; (void)fn; (void)user;
    return 0;
}
int decoder_input_rate(const Decoder *d) { (void)d; return 44100; }
int decoder_input_channels(const Decoder *d) { (void)d; return 2; }
const char *decoder_profile_name(const Decoder *d) { (void)d; return "MP3"; }

static int  fake_audio_fail;
static char fake_port;
AudioOut *audio_out_open(void) { return fake_audio_fail ? NULL : (AudioOut *)&fake_port; }
int audio_out_write(AudioOut *a, const int16_t *pcm, int frames) { (void)a; (void)pcm; (void)frames; return 0; }
void audio_out_discard(AudioOut *a) { (void)a; }
void audio_out_set_volume(AudioOut *a, int volume) { (void)a; (void)volume; }
void audio_out_close(AudioOut *a) { (void)a; }

/* ---- tests ------------------------------------------------------------- */

static void wait_for_state(PlayerState want, int timeout_ms, PlayerStatus *st)
{
    long end = now_ms() + timeout_ms;
    do {
        player_get_status(st);
        if (st->state == want)
            return;
        usleep(10 * 1000);
    } while (now_ms() < end);
}

/* Reapers are detached, so the count they drive drops some time after the call
 * that spawned them returned. */
static void wait_for_live(int want, int timeout_ms)
{
    long end = now_ms() + timeout_ms;
    while (atomic_load(&fake_live) > want && now_ms() < end)
        usleep(10 * 1000);
}

int main(void)
{
    PlayerStatus st;
    long t0, dt;

    check(player_init("test", NULL) == 0, "player_init");

    /* 1. leaving a connection stuck for 2 s returns at once */
    atomic_store(&fake_block_ms, 2000);
    player_play("http://a/");
    t0 = now_ms();
    player_play("http://b/");
    dt = now_ms() - t0;
    check(dt < 300, "switch away from a blocked connection took %ld ms (want < 300)", dt);
    t0 = now_ms();
    player_stop();
    dt = now_ms() - t0;
    check(dt < 300, "stop on a blocked connection took %ld ms (want < 300)", dt);
    player_get_status(&st);
    check(st.state == PLAYER_IDLE, "after stop: state %d (want IDLE)", st.state);

    /* 2. an abandoned connection's late callbacks do not reach the new status */
    atomic_store(&fake_block_ms, 300);
    player_play("http://old/");
    atomic_store(&fake_block_ms, 5000);
    player_play("http://new/");
    usleep(800 * 1000);   /* the old worker has called back by now */
    player_get_status(&st);
    check(st.state == PLAYER_CONNECTING, "stale: state %d (want CONNECTING)", st.state);
    check(strcmp(st.url, "http://new/") == 0, "stale: url '%s'", st.url);
    check(st.content_type[0] == '\0' && st.title[0] == '\0' && st.http_status == 0,
          "stale callback leaked: content_type '%s' title '%s' http %ld",
          st.content_type, st.title, st.http_status);

    /* 3. the current connection's callbacks and end-of-stream still land */
    atomic_store(&fake_block_ms, 50);
    player_play("http://c/");
    wait_for_state(PLAYER_ERROR, 3000, &st);
    check(st.state == PLAYER_ERROR && strcmp(st.error, "Stream ended") == 0,
          "own end: state %d error '%s' (want ERROR 'Stream ended')", st.state, st.error);
    check(st.http_status == 200 && strcmp(st.content_type, "audio/mpeg") == 0 &&
          strcmp(st.title, "title from worker") == 0,
          "own callbacks: http %ld content_type '%s' title '%s'",
          st.http_status, st.content_type, st.title);

    /* 4. an early error is reported, not dropped */
    player_play("");
    player_get_status(&st);
    check(st.state == PLAYER_ERROR && strcmp(st.error, "No URL") == 0,
          "empty url: state %d error '%s' (want ERROR 'No URL')", st.state, st.error);

    /* 5. rapid switching stays fast, and shutdown frees every connection */
    atomic_store(&fake_block_ms, 400);
    t0 = now_ms();
    for (int i = 0; i < 20; i++)
        player_play(i % 2 ? "http://x/" : "http://y/");
    dt = now_ms() - t0;
    check(dt < 1000, "20 switches took %ld ms (want < 1000)", dt);
    player_shutdown();
    check(atomic_load(&fake_live) == 0, "%d connections still alive after shutdown",
          atomic_load(&fake_live));

    /* 6. a missing audio port is reported */
    fake_audio_fail = 1;
    check(player_init("test", NULL) == 0, "player_init without audio");
    player_play("http://d/");
    player_get_status(&st);
    check(st.state == PLAYER_ERROR && strcmp(st.error, "audio port open failed") == 0,
          "no audio: state %d error '%s'", st.state, st.error);
    player_shutdown();

    /* ---- source swaps (v2.0.0: HLS and playlist support) ---------------- */

    fake_audio_fail = 0;
    check(player_init("test", NULL) == 0, "player_init for the swap tests");

    /* 7. an HLS body swaps the source to the HLS worker.
     * The HLS stub writes no audio, so the run still ends in "Stream ended" -
     * but it can only reach that state THROUGH the swap, and content_type and
     * title here can only have been set by the HLS source's own callbacks. */
    atomic_store(&fake_block_ms, 0);
    atomic_store(&fake_hls_block_ms, 0);
    fake_body = "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:6\nseg0.ts\n";
    player_play("http://station/live.m3u8");
    wait_for_state(PLAYER_ERROR, 3000, &st);
    check(strcmp(st.content_type, "HLS (AAC)") == 0,
          "hls swap: content_type '%s' (want 'HLS (AAC)')", st.content_type);
    check(strcmp(st.title, "variant 128k") == 0,
          "hls swap: title '%s' (want 'variant 128k')", st.title);
    player_stop();

    /* 8. a .pls body is parsed by the REAL playlist.c and the URL follows it.
     * The stub keeps serving the same body, so this also walks into the
     * MAX_SOURCE_HOPS bound rather than looping forever - which is the point. */
    fake_body = "[playlist]\nNumberOfEntries=1\nFile1=http://real/stream.mp3\n";
    player_play("http://station/listen.pls");
    wait_for_state(PLAYER_ERROR, 3000, &st);
    check(strcmp(st.url, "http://real/stream.mp3") == 0,
          "pls swap: url '%s' (want 'http://real/stream.mp3')", st.url);
    check(strcmp(st.error, "Too many playlist redirections") == 0,
          "pls hop bound: error '%s' (want 'Too many playlist redirections')", st.error);
    player_stop();

    /* 9. stopping while the SWAPPED-TO source is stuck must not wait it out.
     * This is the interleaving the concurrency audit flagged for hardware:
     * the decode thread has just republished the connection when the UI
     * thread stops it. */
    atomic_store(&fake_block_ms, 0);
    atomic_store(&fake_hls_block_ms, 1500);   /* the HLS source hangs */
    fake_body = "#EXTM3U\n#EXT-X-TARGETDURATION:6\nseg0.ts\n";
    player_play("http://station/stuck.m3u8");
    usleep(400 * 1000);                       /* the swap has happened by now */
    t0 = now_ms();
    player_stop();
    dt = now_ms() - t0;
    check(dt < 300, "stop during an HLS swap took %ld ms (want < 300)", dt);

    fake_body = NULL;
    player_shutdown();
    check(atomic_load(&fake_live) == 0, "%d streams still alive after swap shutdown",
          atomic_load(&fake_live));

    /* ---- connection lifetime when nobody stops the player ---------------- */

    check(player_init("test", NULL) == 0, "player_init for the lifetime tests");

    /* 10. a decode thread that leaves for its own reason retires its
     * connection. Without that the source stays installed after the error
     * toast: its worker keeps downloading until the 256 KB ring fills and then
     * parks in rb_write forever, pinning a thread, a socket and the buffer
     * until the user happens to press play or stop again. */
    atomic_store(&fake_block_ms, 0);
    fake_body = NULL;                 /* no body: the run ends in "Stream ended" */
    player_play("http://selfend/");
    wait_for_state(PLAYER_ERROR, 3000, &st);
    check(st.state == PLAYER_ERROR, "self-end: state %d (want ERROR)", st.state);
    wait_for_live(0, 2000);
    check(atomic_load(&fake_live) == 0,
          "self-end: %d connections still live after the error, with no stop",
          atomic_load(&fake_live));
    player_stop();
    wait_for_live(0, 2000);

    /* 11. the reaper fan-out is capped. Rapid switches against hosts that will
     * not answer must not stack up one worker, one socket and one 256 KB ring
     * per abandoned station; past the cap a new connection is refused rather
     * than started, because waiting for room would put the stall back on the
     * UI thread. */
    atomic_store(&fake_block_ms, 1500);
    int peak = 0;
    for (int i = 0; i < 12; i++) {
        player_play("http://dead/");
        int live = atomic_load(&fake_live);
        if (live > peak)
            peak = live;
    }
    check(peak <= 5, "reaper fan-out peaked at %d live connections (want <= 5)", peak);
    player_shutdown();
    check(atomic_load(&fake_live) == 0, "%d connections still alive after the cap test",
          atomic_load(&fake_live));

    printf("test_player: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
