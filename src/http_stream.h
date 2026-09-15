#ifndef VR_HTTP_STREAM_H
#define VR_HTTP_STREAM_H

#include <stddef.h>
#include "ringbuf.h"

/* One live HTTP(S) radio connection on its own worker thread (libcurl).
 * Sends "Icy-MetaData: 1", follows redirects, reads icy-metaint, strips metadata
 * with IcyParser, and writes pure audio bytes into cfg.out. When the transfer
 * ends for any reason the worker calls rb_close(cfg.out). */

typedef struct HttpStream HttpStream;

typedef struct {
    const char *url;
    const char *user_agent;   /* e.g. "VitaRadio/0.1" */
    const char *ca_file;      /* e.g. "app0:assets/cacert.pem"; NULL = curl default */
    RingBuf    *out;
    /* Called once from the worker, after response headers and before the first
     * audio byte. content_type may be NULL. */
    void (*on_headers)(void *user, const char *content_type, long http_status);
    void (*on_title)(void *user, const char *title);
    void *user;
} HttpStreamConfig;

/* Copies the config strings; spawns the worker. NULL on failure. */
HttpStream *http_stream_start(const HttpStreamConfig *cfg);

/* Asks the worker to stop (aborts cfg.out so a blocked write returns),
 * joins it, frees the handle. */
void        http_stream_stop(HttpStream *s);

int         http_stream_finished(HttpStream *s);   /* 1 once the worker exited */

/* Valid after finished. Returns 0 for a clean end, else the CURLcode, or an
 * HTTP status >= 400. A readable message goes to err. */
int         http_stream_result(HttpStream *s, char *err, size_t errsz);

#endif
