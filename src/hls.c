#include "hls.h"

#include "adts_scan.h"
#include "hls_crypt.h"
#include "http_get.h"
#include "m3u8.h"
#include "ts_demux.h"
#include "url_util.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define WORKER_STACK_SIZE  (256 * 1024)
#define URL_MAX            1024
#define ERR_MAX            256
#define PLAYLIST_MAX       (256 * 1024)
#define SEGMENT_MAX        (4 * 1024 * 1024)   /* a 30 s 320 kbps segment is ~1.2 MB */
#define SEG_INITIAL_CAP    (64 * 1024)
#define KEY_BYTES          16
#define KEY_CACHE_SLOTS    4
#define MAX_SEG_FAILS      5
#define MAX_RELOAD_FAILS   5
#define SLEEP_SLICE_US     100000              /* stop is noticed within 100 ms */
#define TARGET_MIN         1.0                 /* never poll faster than this */
#define TARGET_MAX         60.0
#define LIVE_EDGE_TARGETS  3.0                 /* RFC 8216 s6.3.3 */
#define TS_PACKET          188

/* Segment downloads use this module's own curl handle rather than http_get:
 * http_get has a fixed 30 s timeout and no abort hook, and a stop that waits out
 * a whole segment fetch hangs the UI. Playlists and keys are small and do go
 * through http_get, as the module contract asks. */

typedef struct {
    char          *uri;
    unsigned char  key[KEY_BYTES];
} KeyEntry;

typedef struct {
    unsigned char *data;
    size_t         len;
    size_t         cap;
    int            oom;
    int            too_large;
} SegBuf;

struct HlsStream {
    char       *url;
    char       *user_agent;
    char       *ca_file;
    RingBuf    *out;
    void      (*on_ready)(void *user, VrCodec codec, long http_status);
    void      (*on_note)(void *user, const char *text);
    void       *user;

    CURL       *curl;
    pthread_t   thread;
    atomic_int  stop;
    atomic_int  finished;
    atomic_int  segments_done;
    atomic_int  segments_failed;

    /* worker-owned until finished */
    SegBuf      seg;
    TsDemux    *ts;
    AdtsScan   *adts;
    int         last_was_ts;
    int         ready_sent;
    long        seg_status;
    KeyEntry    keys[KEY_CACHE_SLOTS];
    int         key_next;          /* round-robin eviction slot */
    int         result;
    char        errmsg[ERR_MAX];
    char        errbuf[CURL_ERROR_SIZE];
};

/* ---- helpers ------------------------------------------------------------- */

static char *dup_str(const char *s)
{
    size_t n;
    char *d;
    if (!s)
        return NULL;
    n = strlen(s) + 1;
    d = malloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

static void set_err(char *err, size_t errsz, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void set_err(char *err, size_t errsz, const char *fmt, ...)
{
    va_list ap;
    if (!err || errsz == 0)
        return;
    va_start(ap, fmt);
    vsnprintf(err, errsz, fmt, ap);
    va_end(ap);
}

static void note(HlsStream *s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void note(HlsStream *s, const char *fmt, ...)
{
    char text[ERR_MAX];
    va_list ap;
    if (!s->on_note)
        return;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    s->on_note(s->user, text);
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* Sleeps in slices so a stop between reloads is acted on promptly. */
static void nap(HlsStream *s, long ms)
{
    long waited;
    for (waited = 0; waited < ms; waited += SLEEP_SLICE_US / 1000) {
        if (atomic_load(&s->stop))
            return;
        usleep(SLEEP_SLICE_US);
    }
}

/* ---- segment buffer ------------------------------------------------------ */

static int seg_reserve(SegBuf *b, size_t n)
{
    size_t want = b->len + n;
    size_t cap = b->cap;
    unsigned char *d;

    if (want <= cap)
        return 0;
    if (cap == 0)
        cap = SEG_INITIAL_CAP;
    while (cap < want) {
        if (cap > SIZE_MAX / 2) {
            cap = want;
            break;
        }
        cap *= 2;
    }
    d = realloc(b->data, cap);
    if (!d)
        return -1;
    b->data = d;
    b->cap = cap;
    return 0;
}

/* ---- curl callbacks (worker thread) -------------------------------------- */

static size_t seg_write_cb(char *data, size_t size, size_t nmemb, void *userp)
{
    HlsStream *s = userp;
    size_t n = size * nmemb;

    if (n == 0 || atomic_load(&s->stop))
        return 0;
    /* A hostile or misdeclared server must not be able to grow this until the
     * Vita runs out of RAM. */
    if (n > SEGMENT_MAX || s->seg.len > SEGMENT_MAX - n) {
        s->seg.too_large = 1;
        return 0;
    }
    if (seg_reserve(&s->seg, n) != 0) {
        s->seg.oom = 1;
        return 0;
    }
    memcpy(s->seg.data + s->seg.len, data, n);
    s->seg.len += n;
    return n;
}

static int xferinfo_cb(void *userp, curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t ultotal, curl_off_t ulnow)
{
    HlsStream *s = userp;
    (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    return atomic_load(&s->stop) ? 1 : 0;
}

/* The one place elementary stream bytes reach the caller. Non-zero stops the
 * demuxer feed, which the caller then tells apart by checking stop. */
static int es_write_cb(void *user, const unsigned char *es, size_t len)
{
    HlsStream *s = user;
    if (atomic_load(&s->stop))
        return 1;
    return rb_write(s->out, es, len) < 0 ? 1 : 0;
}

/* ---- key cache ----------------------------------------------------------- */

/* Consecutive segments almost always share one EXT-X-KEY, so refetching per
 * segment would double the request count on a 6 s window. Four slots covers a
 * key rotation without the cache ever growing without bound. */

static const unsigned char *key_cache_find(HlsStream *s, const char *uri)
{
    int i;
    for (i = 0; i < KEY_CACHE_SLOTS; i++)
        if (s->keys[i].uri && strcmp(s->keys[i].uri, uri) == 0)
            return s->keys[i].key;
    return NULL;
}

static void key_cache_put(HlsStream *s, const char *uri, const unsigned char *key)
{
    KeyEntry *e = &s->keys[s->key_next];
    char *copy = dup_str(uri);

    if (!copy)
        return;   /* caching is an optimisation; losing it only costs a refetch */
    free(e->uri);
    e->uri = copy;
    memcpy(e->key, key, KEY_BYTES);
    s->key_next = (s->key_next + 1) % KEY_CACHE_SLOTS;
}

static int segment_key(HlsStream *s, const char *uri, unsigned char *key,
                       char *err, size_t errsz)
{
    const unsigned char *hit = key_cache_find(s, uri);
    HttpDoc doc;

    if (hit) {
        memcpy(key, hit, KEY_BYTES);
        return 0;
    }
    if (http_get(uri, s->user_agent, s->ca_file, 64, &doc, err, errsz) != 0)
        return -1;
    if (doc.len != KEY_BYTES) {
        set_err(err, errsz, "key is %lu bytes, expected %d",
                (unsigned long)doc.len, KEY_BYTES);
        http_doc_free(&doc);
        return -1;
    }
    memcpy(key, doc.data, KEY_BYTES);
    http_doc_free(&doc);
    key_cache_put(s, uri, key);
    return 0;
}

/* ---- playlists ----------------------------------------------------------- */

static int load_playlist(HlsStream *s, const char *url, M3u8 *out,
                         char *err, size_t errsz)
{
    HttpDoc doc;
    int rc;

    memset(out, 0, sizeof(*out));
    if (http_get(url, s->user_agent, s->ca_file, PLAYLIST_MAX, &doc, err, errsz) != 0)
        return -1;
    /* Resolve against the effective URL: a redirected playlist's relative
     * segment URIs belong to wherever it actually came from. */
    rc = m3u8_parse(doc.data, doc.len, doc.final_url, out);
    http_doc_free(&doc);
    if (rc != 0) {
        set_err(err, errsz, "not an M3U8 playlist");
        return -1;
    }
    return 0;
}

/* Lowest bandwidth, because this is a Vita on Wi-Fi. */
static int choose_variant(HlsStream *s, const M3u8 *master, const char *base,
                          char *out, size_t outsz, char *err, size_t errsz)
{
    int idx = m3u8_pick_variant(master);
    const M3u8Variant *v;

    if (idx < 0) {
        set_err(err, errsz, "master playlist has no variants");
        return -1;
    }
    v = &master->variants[idx];
    /* m3u8_parse already absolutised this; re-resolving a relative URI is a
     * boundary check, not a second implementation. */
    if (url_is_absolute(v->uri)) {
        if (snprintf(out, outsz, "%s", v->uri) >= (int)outsz) {
            set_err(err, errsz, "variant URL too long");
            return -1;
        }
    } else if (url_resolve(base, v->uri, out, outsz) != 0) {
        set_err(err, errsz, "could not resolve variant URL");
        return -1;
    }
    note(s, "Variant: %ld kbps%s%s", v->bandwidth / 1000,
         v->codecs[0] ? " " : "", v->codecs);
    return 0;
}

/* RFC 8216 s6.3.3: start three target durations back from the live edge, so
 * there is something to play before the next reload and the window does not
 * slide past us on the first stall. */
static int live_start_index(const M3u8 *pl)
{
    double back = 0.0;
    int i;

    if (pl->endlist)
        return 0;   /* not live: there is no edge to stay behind */
    for (i = pl->segment_count - 1; i > 0; i--) {
        back += pl->segments[i].duration;
        if (back >= LIVE_EDGE_TARGETS * pl->target_duration)
            break;
    }
    return i;
}

static double clamp_target(double target)
{
    if (!(target >= TARGET_MIN))   /* also catches 0 and NaN: never spin */
        return TARGET_MIN;
    return target > TARGET_MAX ? TARGET_MAX : target;
}

/* ---- segments ------------------------------------------------------------ */

static int looks_like_ts(const unsigned char *b, size_t len)
{
    /* One sync byte is a coincidence; three at 188-byte spacing is a transport
     * stream. Anything else is treated as a bare ADTS segment. */
    return len > 2 * TS_PACKET && b[0] == 0x47 &&
           b[TS_PACKET] == 0x47 && b[2 * TS_PACKET] == 0x47;
}

static int fetch_segment(HlsStream *s, const char *url, char *err, size_t errsz)
{
    CURLcode rc;
    long status = 0;

    s->seg.len = 0;
    s->seg.oom = 0;
    s->seg.too_large = 0;
    s->errbuf[0] = '\0';

    curl_easy_setopt(s->curl, CURLOPT_URL, url);
    rc = curl_easy_perform(s->curl);
    curl_easy_getinfo(s->curl, CURLINFO_RESPONSE_CODE, &status);
    s->seg_status = status;

    if (atomic_load(&s->stop)) {
        set_err(err, errsz, "stopped");
        return -1;
    }
    if (s->seg.too_large)
        set_err(err, errsz, "segment larger than %d bytes", SEGMENT_MAX);
    else if (s->seg.oom)
        set_err(err, errsz, "out of memory");
    else if (rc != CURLE_OK)
        set_err(err, errsz, "%s", s->errbuf[0] ? s->errbuf : curl_easy_strerror(rc));
    else if (status < 200 || status > 299)
        set_err(err, errsz, "HTTP %ld", status);
    else if (s->seg.len == 0)
        set_err(err, errsz, "empty segment");
    else
        return 0;
    return -1;
}

/* Returns 0, -1 for a segment worth skipping, -2 when the stream cannot go on. */
static int feed_segment(HlsStream *s, char *err, size_t errsz)
{
    int r;

    /* The container is sniffed per segment, but each handler is opened once and
     * kept for the life of the stream: continuity counters and a frame split
     * across a segment boundary have to survive it. Only a discontinuity
     * resets them. */
    if (looks_like_ts(s->seg.data, s->seg.len)) {
        if (!s->ts && !(s->ts = ts_demux_open())) {
            set_err(err, errsz, "out of memory");
            return -2;
        }
        s->last_was_ts = 1;
        r = ts_demux_feed(s->ts, s->seg.data, s->seg.len, es_write_cb, s);
    } else {
        if (!s->adts && !(s->adts = adts_scan_open())) {
            set_err(err, errsz, "out of memory");
            return -2;
        }
        s->last_was_ts = 0;
        r = adts_scan_feed(s->adts, s->seg.data, s->seg.len, es_write_cb, s);
    }

    if (r < 0) {
        set_err(err, errsz, "segment did not demux");
        return -1;
    }
    if (r > 0) {
        /* es_write_cb only asks to stop for a stop request or a dead sink. */
        if (!atomic_load(&s->stop))
            set_err(err, errsz, "output closed");
        return -2;
    }
    return 0;
}

static void announce_ready(HlsStream *s)
{
    VrCodec codec = VR_CODEC_UNKNOWN;

    if (s->ready_sent || !s->on_ready)
        return;
    if (s->last_was_ts) {
        switch (ts_demux_stream_type(s->ts)) {
        case 0x0F:
        case 0x11:
            codec = VR_CODEC_AAC;
            break;
        case 0x03:
        case 0x04:
            codec = VR_CODEC_MP3;
            break;
        default:
            return;   /* PMT not seen yet - ask again after the next segment */
        }
    } else {
        codec = VR_CODEC_AAC;   /* adts_scan emits nothing but ADTS frames */
    }
    s->ready_sent = 1;
    s->on_ready(s->user, codec, s->seg_status);
}

static int play_segment(HlsStream *s, const M3u8Segment *seg, char *err, size_t errsz)
{
    unsigned char key[KEY_BYTES];
    size_t len;
    int r;

    if (seg->discontinuity) {
        if (s->ts)
            ts_demux_reset(s->ts);
        if (s->adts)
            adts_scan_reset(s->adts);
        note(s, "Discontinuity at segment %lld", seg->seq);
    }

    /* m3u8.c marks a METHOD it cannot do (SAMPLE-AES) as encrypted with no key
     * URI. Refusing it beats writing noise into the decoder. */
    if (seg->encrypted && !seg->key_uri) {
        set_err(err, errsz, "unsupported encryption method");
        return -1;
    }

    if (fetch_segment(s, seg->uri, err, errsz) != 0)
        return atomic_load(&s->stop) ? -2 : -1;

    if (seg->encrypted) {
        if (segment_key(s, seg->key_uri, key, err, errsz) != 0)
            return -1;
        /* seg->iv is always usable: m3u8.c fills it from the media sequence
         * number when EXT-X-KEY carried no IV, by the same rule as
         * hls_iv_from_sequence. */
        len = s->seg.len;
        if (len == 0 || (len % KEY_BYTES) != 0) {
            set_err(err, errsz, "encrypted segment is %lu bytes, not a multiple of %d",
                    (unsigned long)len, KEY_BYTES);
            return -1;
        }
        if (hls_decrypt_segment(key, seg->iv, s->seg.data, &len, err, errsz) != 0)
            return -1;
        s->seg.len = len;
    }

    r = feed_segment(s, err, errsz);
    if (r != 0)
        return r;
    announce_ready(s);
    return 0;
}

/* ---- worker -------------------------------------------------------------- */

/* Plays every segment at or after *next_seq. Returns 0 to carry on, -1 when the
 * stream is over (err says why, empty for a clean stop). */
static int play_window(HlsStream *s, const M3u8 *pl, long long *next_seq,
                       int *seg_fails, char *err, size_t errsz)
{
    int i;

    for (i = 0; i < pl->segment_count; i++) {
        const M3u8Segment *seg = &pl->segments[i];
        int r;

        if (atomic_load(&s->stop)) {
            err[0] = '\0';
            return -1;
        }
        if (seg->seq < *next_seq)
            continue;

        r = play_segment(s, seg, err, errsz);
        *next_seq = seg->seq + 1;
        if (r == -2) {
            if (atomic_load(&s->stop))
                err[0] = '\0';
            return -1;
        }
        if (r != 0) {
            /* Live windows drop segments routinely; one bad segment is a gap in
             * the audio, not the end of the stream. */
            atomic_fetch_add(&s->segments_failed, 1);
            note(s, "Skipped segment %lld: %s", seg->seq, err);
            if (++*seg_fails > MAX_SEG_FAILS) {
                set_err(err, errsz, "%d segments in a row failed: %s",
                        *seg_fails, err);
                return -1;
            }
            continue;
        }
        *seg_fails = 0;
        atomic_fetch_add(&s->segments_done, 1);
    }
    return 0;
}

static void *worker(void *arg)
{
    HlsStream *s = arg;
    M3u8 pl;
    char media_url[URL_MAX];
    char err[ERR_MAX];
    long long next_seq = -1;
    int have_start = 0, seg_fails = 0, reload_fails = 0;

    err[0] = '\0';
    memset(&pl, 0, sizeof(pl));
    snprintf(media_url, sizeof(media_url), "%s", s->url);

    if (load_playlist(s, media_url, &pl, err, sizeof(err)) != 0)
        goto done;

    /* At most one master hop: a master that points at another master is a
     * broken feed, not a reason to recurse. */
    if (pl.kind == VR_M3U8_MASTER) {
        char master_url[URL_MAX];
        snprintf(master_url, sizeof(master_url), "%s", media_url);
        if (choose_variant(s, &pl, master_url, media_url, sizeof(media_url),
                           err, sizeof(err)) != 0)
            goto done;
        m3u8_free(&pl);
        if (load_playlist(s, media_url, &pl, err, sizeof(err)) != 0)
            goto done;
        if (pl.kind == VR_M3U8_MASTER) {
            set_err(err, sizeof(err), "master playlist points at another master");
            goto done;
        }
    }
    if (pl.kind != VR_M3U8_MEDIA || pl.segment_count == 0) {
        set_err(err, sizeof(err), "no playable segments in the playlist");
        goto done;
    }

    for (;;) {
        long long before = next_seq;
        double target;
        long started, wait_ms, spent;
        M3u8 next;

        if (!have_start) {
            next_seq = pl.segments[live_start_index(&pl)].seq;
            have_start = 1;
        } else if (next_seq < pl.segments[0].seq) {
            /* The window slid past us while we were downloading. Skipping to
             * its start loses audio, but chasing segments that no longer exist
             * loses the stream. */
            note(s, "Fell behind the live window, skipping to segment %lld",
                 pl.segments[0].seq);
            next_seq = pl.segments[0].seq;
        }

        started = now_ms();
        if (play_window(s, &pl, &next_seq, &seg_fails, err, sizeof(err)) != 0)
            goto done;
        if (atomic_load(&s->stop)) {
            err[0] = '\0';
            goto done;
        }
        if (pl.endlist) {
            err[0] = '\0';   /* VOD or a live stream that ended: a clean finish */
            goto done;
        }

        /* hls.h: about one target duration between reloads, half that after a
         * reload that brought nothing new (RFC 8216 s6.3.4). Time already spent
         * downloading counts towards the wait, so a slow segment does not push
         * us off the live edge. */
        target = clamp_target(pl.target_duration);
        wait_ms = (long)(target * (next_seq != before ? 1000.0 : 500.0));
        spent = now_ms() - started;
        if (spent < wait_ms)
            nap(s, wait_ms - spent);
        if (atomic_load(&s->stop)) {
            err[0] = '\0';
            goto done;
        }

        if (load_playlist(s, media_url, &next, err, sizeof(err)) == 0 &&
            next.kind == VR_M3U8_MEDIA && next.segment_count > 0) {
            m3u8_free(&pl);
            pl = next;
            reload_fails = 0;
        } else {
            m3u8_free(&next);
            if (++reload_fails > MAX_RELOAD_FAILS) {
                set_err(err, sizeof(err), "playlist reload failed %d times: %s",
                        reload_fails, err[0] ? err : "not a media playlist");
                goto done;
            }
            note(s, "Playlist reload failed (%d/%d), retrying",
                 reload_fails, MAX_RELOAD_FAILS);
        }
    }

done:
    m3u8_free(&pl);
    if (atomic_load(&s->stop) || err[0] == '\0') {
        s->result = 0;
        s->errmsg[0] = '\0';
    } else {
        s->result = -1;
        snprintf(s->errmsg, sizeof(s->errmsg), "%s", err);
    }
    rb_close(s->out);
    atomic_store(&s->finished, 1);
    return NULL;
}

/* ---- public -------------------------------------------------------------- */

static void free_stream(HlsStream *s)
{
    int i;
    if (s->curl)
        curl_easy_cleanup(s->curl);
    ts_demux_close(s->ts);
    adts_scan_close(s->adts);
    for (i = 0; i < KEY_CACHE_SLOTS; i++)
        free(s->keys[i].uri);
    free(s->seg.data);
    free(s->url);
    free(s->user_agent);
    free(s->ca_file);
    free(s);
}

HlsStream *hls_stream_start(const HlsStreamConfig *cfg)
{
    HlsStream *s;
    pthread_attr_t attr;
    CURL *c;
    int err;

    if (!cfg || !cfg->url || !cfg->out)
        return NULL;
    s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    atomic_init(&s->stop, 0);
    atomic_init(&s->finished, 0);
    atomic_init(&s->segments_done, 0);
    atomic_init(&s->segments_failed, 0);

    s->url        = dup_str(cfg->url);
    s->user_agent = dup_str(cfg->user_agent);
    s->ca_file    = dup_str(cfg->ca_file);
    s->out        = cfg->out;
    s->on_ready   = cfg->on_ready;
    s->on_note    = cfg->on_note;
    s->user       = cfg->user;
    if (!s->url || (cfg->user_agent && !s->user_agent) || (cfg->ca_file && !s->ca_file)) {
        free_stream(s);
        return NULL;
    }

    /* One handle reused for every segment, so the TLS session and connection
     * survive the whole stream instead of being rebuilt every few seconds. */
    s->curl = c = curl_easy_init();
    if (!c) {
        free_stream(s);
        return NULL;
    }
    curl_easy_setopt(c, CURLOPT_URL, s->url);
    if (s->user_agent)
        curl_easy_setopt(c, CURLOPT_USERAGENT, s->user_agent);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    /* No total timeout: segment sizes vary. A stalled transfer is caught by the
     * low-speed guard, and a stop by xferinfo_cb. */
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 20L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    if (s->ca_file)
        curl_easy_setopt(c, CURLOPT_CAINFO, s->ca_file);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, (char *)NULL);
    curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 16384L);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, s->errbuf);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, seg_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, s);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xferinfo_cb);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, s);

    if (pthread_attr_init(&attr) != 0) {
        free_stream(s);
        return NULL;
    }
    pthread_attr_setstacksize(&attr, WORKER_STACK_SIZE);
    err = pthread_create(&s->thread, &attr, worker, s);
    pthread_attr_destroy(&attr);
    if (err != 0) {
        free_stream(s);
        return NULL;
    }
    return s;
}

void hls_stream_stop(HlsStream *s)
{
    if (!s)
        return;
    /* Same three steps as http_stream_stop: the flag turns the next curl
     * progress tick and the next ring write into a failure, rb_abort unblocks a
     * write that is already waiting, then the worker is joined. */
    atomic_store(&s->stop, 1);
    rb_abort(s->out);
    pthread_join(s->thread, NULL);
    free_stream(s);
}

int hls_stream_finished(HlsStream *s)
{
    return s ? atomic_load(&s->finished) : 1;
}

int hls_stream_result(HlsStream *s, char *err, size_t errsz)
{
    if (!s) {
        if (err && errsz)
            snprintf(err, errsz, "no stream");
        return -1;
    }
    if (err && errsz)
        snprintf(err, errsz, "%s", s->errmsg);
    return s->result;
}

void hls_stream_stats(HlsStream *s, int *segments_done, int *segments_failed)
{
    if (segments_done)
        *segments_done = s ? atomic_load(&s->segments_done) : 0;
    if (segments_failed)
        *segments_failed = s ? atomic_load(&s->segments_failed) : 0;
}
