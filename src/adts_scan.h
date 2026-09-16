#ifndef VR_ADTS_SCAN_H
#define VR_ADTS_SCAN_H

#include <stddef.h>

/* Cleans a raw ".aac" HLS segment stream into pure ADTS frames.
 *
 * Why this exists: many HLS radio feeds ship bare ADTS segments rather than
 * MPEG-TS, and they interleave ID3v2.4 tags BETWEEN frames (timed metadata),
 * not just at the start. Feeding those tag bytes to the decoder desynchronises
 * it, so they are dropped here. Stateful across chunks: a tag or a frame may be
 * split across two feeds. */

typedef struct AdtsScan AdtsScan;

/* Return 0 to continue, non-zero to stop the feed. */
typedef int (*AdtsFn)(void *user, const unsigned char *frames, size_t len);

AdtsScan *adts_scan_open(void);
void      adts_scan_close(AdtsScan *a);
void      adts_scan_reset(AdtsScan *a);

/* Emits only whole ADTS frames, in order, with tags and junk removed.
 * Returns 0, 1 if fn asked to stop, -1 on unrecoverable garbage. */
int       adts_scan_feed(AdtsScan *a, const unsigned char *data, size_t len,
                         AdtsFn fn, void *user);

unsigned  adts_scan_tags_dropped(const AdtsScan *a);
unsigned  adts_scan_bytes_skipped(const AdtsScan *a);

#endif
