#include "http_stream.h"
#include "icy.h"

#include <curl/curl.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORKER_STACK_SIZE (256 * 1024)

/* Sane band for icy-metaint. Real stations use 8192-65536; anything outside
 * this is a broken header, not a block size we should trust. */
#define ICY_METAINT_MAX   (1024 * 1024)

struct HttpStream {
    char       *url;
    char       *user_agent;
    char       *ca_file;
    RingBuf    *out;
    void      (*on_headers)(void *user, const char *content_type, long http_status);
    void      (*on_title)(void *user, const char *title);
    void       *user;

    CURL              *curl;
    struct curl_slist *headers;
    pthread_t          thread;
    atomic_int         stop;
    atomic_int         finished;

    /* worker-owned until finished */
    long       metaint;
    int        metaint_bad;        /* header was present but unusable */
    char       metaint_raw[32];    /* the rejected value, for the notice */
    char       content_type[128];
    int        got_body;
    size_t     audio_bytes;        /* audio handed to the ring buffer so far */
    long       http_status;
    int        result;
    char       errmsg[CURL_ERROR_SIZE + 16];
    char       errbuf[CURL_ERROR_SIZE];
    IcyParser  icy;
};

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

static int ci_equal_n(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        int ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb)
            return 0;
    }
    return 1;
}

static size_t header_cb(char *data, size_t size, size_t nmemb, void *userp)
{
    HttpStream *s = userp;
    size_t total = size * nmemb;
    char line[512];
    size_t n = total < sizeof(line) - 1 ? total : sizeof(line) - 1;
    char *colon, *val, *end;

    memcpy(line, data, n);
    line[n] = '\0';
    while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == '\n'))
        line[--n] = '\0';

    /* Every new status line (redirect hop) starts a fresh header set, so the
     * body latch has to re-arm with it - libcurl hands us the body of each
     * redirect hop too, and only the last hop is the real response.
     * SHOUTcast v1 answers with "ICY 200 OK" instead of "HTTP/1.x". */
    if ((n >= 5 && memcmp(line, "HTTP/", 5) == 0) ||
        (n >= 4 && memcmp(line, "ICY ", 4) == 0)) {
        s->metaint = 0;
        s->metaint_bad = 0;
        s->metaint_raw[0] = '\0';
        s->content_type[0] = '\0';
        s->got_body = 0;
        return total;
    }

    colon = strchr(line, ':');
    if (!colon)
        return total;
    val = colon + 1;
    while (*val == ' ' || *val == '\t')
        val++;
    end = val + strlen(val);
    while (end > val && (end[-1] == ' ' || end[-1] == '\t'))
        *--end = '\0';

    if ((size_t)(colon - line) == 11 && ci_equal_n(line, "icy-metaint", 11)) {
        char *ep = val;
        long v;
        errno = 0;
        v = strtol(val, &ep, 10);
        /* A garbled or absurd value must not quietly become "no metadata":
         * that feeds every metadata block to the decoder as audio. Keep 0 so
         * the parser stays off, but record it so the notice can say why. */
        if (ep == val || *ep != '\0' || errno == ERANGE ||
            v <= 0 || v > ICY_METAINT_MAX) {
            s->metaint = 0;
            s->metaint_bad = 1;
            snprintf(s->metaint_raw, sizeof(s->metaint_raw), "%s", val);
        } else {
            s->metaint = v;
        }
    } else if ((size_t)(colon - line) == 12 && ci_equal_n(line, "content-type", 12)) {
        snprintf(s->content_type, sizeof(s->content_type), "%s", val);
    }
    return total;
}

/* Only a 2xx carries a stream, and 204/205 are defined to carry no body at all.
 * Everything else is an error page, an un-followable redirect, or nothing. */
static int status_is_streamable(long code)
{
    return code >= 200 && code <= 299 && code != 204 && code != 205;
}

static int icy_audio_cb(void *user, const unsigned char *data, size_t len)
{
    HttpStream *s = user;
    if (rb_write(s->out, data, len) < 0)
        return -1;
    s->audio_bytes += len;
    return 0;
}

static void icy_title_cb(void *user, const char *title)
{
    HttpStream *s = user;
    if (s->on_title)
        s->on_title(s->user, title);
}

static size_t write_cb(char *data, size_t size, size_t nmemb, void *userp)
{
    HttpStream *s = userp;
    size_t total = size * nmemb;

    if (atomic_load(&s->stop))
        return 0;

    if (!s->got_body) {
        long code = 0;
        s->got_body = 1;
        curl_easy_getinfo(s->curl, CURLINFO_RESPONSE_CODE, &code);
        s->http_status = code;
        /* A hop we are being redirected away from: its body is neither audio
         * nor the final answer, so swallow it rather than abort the transfer.
         * header_cb re-arms the latch when the next status line arrives. */
        if (code >= 300 && code <= 399)
            return total;
        if (s->on_headers)
            s->on_headers(s->user, s->content_type[0] ? s->content_type : NULL, code);
        /* A 1xx, a 204/205, a 3xx we could not follow or no status line at all
         * must end the transfer - otherwise the response body is fed to the
         * decoder as audio while the UI shows a playing station. */
        if (!status_is_streamable(code))
            return 0;
        icy_init(&s->icy, (size_t)s->metaint, icy_audio_cb, icy_title_cb, s);
    }

    if (s->http_status >= 300 && s->http_status <= 399)
        return total;
    if (!status_is_streamable(s->http_status))
        return 0;
    if (icy_feed(&s->icy, (const unsigned char *)data, total) < 0)
        return 0;
    if (atomic_load(&s->stop))
        return 0;
    return total;
}

static int xferinfo_cb(void *userp, curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t ultotal, curl_off_t ulnow)
{
    HttpStream *s = userp;
    (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    return atomic_load(&s->stop) ? 1 : 0;
}

static void *worker(void *arg)
{
    HttpStream *s = arg;
    CURLcode rc;

    /* No CA bundle is a configuration fault, not a network one: curl's
     * compile-time default path does not exist on the Vita, so every https
     * station would fail with an opaque TLS error. Say what is actually wrong. */
    if (!s->ca_file) {
        s->result = -1;
        snprintf(s->errmsg, sizeof(s->errmsg), "no CA bundle configured");
        rb_close(s->out);
        atomic_store(&s->finished, 1);
        return NULL;
    }

    rc = curl_easy_perform(s->curl);

    if (atomic_load(&s->stop)) {
        s->result = 0;
        s->errmsg[0] = '\0';
    } else {
        if (!s->got_body) {
            long code = 0;
            curl_easy_getinfo(s->curl, CURLINFO_RESPONSE_CODE, &code);
            s->http_status = code;
            /* No body arrived (e.g. an empty 404): still report what we got. */
            if (code > 0 && s->on_headers)
                s->on_headers(s->user, s->content_type[0] ? s->content_type : NULL, code);
        }
        /* Anything but a 2xx is a failure: a 1xx, a 204/205 or a 3xx we could
         * not follow all end with no stream, and reporting them as a clean end
         * shows a playing station that is silent. The status is checked before
         * rc because refusing the body above makes rc CURLE_WRITE_ERROR, which
         * would hide the far more useful HTTP code. */
        if (s->http_status > 0 && !status_is_streamable(s->http_status)) {
            s->result = (int)s->http_status;
            snprintf(s->errmsg, sizeof(s->errmsg), "HTTP %ld", s->http_status);
        } else if (rc != CURLE_OK) {
            s->result = (int)rc;
            snprintf(s->errmsg, sizeof(s->errmsg), "%s",
                     s->errbuf[0] ? s->errbuf : curl_easy_strerror(rc));
        } else if (s->http_status == 0) {
            /* curl is happy but no status line ever arrived. */
            s->result = -1;
            snprintf(s->errmsg, sizeof(s->errmsg), "no HTTP response");
        } else if (s->audio_bytes == 0) {
            /* A 2xx that ended without handing over a single audio byte: the
             * station connected and played nothing, so calling it a clean end
             * shows a working station that is silent. Every transport failure
             * is caught above, which keeps "never connected" distinct from
             * "connected and sent nothing". */
            s->result = -1;
            snprintf(s->errmsg, sizeof(s->errmsg),
                     "HTTP %ld, no audio received", s->http_status);
        } else {
            s->result = 0;
            s->errmsg[0] = '\0';
        }
    }

    /* Non-fatal, but the session played with metadata left in the audio, so
     * say so instead of letting it look like a station with no metadata. */
    if (s->result == 0 && s->metaint_bad && s->errmsg[0] == '\0')
        snprintf(s->errmsg, sizeof(s->errmsg), "ignored bad icy-metaint \"%s\"",
                 s->metaint_raw);

    rb_close(s->out);
    atomic_store(&s->finished, 1);
    return NULL;
}

static void free_stream(HttpStream *s)
{
    if (s->curl)
        curl_easy_cleanup(s->curl);
    curl_slist_free_all(s->headers);
    free(s->url);
    free(s->user_agent);
    free(s->ca_file);
    free(s);
}

HttpStream *http_stream_start(const HttpStreamConfig *cfg)
{
    HttpStream *s;
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

    s->url        = dup_str(cfg->url);
    s->user_agent = dup_str(cfg->user_agent);
    s->ca_file    = dup_str(cfg->ca_file);
    s->out        = cfg->out;
    s->on_headers = cfg->on_headers;
    s->on_title   = cfg->on_title;
    s->user       = cfg->user;
    if (!s->url || (cfg->user_agent && !s->user_agent) || (cfg->ca_file && !s->ca_file)) {
        free_stream(s);
        return NULL;
    }

    s->headers = curl_slist_append(NULL, "Icy-MetaData: 1");
    s->curl = c = curl_easy_init();
    if (!s->headers || !c) {
        free_stream(s);
        return NULL;
    }

    curl_easy_setopt(c, CURLOPT_URL, s->url);
    if (s->user_agent)
        curl_easy_setopt(c, CURLOPT_USERAGENT, s->user_agent);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, s->headers);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    /* Follow redirects, but never let an https station be walked down to
     * plaintext http by a Location header. */
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR,
                     (strlen(s->url) >= 8 && ci_equal_n(s->url, "https://", 8))
                         ? "https" : "http,https");
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
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
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, s);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
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

void http_stream_stop(HttpStream *s)
{
    if (!s)
        return;
    atomic_store(&s->stop, 1);
    rb_abort(s->out);
    pthread_join(s->thread, NULL);
    free_stream(s);
}

int http_stream_finished(HttpStream *s)
{
    return s ? atomic_load(&s->finished) : 1;
}

int http_stream_result(HttpStream *s, char *err, size_t errsz)
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
