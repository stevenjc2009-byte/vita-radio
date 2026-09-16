#include "decoder.h"

#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libswresample/swresample.h>

#ifndef AV_PROFILE_AAC_HE
#define AV_PROFILE_AAC_HE    FF_PROFILE_AAC_HE
#define AV_PROFILE_AAC_HE_V2 FF_PROFILE_AAC_HE_V2
#endif

#define MAX_CONSECUTIVE_ERRORS 50

struct Decoder {
    VrCodec               codec;
    AVCodecContext       *ctx;
    AVCodecParserContext *parser;
    AVPacket             *pkt;
    AVFrame              *frame;

    SwrContext           *swr;
    int                   swr_rate;
    int                   swr_format;
    AVChannelLayout       swr_layout;

    int16_t              *out;
    int                   out_frames_cap;

    int                   in_rate;
    int                   in_channels;
    int                   errors;
};

Decoder *decoder_open(VrCodec codec)
{
    enum AVCodecID id;
    if (codec == VR_CODEC_MP3)
        id = AV_CODEC_ID_MP3;
    else if (codec == VR_CODEC_AAC)
        id = AV_CODEC_ID_AAC;
    else
        return NULL;

    const AVCodec *c = avcodec_find_decoder(id);
    if (!c)
        return NULL;

    Decoder *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->codec = codec;

    d->ctx = avcodec_alloc_context3(c);
    d->parser = av_parser_init((int)id);
    d->pkt = av_packet_alloc();
    d->frame = av_frame_alloc();
    if (!d->ctx || !d->parser || !d->pkt || !d->frame || avcodec_open2(d->ctx, c, NULL) < 0) {
        decoder_close(d);
        return NULL;
    }
    return d;
}

void decoder_close(Decoder *d)
{
    if (!d)
        return;
    swr_free(&d->swr);
    av_channel_layout_uninit(&d->swr_layout);
    if (d->parser)
        av_parser_close(d->parser);
    avcodec_free_context(&d->ctx);
    av_packet_free(&d->pkt);
    av_frame_free(&d->frame);
    free(d->out);
    free(d);
}

/* (Re)creates the resampler when the frame's rate, format or layout differ
 * from what the current one was built for. 0 ok, -1 failure. fn/user are only
 * used to emit the old resampler's delay line before it is torn down. */
static int ensure_swr(Decoder *d, const AVFrame *f, PcmFn fn, void *user)
{
    AVChannelLayout in_layout = {0};
    if (f->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || f->ch_layout.nb_channels <= 0) {
        int ch = f->ch_layout.nb_channels > 0 ? f->ch_layout.nb_channels : 2;
        av_channel_layout_default(&in_layout, ch);
    } else if (av_channel_layout_copy(&in_layout, &f->ch_layout) < 0) {
        return -1;
    }

    if (d->swr && d->swr_rate == f->sample_rate && d->swr_format == f->format &&
        av_channel_layout_compare(&d->swr_layout, &in_layout) == 0) {
        av_channel_layout_uninit(&in_layout);
        return 0;
    }

    /* The outgoing resampler still holds 30-60 samples in its delay line.
     * Dropping them on every format change - an HLS variant switch, an ad
     * break - is an audible click, so flush them out first. A stop asked for
     * here is not propagated: the next emit_frame() sees it one frame later,
     * which is cheaper than threading a third return code through. */
    if (d->swr && d->out && d->out_frames_cap > 0) {
        uint8_t *tail = (uint8_t *)d->out;
        int left = swr_convert(d->swr, &tail, d->out_frames_cap, NULL, 0);
        if (left > 0 && fn)
            fn(user, d->out, left);
    }

    swr_free(&d->swr);
    av_channel_layout_uninit(&d->swr_layout);

    AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
    if (swr_alloc_set_opts2(&d->swr, &out_layout, AV_SAMPLE_FMT_S16, VR_PCM_RATE,
                            &in_layout, (enum AVSampleFormat)f->format, f->sample_rate,
                            0, NULL) < 0 ||
        swr_init(d->swr) < 0) {
        swr_free(&d->swr);
        av_channel_layout_uninit(&in_layout);
        return -1;
    }
    d->swr_rate = f->sample_rate;
    d->swr_format = f->format;
    d->swr_layout = in_layout;   /* takes ownership */
    return 0;
}

/* Converts the current frame and hands it to fn. 0 ok, 1 fn asked to stop,
 * -1 conversion failure (counted as a decode error by the caller). */
static int emit_frame(Decoder *d, PcmFn fn, void *user)
{
    AVFrame *f = d->frame;
    if (f->sample_rate <= 0 || f->nb_samples <= 0)
        return -1;
    if (ensure_swr(d, f, fn, user) < 0)
        return -1;

    d->in_rate = f->sample_rate;
    d->in_channels = f->ch_layout.nb_channels;

    int need = swr_get_out_samples(d->swr, f->nb_samples);
    if (need < 0)
        return -1;
    need += 16;
    if (need > d->out_frames_cap) {
        int16_t *nb = realloc(d->out, (size_t)need * VR_PCM_CHANNELS * sizeof(int16_t));
        if (!nb)
            return -1;
        d->out = nb;
        d->out_frames_cap = need;
    }

    uint8_t *outp = (uint8_t *)d->out;
    int got = swr_convert(d->swr, &outp, d->out_frames_cap,
                          (const uint8_t *const *)f->extended_data, f->nb_samples);
    if (got < 0)
        return -1;
    if (got > 0 && fn && fn(user, d->out, got) == -1)
        return 1;
    return 0;
}

/* Drains every ready frame. 0 ok, 1 stop requested. */
static int receive_all(Decoder *d, PcmFn fn, void *user)
{
    for (;;) {
        int r = avcodec_receive_frame(d->ctx, d->frame);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
            return 0;
        if (r < 0) {
            d->errors++;
            return 0;
        }
        int e = emit_frame(d, fn, user);
        if (e == 1)
            return 1;
        if (e < 0)
            d->errors++;
        else
            d->errors = 0;
        if (d->errors >= MAX_CONSECUTIVE_ERRORS)
            return 0;
    }
}

int decoder_feed(Decoder *d, const unsigned char *data, size_t len, PcmFn fn, void *user)
{
    int stalled = 0;        /* consecutive parse calls that consumed nothing */

    while (len > 0) {
        int chunk = len > (size_t)0x7fffffff ? 0x7fffffff : (int)len;
        int used = av_parser_parse2(d->parser, d->ctx, &d->pkt->data, &d->pkt->size,
                                    data, chunk, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (used < 0) {
            if (++d->errors >= MAX_CONSECUTIVE_ERRORS)
                return -1;
            return 0;   /* drop the rest of this block */
        }
        data += used;
        len -= (size_t)used;
        stalled = (used == 0) ? stalled + 1 : 0;

        if (d->pkt->size > 0) {
            int r = avcodec_send_packet(d->ctx, d->pkt);
            if (r == AVERROR(EAGAIN)) {
                /* EAGAIN means "drain, then send this packet again", not
                 * "drop it". Parsing straight on loses ~21 ms of audio
                 * without even counting an error. */
                if (receive_all(d, fn, user) == 1)
                    return 1;
                r = avcodec_send_packet(d->ctx, d->pkt);
            }
            if (r < 0 && r != AVERROR(EAGAIN))
                d->errors++;
            if (receive_all(d, fn, user) == 1)
                return 1;
            if (d->errors >= MAX_CONSECUTIVE_ERRORS)
                return -1;
            /* A packet every time while the input never advances is a spin,
             * not progress: ff_combine_frame sets *buf_size to pc->index +
             * next, and next is negative when a frame completes inside bytes
             * the parser already held. That would re-parse this block for
             * ever on the audio thread, with the UI still responding. */
            if (stalled >= 2)
                break;
        } else if (used == 0) {
            break;   /* parser made no progress and produced nothing */
        }
    }
    return 0;
}

int decoder_input_rate(const Decoder *d)
{
    return d ? d->in_rate : 0;
}

int decoder_input_channels(const Decoder *d)
{
    return d ? d->in_channels : 0;
}

const char *decoder_profile_name(const Decoder *d)
{
    if (!d || d->codec == VR_CODEC_MP3)
        return "MP3";
    if (d->ctx) {
        if (d->ctx->profile == AV_PROFILE_AAC_HE_V2)
            return "HE-AACv2";
        if (d->ctx->profile == AV_PROFILE_AAC_HE)
            return "HE-AAC";
    }
    return "AAC-LC";
}
