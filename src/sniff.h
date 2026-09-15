#ifndef VR_SNIFF_H
#define VR_SNIFF_H

#include <stddef.h>

typedef enum {
    VR_CODEC_UNKNOWN = 0,
    VR_CODEC_MP3,
    VR_CODEC_AAC        /* ADTS framed; covers AAC-LC and HE-AAC v1/v2 */
} VrCodec;

typedef enum {
    VR_BODY_AUDIO = 0,  /* audio/..., or unknown type that may still be audio */
    VR_BODY_HLS,        /* application/vnd.apple.mpegurl, application/x-mpegurl, audio/mpegurl */
    VR_BODY_PLS,        /* audio/x-scpls */
    VR_BODY_M3U,        /* audio/x-mpegurl (plain m3u) */
    VR_BODY_TEXT        /* text/html etc. - not a stream */
} VrBodyKind;

/* Case-insensitive; ignores parameters like "; charset=". NULL -> UNKNOWN / AUDIO. */
VrCodec    sniff_codec_from_content_type(const char *content_type);
VrBodyKind sniff_body_kind(const char *content_type);

/* Detects codec from the first bytes of a stream: skips an ID3v2 tag if present,
 * then requires two consecutive valid frame headers (ADTS 0xFFF with layer 0,
 * or MPEG-1/2/2.5 Layer III sync whose computed frame length leads to another sync).
 * Returns VR_CODEC_UNKNOWN if not confident. */
VrCodec    sniff_codec_from_bytes(const unsigned char *buf, size_t len);

/* Detects a playlist by body text: "#EXTM3U" + "#EXT-X-" -> HLS, "[playlist]" -> PLS. */
VrBodyKind sniff_body_kind_from_bytes(const unsigned char *buf, size_t len);

#endif
