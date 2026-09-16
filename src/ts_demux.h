#ifndef VR_TS_DEMUX_H
#define VR_TS_DEMUX_H

#include <stddef.h>

/* MPEG-TS -> audio elementary stream.
 *
 * Hand-written on purpose: the VitaSDK's libavformat.a is built WITHOUT
 * ff_mpegts_demuxer (measured 2026-09-16 - it has 22 demuxers and mpegts,
 * hls and mov are all absent), so FFmpeg cannot open a .ts segment here.
 * Measured BBC segment: 188-byte packets, PAT on PID 0, PMT and one audio ES.
 *
 * Emits the elementary stream bytes exactly as they appear inside the PES
 * payloads, concatenated. For stream_type 0x0F that is an ADTS byte stream
 * which decoder_feed() parses directly - do NOT assume an ADTS sync word sits
 * at the start of a PES payload, it does not. */

typedef struct TsDemux TsDemux;

/* Return 0 to continue, non-zero to stop the feed. */
typedef int (*TsEsFn)(void *user, const unsigned char *es, size_t len);

TsDemux *ts_demux_open(void);
void     ts_demux_close(TsDemux *t);

/* Forget PAT/PMT and any partial packet. Call on #EXT-X-DISCONTINUITY. */
void     ts_demux_reset(TsDemux *t);

/* Feed arbitrary-sized chunks; packet boundaries are found internally, including
 * resynchronising on 0x47 after a gap. Returns 0, 1 if fn asked to stop,
 * -1 on unrecoverable garbage. */
int      ts_demux_feed(TsDemux *t, const unsigned char *data, size_t len,
                       TsEsFn fn, void *user);

/* 0x0F ADTS AAC, 0x11 LATM AAC, 0x03/0x04 MPEG audio, 0 until the PMT is seen. */
int      ts_demux_stream_type(const TsDemux *t);

/* Count of continuity-counter discontinuities seen - a lost-packet indicator
 * worth surfacing rather than hiding. */
unsigned ts_demux_cc_errors(const TsDemux *t);

#endif
