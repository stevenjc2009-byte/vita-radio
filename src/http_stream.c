#include "http_stream.h"
#include "icy.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORKER_STACK_SIZE (256 * 1024)

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
    char       content_type[128];
    int        got_body;
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

    /* Every new status line (redirect hop) starts a fresh header set.
     * SHOUTcast v1 answers with "ICY 200 OK" instead of "HTTP/1.x". */
    if ((n >= 5 && memcmp(line, "HTTP/", 5) == 0) ||
        (n >= 4 && memcmp(line, "ICY ", 4) == 0)) {
        s->metaint = 0;
        s->content_type[0] = '\0';
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
        long v = strtol(val, NULL, 10);
        s->metaint = v > 0 ? v : 0;
    } else if ((size_t)(colon - line) == 12 && ci_equal_n(line, "content-type", 12)) {
        snprintf(s->content_type, sizeof(s->content_type), "%s", val);
    }
    return total;
}

static int icy_audio_cb(void *user, const unsigned char *data, size_t len)
{
    HttpStream *s = user;
    return rb_write(s->out, data, len) < 0 ? -1 : 0;
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
        if (s->on_headers)
            s->on_headers(s->user, s->content_type[0] ? s->content_type : NULL, code);
        if (code >= 400)
            return 0;
        icy_init(&s->icy, (size_t)s->metaint, icy_audio_cb, icy_title_cb, s);
    }

    if (s->http_status >= 400)
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
    CURLcode rc = curl_easy_perform(s->curl);

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
        if (s->http_status >= 400) {
            s->result = (int)s->http_status;
            snprintf(s->errmsg, sizeof(s->errmsg), "HTTP %ld", s->http_status);
        } else if (rc == CURLE_OK) {
            s->result = 0;
            s->errmsg[0] = '\0';
        } else {
            s->result = (int)rc;
            snprintf(s->errmsg, sizeof(s->errmsg), "%s",
                     s->errbuf[0] ? s->errbuf : curl_easy_strerror(rc));
        }
    }

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
