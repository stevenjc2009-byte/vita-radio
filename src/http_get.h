#ifndef VR_HTTP_GET_H
#define VR_HTTP_GET_H

#include <stddef.h>

/* Fetch one whole small document (playlist, API response) into memory.
 * Blocking, runs on the caller's thread - this is not the streaming path;
 * see http_stream.h for that. */

#define VR_HTTP_GET_MAX 1048576   /* refuse anything larger than 1 MiB */

typedef struct {
    char  *data;        /* NUL-terminated; free with http_doc_free */
    size_t len;
    long   status;      /* HTTP status, 0 if the transfer never got that far */
    char  *final_url;   /* after redirects - resolve relative URIs against this */
} HttpDoc;

/* max_bytes 0 means VR_HTTP_GET_MAX. Returns 0 on a 2xx response with a body,
 * else -1 with a readable reason in err. out is zeroed on failure. */
int  http_get(const char *url, const char *user_agent, const char *ca_file,
              size_t max_bytes, HttpDoc *out, char *err, size_t errsz);

void http_doc_free(HttpDoc *d);

/* Relative URI resolution lives in url_util.h - it has to stay free of libcurl
 * so the m3u8 and playlist host tests can link it. */

#endif
