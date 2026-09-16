#ifndef VR_M3U8_H
#define VR_M3U8_H

#include <stddef.h>

/* RFC 8216 playlist parser. Pure text in, structs out - no I/O, so the host
 * tests can feed it captured playlists. */

typedef enum {
    VR_M3U8_UNKNOWN = 0,
    VR_M3U8_MASTER,     /* has #EXT-X-STREAM-INF */
    VR_M3U8_MEDIA       /* has #EXT-X-TARGETDURATION */
} M3u8Kind;

typedef struct {
    char *uri;          /* absolute, resolved against base_url */
    long  bandwidth;    /* BANDWIDTH, 0 if absent */
    char  codecs[40];   /* CODECS, "" if absent */
} M3u8Variant;

typedef struct {
    char     *uri;              /* absolute */
    double    duration;         /* EXTINF seconds */
    long long seq;              /* this segment's media sequence number */
    int       discontinuity;    /* 1 if preceded by #EXT-X-DISCONTINUITY */
    int       encrypted;        /* 1 if an AES-128 EXT-X-KEY applies */
    char     *key_uri;          /* absolute, NULL when not encrypted */
    char      key_method[32];   /* EXT-X-KEY METHOD verbatim, "" if no key tag */
    unsigned char iv[16];       /* explicit IV, or seq zero-padded big-endian */
} M3u8Segment;

typedef struct {
    M3u8Kind      kind;
    M3u8Variant  *variants;         /* master only */
    int           variant_count;
    M3u8Segment  *segments;         /* media only */
    int           segment_count;
    double        target_duration;  /* EXT-X-TARGETDURATION */
    long long     media_sequence;   /* EXT-X-MEDIA-SEQUENCE, 0 if absent */
    int           endlist;          /* 1 if #EXT-X-ENDLIST present (VOD, stop reloading) */
} M3u8;

/* base_url resolves relative URIs; pass the URL the text was fetched from.
 * Returns 0 on success, -1 if the text is not a playlist (no #EXTM3U).
 * An EXT-X-KEY with METHOD=NONE clears encryption for following segments;
 * a METHOD this parser does not understand (SAMPLE-AES), or one whose URI or IV
 * was too long to have arrived intact, marks those segments encrypted with
 * key_uri NULL, so the caller can refuse them cleanly and name key_method. */
int  m3u8_parse(const char *text, size_t len, const char *base_url, M3u8 *out);
void m3u8_free(M3u8 *p);

/* Index of the variant to play: the lowest BANDWIDTH that has one, because the
 * Vita's Wi-Fi and the 96 kbps-class radio feeds make the smallest the right
 * pick. Returns -1 if there are no variants. */
int  m3u8_pick_variant(const M3u8 *master);

#endif
