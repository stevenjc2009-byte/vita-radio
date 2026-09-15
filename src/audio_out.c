#include "audio_out.h"
#include "decoder.h"

#include <stdlib.h>
#include <string.h>

#include <psp2/audioout.h>

struct AudioOut {
    int     port;
    int     fill;                                   /* frames in buf */
    int16_t buf[VR_AUDIO_GRAIN * VR_PCM_CHANNELS];
};

static void apply_volume(int port, int volume)
{
    int vol[2] = { volume, volume };
    sceAudioOutSetVolume(port, (SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH), vol);
}

AudioOut *audio_out_open(void)
{
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, VR_AUDIO_GRAIN, VR_PCM_RATE,
                                   SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0)
        return NULL;
    AudioOut *a = calloc(1, sizeof(*a));
    if (!a) {
        sceAudioOutReleasePort(port);
        return NULL;
    }
    a->port = port;
    apply_volume(port, SCE_AUDIO_VOLUME_0DB);
    return a;
}

int audio_out_write(AudioOut *a, const int16_t *pcm, int frames)
{
    if (!a)
        return -1;
    while (frames > 0) {
        int n = VR_AUDIO_GRAIN - a->fill;
        if (n > frames)
            n = frames;
        memcpy(a->buf + a->fill * VR_PCM_CHANNELS, pcm, (size_t)n * VR_PCM_CHANNELS * sizeof(int16_t));
        a->fill += n;
        pcm += n * VR_PCM_CHANNELS;
        frames -= n;
        if (a->fill == VR_AUDIO_GRAIN) {
            a->fill = 0;
            if (sceAudioOutOutput(a->port, a->buf) < 0)
                return -1;
        }
    }
    return 0;
}

void audio_out_discard(AudioOut *a)
{
    if (a)
        a->fill = 0;
}

void audio_out_set_volume(AudioOut *a, int volume)
{
    if (!a)
        return;
    if (volume < 0)
        volume = 0;
    if (volume > SCE_AUDIO_VOLUME_0DB)
        volume = SCE_AUDIO_VOLUME_0DB;
    apply_volume(a->port, volume);
}

void audio_out_close(AudioOut *a)
{
    if (!a)
        return;
    sceAudioOutReleasePort(a->port);
    free(a);
}
