#ifndef VR_RBROWSER_H
#define VR_RBROWSER_H

#include <stddef.h>

/* radio-browser.info client. ~58,000 stations, metadata dedicated to the
 * public domain.
 *
 * Fixed-size fields on purpose: 100 results is about 70 KB and cannot fragment
 * the heap mid-playback. Mirrors are tried in order on connect failure or 5xx
 * (the project documents that any single mirror can and does go dark), and the
 * API wants a real "App/Version" User-Agent. */

#define RB_MAX_RESULTS 100

typedef struct {
    char name[96];
    char url[512];      /* url_resolved when the API gave one, else url */
    char codec[16];     /* a hint only - the API reports UNKNOWN for many HLS feeds */
    char country[4];
    char uuid[40];
    int  bitrate;
    int  is_hls;        /* the API's hls flag */
} RbStation;

typedef struct {
    RbStation *items;
    int        count;
} RbResult;

/* Each returns 0 with out filled, or -1 with a reason in err. limit is clamped
 * to RB_MAX_RESULTS. Free with rb_result_free. */
int  rb_search_name(const char *query, int limit, RbResult *out, char *err, size_t errsz);
int  rb_top_click(int limit, RbResult *out, char *err, size_t errsz);
int  rb_by_country(const char *iso2, int limit, RbResult *out, char *err, size_t errsz);
int  rb_by_tag(const char *tag, int limit, RbResult *out, char *err, size_t errsz);

void rb_result_free(RbResult *r);

/* The API asks clients to register a play so its popularity data means
 * something. Best effort - failure is ignored, never surfaced. */
void rb_register_click(const char *uuid);

/* Set once at startup; defaults to "VitaRadio/2.0.0". */
void rb_set_user_agent(const char *ua);
void rb_set_ca_file(const char *path);

#endif
