#ifndef VR_DECODER_H
#define VR_DECODER_H

#include <stddef.h>
#include <stdint.h>
#include "sniff.h"

#define VR_PCM_RATE     48000   /* every decoder output is resampled to this */
#define VR_PCM_CHANNELS 2

/* Receives interleaved S16 stereo at VR_PCM_RATE. Return 0, or -1 to stop. */
typedef int (*PcmFn)(void *user, const int16_t *pcm, int frames);

typedef struct Decoder Decoder;

/* FFmpeg (libavcodec + parser + libswresample). MP3, or ADTS AAC including
 * HE-AAC v1/v2 (implicit SBR/PS: the real output rate comes from the decoded frame,
 * never from the ADTS header). HLS will later feed ADTS pulled out of MPEG-TS
 * through this same interface. */
Decoder    *decoder_open(VrCodec codec);
void        decoder_close(Decoder *d);

/* Feed any amount of compressed bytes; frame boundaries are found internally.
 * Returns 0, 1 if fn asked to stop, -1 after too many consecutive decode errors. */
int         decoder_feed(Decoder *d, const unsigned char *data, size_t len, PcmFn fn, void *user);

int         decoder_input_rate(const Decoder *d);      /* 0 until the first frame */
int         decoder_input_channels(const Decoder *d);  /* 0 until the first frame */
const char *decoder_profile_name(const Decoder *d);    /* "MP3", "AAC-LC", "HE-AAC", "HE-AACv2" */

#endif
