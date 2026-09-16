#include "http_get.h"

#include <curl/curl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The Vita's Wi-Fi is slow: give the handshake room, but never let a stalled
 * server hold the HLS worker or the UI thread for longer than this. */
#define GET_CONNECT_TIMEOUT 15L
#define GET_TOTAL_TIMEOUT   30L
#define GET_INITIAL_CAP     8192

typedef struct {
    char   *data;
    size_t  len;
    size_t  cap;        /* allocated bytes - always leaves room for the NUL */
    size_t  max;        /* body bytes allowed before we abort the transfer */
    int     too_large;
    int     oom;
} MemBuf;

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
{
    va_list ap;
    if (!err || errsz == 0)
        return;
    va_start(ap, fmt);
    vsnprintf(err, errsz, fmt, ap);
    va_end(ap);
}

/* Make room for n more body bytes plus the terminator. Returns 0 on success. */
static int mem_reserve(MemBuf *m, size_t n)
{
    size_t want = m->len + n + 1;
    size_t cap = m->cap;
    char *d;

    if (want <= cap)
        return 0;
    if (cap == 0)
        cap = GET_INITIAL_CAP;
    while (cap < want) {
        if (cap > SIZE_MAX / 2) {
            cap = want;
            break;
        }
        cap *= 2;
    }
    d = realloc(m->data, cap);
    if (!d)
        return -1;
    m->data = d;
    m->cap = cap;
    return 0;
}

static size_t mem_write(char *data, size_t size, size_t nmemb, void *user)
{
    MemBuf *m = user;
    size_t n = size * nmemb;

    if (n == 0)
        return 0;
    /* Stop the moment the cap is passed - a hostile server must not be able
     * to grow this buffer until the Vita runs out of RAM. */
    if (n > m->max || m->len > m->max - n) {
        m->too_large = 1;
        return 0;
    }
    if (mem_reserve(m, n) != 0) {
        m->oom = 1;
        return 0;
    }
    memcpy(m->data + m->len, data, n);
    m->len += n;
    m->data[m->len] = '\0';
    return n;
}

static size_t header_cb(char *data, size_t size, size_t nmemb, void *user)
{
    MemBuf *m = user;
    size_t total = size * nmemb;

    /* libcurl hands us the body of each redirect hop as well, so every new
     * status line starts the document over - we only want the final one. */
    if (total >= 5 && memcmp(data, "HTTP/", 5) == 0) {
        m->len = 0;
        if (m->data)
            m->data[0] = '\0';
    }
    return total;
}

int http_get(const char *url, const char *user_agent, const char *ca_file,
             size_t max_bytes, HttpDoc *out, char *err, size_t errsz)
{
    MemBuf body;
    CURL *c;
    CURLcode rc;
    char errbuf[CURL_ERROR_SIZE];
    char *final_url;
    const char *eff = NULL;
    long status = 0;

    if (!out) {
        set_err(err, errsz, "no output");
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (!url || !*url) {
        set_err(err, errsz, "no url");
        return -1;
    }

    memset(&body, 0, sizeof(body));
    body.max = max_bytes ? max_bytes : VR_HTTP_GET_MAX;

    /* One handle per call, created and destroyed here: http_get is called from
     * the HLS worker thread and from the UI thread, so it has to be re-entrant. */
    c = curl_easy_init();
    if (!c) {
        set_err(err, errsz, "out of memory");
        return -1;
    }
    errbuf[0] = '\0';

    curl_easy_setopt(c, CURLOPT_URL, url);
    if (user_agent)
        curl_easy_setopt(c, CURLOPT_USERAGENT, user_agent);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, GET_CONNECT_TIMEOUT);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, GET_TOTAL_TIMEOUT);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    if (ca_file)
        curl_easy_setopt(c, CURLOPT_CAINFO, ca_file);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &body);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, mem_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);

    rc = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);

    if (body.too_large)
        set_err(err, errsz, "response too large (limit %lu bytes)",
                (unsigned long)body.max);
    else if (body.oom)
        set_err(err, errsz, "out of memory");
    else if (rc != CURLE_OK)
        set_err(err, errsz, "%s", errbuf[0] ? errbuf : curl_easy_strerror(rc));
    else if (status < 200 || status > 299)
        set_err(err, errsz, "HTTP %ld", status);
    else if (body.len == 0)
        set_err(err, errsz, "empty response");
    else {
        /* Callers resolve relative playlist URIs against the effective URL,
         * so a missing one is a failure, not a cosmetic gap. */
        curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &eff);
        final_url = dup_str(eff ? eff : url);
        if (!final_url) {
            set_err(err, errsz, "out of memory");
        } else {
            out->data = body.data;
            out->len = body.len;
            out->status = status;
            out->final_url = final_url;
            curl_easy_cleanup(c);
            if (err && errsz)
                err[0] = '\0';
            return 0;
        }
    }

    curl_easy_cleanup(c);
    free(body.data);
    memset(out, 0, sizeof(*out));
    out->status = status;
    return -1;
}

void http_doc_free(HttpDoc *d)
{
    if (!d)
        return;
    free(d->data);
    free(d->final_url);
    d->data = NULL;
    d->final_url = NULL;
    d->len = 0;
    d->status = 0;
}
