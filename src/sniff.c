#include "sniff.h"

#include <string.h>

/* ---- content types -------------------------------------------------- */

static int lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

/* Copies the media type (before ';', trimmed, lowercased) into out. */
static void media_type(const char *ct, char *out, size_t outsz)
{
    size_t n = 0;
    while (*ct == ' ' || *ct == '\t')
        ct++;
    while (*ct && *ct != ';' && n + 1 < outsz)
        out[n++] = (char)lower((unsigned char)*ct++);
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t'))
        n--;
    out[n] = '\0';
}

VrCodec sniff_codec_from_content_type(const char *content_type)
{
    static const char *const mp3[] = { "audio/mpeg", "audio/mp3", "audio/mpeg3" };
    static const char *const aac[] = { "audio/aac", "audio/aacp", "audio/x-aac", "audio/aac+" };
    char t[96];
    size_t i;

    if (!content_type)
        return VR_CODEC_UNKNOWN;
    media_type(content_type, t, sizeof(t));
    for (i = 0; i < sizeof(mp3) / sizeof(mp3[0]); i++)
        if (strcmp(t, mp3[i]) == 0)
            return VR_CODEC_MP3;
    for (i = 0; i < sizeof(aac) / sizeof(aac[0]); i++)
        if (strcmp(t, aac[i]) == 0)
            return VR_CODEC_AAC;
    return VR_CODEC_UNKNOWN;
}

VrBodyKind sniff_body_kind(const char *content_type)
{
    char t[96];

    if (!content_type)
        return VR_BODY_AUDIO;
    media_type(content_type, t, sizeof(t));
    if (strcmp(t, "application/vnd.apple.mpegurl") == 0 ||
        strcmp(t, "application/x-mpegurl") == 0)
        return VR_BODY_HLS;
    if (strcmp(t, "audio/x-scpls") == 0)
        return VR_BODY_PLS;
    /* audio/mpegurl and audio/x-mpegurl are both the classic plain-M3U type;
     * only the application ones mean HLS. */
    if (strcmp(t, "audio/x-mpegurl") == 0 ||
        strcmp(t, "audio/mpegurl") == 0)
        return VR_BODY_M3U;
    if (strncmp(t, "text/", 5) == 0)
        return VR_BODY_TEXT;
    return VR_BODY_AUDIO;
}

/* ---- frame headers -------------------------------------------------- */

/* Returns the MPEG Layer III frame length at p, or 0 if not a valid header. */
static size_t mp3_frame_len(const unsigned char *p, size_t avail)
{
    static const unsigned short br_v1[16] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
    static const unsigned short br_v2[16] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 };
    static const unsigned int sr_v1[3]  = { 44100, 48000, 32000 };
    static const unsigned int sr_v2[3]  = { 22050, 24000, 16000 };
    static const unsigned int sr_v25[3] = { 11025, 12000, 8000 };
    unsigned version, layer, bri, sri, pad, br, sr;

    if (avail < 4 || p[0] != 0xFF || (p[1] & 0xE0) != 0xE0)
        return 0;
    version = (p[1] >> 3) & 3;   /* 0 = 2.5, 1 = reserved, 2 = MPEG-2, 3 = MPEG-1 */
    layer   = (p[1] >> 1) & 3;   /* 1 = Layer III */
    bri     = p[2] >> 4;
    sri     = (p[2] >> 2) & 3;
    pad     = (p[2] >> 1) & 1;
    if (version == 1 || layer != 1 || bri == 0 || bri == 15 || sri == 3)
        return 0;

    if (version == 3) {
        br = br_v1[bri] * 1000u;
        sr = sr_v1[sri];
        return 144u * br / sr + pad;
    }
    br = br_v2[bri] * 1000u;
    sr = (version == 2) ? sr_v2[sri] : sr_v25[sri];
    return 72u * br / sr + pad;
}

/* Returns the ADTS frame length at p, or 0 if not a valid header. The field
 * rejections match adts_scan.c: the syncword and the layer bits alone are
 * only 14 bits, which noise clears often enough to matter. */
static size_t adts_frame_len(const unsigned char *p, size_t avail)
{
    unsigned freq, chan;
    size_t fl;
    if (avail < 7 || p[0] != 0xFF || (p[1] & 0xF6) != 0xF0)
        return 0;
    freq = (unsigned)(p[2] >> 2) & 0x0F;
    chan = (unsigned)((p[2] & 1) << 2) | (unsigned)(p[3] >> 6);
    if (freq > 12)                  /* 13, 14 reserved; 15 is escape */
        return 0;
    if (chan == 0)                  /* 0 means an in-band PCE: not a radio feed */
        return 0;
    fl = ((size_t)(p[3] & 3) << 11) | ((size_t)p[4] << 3) | (size_t)(p[5] >> 5);
    return fl >= 7 ? fl : 0;
}

static size_t id3v2_size(const unsigned char *p, size_t avail)
{
    size_t size;
    if (avail < 10 || p[0] != 'I' || p[1] != 'D' || p[2] != '3')
        return 0;
    if (p[3] == 0xFF || p[4] == 0xFF ||
        (p[6] | p[7] | p[8] | p[9]) & 0x80)
        return 0;
    size = ((size_t)p[6] << 21) | ((size_t)p[7] << 14) | ((size_t)p[8] << 7) | (size_t)p[9];
    size += 10;
    if (p[5] & 0x10)
        size += 10;   /* footer present */
    return size;
}

VrCodec sniff_codec_from_bytes(const unsigned char *buf, size_t len)
{
    size_t pos = 0, i, tag;

    if (!buf)
        return VR_CODEC_UNKNOWN;

    while ((tag = id3v2_size(buf + pos, len - pos)) != 0) {
        if (tag >= len - pos)
            return VR_CODEC_UNKNOWN;
        pos += tag;
    }

    for (i = pos; i + 4 <= len; i++) {
        size_t fl;
        if (buf[i] != 0xFF)
            continue;
        fl = mp3_frame_len(buf + i, len - i);
        if (fl && fl < len - i && mp3_frame_len(buf + i + fl, len - i - fl))
            return VR_CODEC_MP3;
        fl = adts_frame_len(buf + i, len - i);
        if (fl && fl < len - i && adts_frame_len(buf + i + fl, len - i - fl))
            return VR_CODEC_AAC;
    }
    return VR_CODEC_UNKNOWN;
}

/* ---- playlist bodies ------------------------------------------------ */

static int prefix_ci(const unsigned char *p, size_t avail, const char *s)
{
    size_t n = strlen(s), i;
    if (avail < n)
        return 0;
    for (i = 0; i < n; i++)
        if (lower(p[i]) != lower((unsigned char)s[i]))
            return 0;
    return 1;
}

static int contains(const unsigned char *p, size_t avail, const char *s)
{
    size_t n = strlen(s), i;
    for (i = 0; i + n <= avail; i++)
        if (memcmp(p + i, s, n) == 0)
            return 1;
    return 0;
}

VrBodyKind sniff_body_kind_from_bytes(const unsigned char *buf, size_t len)
{
    size_t i = 0;

    if (!buf)
        return VR_BODY_AUDIO;
    if (len >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF)
        i = 3;
    while (i < len && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n'))
        i++;
    buf += i;
    len -= i;

    if (prefix_ci(buf, len, "#EXTM3U"))
        return contains(buf, len, "#EXT-X-") ? VR_BODY_HLS : VR_BODY_M3U;
    if (prefix_ci(buf, len, "[playlist]"))
        return VR_BODY_PLS;
    if (len > 0 && buf[0] == '<')
        return VR_BODY_TEXT;

    /* Plain m3u: some line starts with http. Stop at the first byte that
     * can't be text so binary audio isn't mistaken for a playlist. */
    for (i = 0; i < len; i++) {
        unsigned char c = buf[i];
        if (c == 0 || (c < 0x20 && c != '\t' && c != '\r' && c != '\n'))
            break;
        if (c >= 0x80) {
            /* Only a well-formed UTF-8 sequence counts as text. This is what
             * keeps audio out: 0xFF and 0xFE - the MPEG and ADTS sync byte
             * among them - are not legal lead bytes at all. */
            size_t extra = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC2) ? 1 : 0;
            size_t k;
            if (extra == 0 || i + extra >= len)
                break;
            for (k = 1; k <= extra; k++)
                if ((buf[i + k] & 0xC0) != 0x80)
                    break;
            if (k <= extra)
                break;
            i += extra;
            continue;
        }
        if ((i == 0 || buf[i - 1] == '\n') && prefix_ci(buf + i, len - i, "http"))
            return VR_BODY_M3U;
    }
    /* Every byte was text and no playlist line turned up: a message, almost
     * always an error page. Calling it audio costs 50 decode errors and a
     * multi-second hang before the player gives up on a dead station. */
    if (i == len && len > 0)
        return VR_BODY_TEXT;
    return VR_BODY_AUDIO;
}
