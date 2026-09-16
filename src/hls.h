#ifndef VR_HLS_H
#define VR_HLS_H

#include <stddef.h>
#include "ringbuf.h"
#include "sniff.h"

/* One live HLS stream on its own worker thread. Deliberately the same shape as
 * HttpStream (start/stop/finished/result) so player.c can own either kind of
 * source without a second state machine.
 *
 * The worker: fetches the playlist, picks the lowest-bandwidth variant if it is
 * a master, starts three target durations back from live, reloads the media
 * playlist about once per target duration (half that after a reload that did
 * not change), downloads each new segment, decrypts it if EXT-X-KEY says to,
 * demuxes MPEG-TS to ADTS (or scrubs ID3 from a bare .aac segment), and writes
 * the elementary stream into cfg.out. On any terminal error it calls
 * rb_close(cfg.out) exactly as http_stream does. */

typedef struct HlsStream HlsStream;

typedef struct {
    const char *url;
    const char *user_agent;
    const char *ca_file;
    RingBuf    *out;
    /* Once, after the first segment's container is known. */
    void (*on_ready)(void *user, VrCodec codec, long http_status);
    /* Human-readable progress worth showing: chosen variant, discontinuity,
     * a skipped segment. Never called for routine per-segment success. */
    void (*on_note)(void *user, const char *text);
    void *user;
} HlsStreamConfig;

HlsStream *hls_stream_start(const HlsStreamConfig *cfg);
void       hls_stream_stop(HlsStream *s);
int        hls_stream_finished(HlsStream *s);
int        hls_stream_result(HlsStream *s, char *err, size_t errsz);

/* Live counters for the stream panel. Safe to call from another thread. */
void       hls_stream_stats(HlsStream *s, int *segments_done, int *segments_failed);

#endif
