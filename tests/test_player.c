/* Host test for player.c with the network, decoder and audio port stubbed.
 * Proves: switching/stopping never waits on a connection stuck in DNS or
 * connect; callbacks from an abandoned connection cannot touch the new
 * status; callbacks from the current one still land; early errors reach the
 * status; shutdown frees every connection. */
#include "audio_out.h"
#include "decoder.h"
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

    printf("test_player: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
