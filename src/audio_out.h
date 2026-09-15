#ifndef VR_AUDIO_OUT_H
#define VR_AUDIO_OUT_H

#include <stdint.h>

#define VR_AUDIO_GRAIN 1024   /* frames per sceAudioOutOutput call */

typedef struct AudioOut AudioOut;

/* Opens a stereo sceAudioOut BGM port at VR_PCM_RATE (48000). NULL on failure. */
AudioOut *audio_out_open(void);

/* Collects frames into VR_AUDIO_GRAIN blocks; every full block goes to
 * sceAudioOutOutput, which blocks in real time (this is what paces playback).
 * Returns 0, or -1 on port error. */
int       audio_out_write(AudioOut *a, const int16_t *pcm, int frames);

/* Drops any partial block (used on stop / station change). */
void      audio_out_discard(AudioOut *a);

void      audio_out_set_volume(AudioOut *a, int volume);  /* 0..32768 */
void      audio_out_close(AudioOut *a);

#endif
