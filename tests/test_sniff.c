#include "sniff.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0, g_pass = 0;

#define CHECK(name, cond) do { \
    if (cond) { printf("PASS %s\n", name); g_pass++; } \
    else { printf("FAIL %s (%s:%d)\n", name, __FILE__, __LINE__); g_fail++; } \
} while (0)

/* MPEG-1 Layer III, 128 kbps, 44.1 kHz: 417 bytes, 418 with padding. */
static size_t put_mp3_v1(unsigned char *b, int pad)
{
    size_t fl = 417 + (pad ? 1 : 0);
    memset(b, 0, fl);
    b[0] = 0xFF; b[1] = 0xFB; b[2] = (unsigned char)(0x90 | (pad ? 0x02 : 0)); b[3] = 0x44;
    return fl;
}

/* MPEG-2 Layer III, 64 kbps, 22.05 kHz: 72*64000/22050 = 208 bytes. */
static size_t put_mp3_v2(unsigned char *b)
{
    memset(b, 0, 208);
    b[0] = 0xFF; b[1] = 0xF3; b[2] = 0x80; b[3] = 0xC4;
    return 208;
}

static size_t put_adts(unsigned char *b, size_t fl)
{
    memset(b, 0, fl);
    b[0] = 0xFF;
    b[1] = 0xF1;                                      /* MPEG-4, layer 00, no CRC */
    b[2] = (unsigned char)((1 << 6) | (4 << 2));      /* AAC-LC, 44.1 kHz */
    b[3] = (unsigned char)((2 << 6) | ((fl >> 11) & 3));
    b[4] = (unsigned char)((fl >> 3) & 0xFF);
    b[5] = (unsigned char)(((fl & 7) << 5) | 0x1F);
    b[6] = 0xFC;
    return fl;
}

static size_t put_id3(unsigned char *b, size_t body, int footer)
{
    size_t n = 10 + body + (footer ? 10 : 0);
    b[0] = 'I'; b[1] = 'D'; b[2] = '3'; b[3] = 4; b[4] = 0;
    b[5] = footer ? 0x10 : 0;
    b[6] = (unsigned char)((body >> 21) & 0x7F);
    b[7] = (unsigned char)((body >> 14) & 0x7F);
    b[8] = (unsigned char)((body >> 7) & 0x7F);
    b[9] = (unsigned char)(body & 0x7F);
    memset(b + 10, 0, body);
    /* Plant two chained ADTS frames inside the tag: must be skipped. */
    if (body >= 14) {
        put_adts(b + 10, 7);
        put_adts(b + 17, 7);
    }
    if (footer)
        memcpy(b + 10 + body, "3DI\x04\x00\x10\x00\x00\x07\x68", 10);
    return n;
}

static unsigned char buf[1 << 17];

int main(void)
{
    size_t n, i;

    /* ---- content types -------------------------------------------- */
    CHECK("ct_mpeg", sniff_codec_from_content_type("audio/mpeg") == VR_CODEC_MP3);
    CHECK("ct_mp3", sniff_codec_from_content_type("audio/mp3") == VR_CODEC_MP3);
    CHECK("ct_mpeg3", sniff_codec_from_content_type("audio/mpeg3") == VR_CODEC_MP3);
    CHECK("ct_mpeg_case_params", sniff_codec_from_content_type(" Audio/MPEG ; charset=utf-8") == VR_CODEC_MP3);
    CHECK("ct_aac", sniff_codec_from_content_type("audio/aac") == VR_CODEC_AAC);
    CHECK("ct_aacp", sniff_codec_from_content_type("AUDIO/AACP") == VR_CODEC_AAC);
    CHECK("ct_x_aac", sniff_codec_from_content_type("audio/x-aac") == VR_CODEC_AAC);
    CHECK("ct_aac_plus", sniff_codec_from_content_type("audio/aac+;x=1") == VR_CODEC_AAC);
    CHECK("ct_ogg_unknown", sniff_codec_from_content_type("audio/ogg") == VR_CODEC_UNKNOWN);
    CHECK("ct_mpegurl_not_mp3", sniff_codec_from_content_type("audio/mpegurl") == VR_CODEC_UNKNOWN);
    CHECK("ct_null_unknown", sniff_codec_from_content_type(NULL) == VR_CODEC_UNKNOWN);

    CHECK("bk_hls_apple", sniff_body_kind("application/vnd.apple.mpegurl") == VR_BODY_HLS);
    CHECK("bk_hls_x", sniff_body_kind("Application/X-MpegURL; charset=utf-8") == VR_BODY_HLS);
    CHECK("bk_pls", sniff_body_kind("audio/x-scpls") == VR_BODY_PLS);
    CHECK("bk_m3u", sniff_body_kind("audio/x-mpegurl") == VR_BODY_M3U);
    /* audio/mpegurl and audio/x-mpegurl are the same classic plain-M3U type;
     * only the two application types mean HLS. */
    CHECK("bk_m3u_audio_mpegurl", sniff_body_kind("audio/mpegurl") == VR_BODY_M3U);
    CHECK("bk_text_html", sniff_body_kind("text/html; charset=UTF-8") == VR_BODY_TEXT);
    CHECK("bk_text_plain", sniff_body_kind("TEXT/plain") == VR_BODY_TEXT);
    CHECK("bk_audio_mpeg", sniff_body_kind("audio/mpeg") == VR_BODY_AUDIO);
    CHECK("bk_octet", sniff_body_kind("application/octet-stream") == VR_BODY_AUDIO);
    CHECK("bk_null", sniff_body_kind(NULL) == VR_BODY_AUDIO);

    /* ---- MP3 ------------------------------------------------------ */
    n = 0;
    n += put_mp3_v1(buf + n, 0);
    n += put_mp3_v1(buf + n, 1);
    n += put_mp3_v1(buf + n, 0);
    CHECK("mp3_v1_frames", sniff_codec_from_bytes(buf, n) == VR_CODEC_MP3);
    CHECK("mp3_v1_body_audio", sniff_body_kind_from_bytes(buf, n) == VR_BODY_AUDIO);

    /* Padding bit must change the computed frame length: a padded frame
     * placed at 417 bytes (instead of 418) must not chain. */
    n = put_mp3_v1(buf, 1);
    memset(buf + 417, 0, 4);
    put_mp3_v1(buf + 418, 0);
    CHECK("mp3_padding_length_used", sniff_codec_from_bytes(buf, 422) == VR_CODEC_MP3);
    memmove(buf + 417, buf + 418, 4);
    buf[421] = 0;
    CHECK("mp3_red_off_by_one_rejected", sniff_codec_from_bytes(buf, 422) == VR_CODEC_UNKNOWN);

    n = 0;
    n += put_mp3_v2(buf + n);
    n += put_mp3_v2(buf + n);
    CHECK("mp3_v2_frames", sniff_codec_from_bytes(buf, n) == VR_CODEC_MP3);

    /* Garbage prefix before the first frame. */
    for (i = 0; i < 333; i++)
        buf[i] = (unsigned char)(i & 0x7F);
    n = 333;
    n += put_mp3_v1(buf + n, 0);
    n += put_mp3_v1(buf + n, 0);
    CHECK("mp3_after_garbage", sniff_codec_from_bytes(buf, n) == VR_CODEC_MP3);

    /* Lone sync with nothing valid after it. */
    n = put_mp3_v1(buf, 0);
    memset(buf + n, 0, 600);
    CHECK("mp3_lone_sync_unknown", sniff_codec_from_bytes(buf, n + 600) == VR_CODEC_UNKNOWN);
    CHECK("mp3_truncated_unknown", sniff_codec_from_bytes(buf, 4) == VR_CODEC_UNKNOWN);

    /* Invalid fields: reserved version, layer I, bad bitrate, bad samplerate. */
    {
        static const unsigned char bad[4][2] = {
            { 0xEB, 0x90 },   /* version reserved */
            { 0xFF, 0x90 },   /* layer I */
            { 0xFB, 0xF0 },   /* bitrate index 15 */
            { 0xFB, 0x9C },   /* samplerate index 3 */
        };
        int all = 1, k;
        for (k = 0; k < 4; k++) {
            n = 0;
            n += put_mp3_v1(buf + n, 0);
            n += put_mp3_v1(buf + n, 0);
            buf[1] = buf[418] = bad[k][0];
            buf[2] = buf[419] = bad[k][1];
            if (sniff_codec_from_bytes(buf, n) != VR_CODEC_UNKNOWN)
                all = 0;
        }
        CHECK("mp3_invalid_fields_unknown", all);
    }

    /* ---- ADTS ----------------------------------------------------- */
    n = 0;
    n += put_adts(buf + n, 200);
    n += put_adts(buf + n, 311);
    n += put_adts(buf + n, 200);
    CHECK("adts_frames", sniff_codec_from_bytes(buf, n) == VR_CODEC_AAC);

    n = put_adts(buf, 200);
    memset(buf + n, 0x11, 300);
    CHECK("adts_lone_unknown", sniff_codec_from_bytes(buf, n + 300) == VR_CODEC_UNKNOWN);

    n = 0;
    n += put_adts(buf + n, 200);
    n += put_adts(buf + n, 200);
    buf[1] = buf[201] = 0xF3;   /* layer 01: not ADTS */
    CHECK("adts_bad_layer_unknown", sniff_codec_from_bytes(buf, n) == VR_CODEC_UNKNOWN);

    /* The syncword and the layer bits are only 14 of the bits that have to be
     * right. adts_scan.c also rejects the reserved/escape sampling frequency
     * indexes and an absent channel configuration; sniff must agree, or a
     * body of noise with two plausible pairs in it passes as AAC. */
    {
        int good, bad_freq, bad_chan;
        n = 0;
        n += put_adts(buf + n, 200);
        n += put_adts(buf + n, 200);
        good = sniff_codec_from_bytes(buf, n) == VR_CODEC_AAC;
        buf[2] = buf[202] = 0x7C;        /* freq index 15 = escape */
        bad_freq = sniff_codec_from_bytes(buf, n) == VR_CODEC_UNKNOWN;
        n = 0;
        n += put_adts(buf + n, 200);
        n += put_adts(buf + n, 200);
        buf[3] = buf[203] = 0x00;        /* channel_configuration 0 */
        bad_chan = sniff_codec_from_bytes(buf, n) == VR_CODEC_UNKNOWN;
        CHECK("adts_green_control_aac", good);
        CHECK("adts_bad_freq_unknown", bad_freq);
        CHECK("adts_bad_chan_unknown", bad_chan);
    }

    /* ---- ID3v2 ---------------------------------------------------- */
    n = put_id3(buf, 1000, 0);
    n += put_mp3_v1(buf + n, 0);
    n += put_mp3_v1(buf + n, 0);
    CHECK("id3_then_mp3", sniff_codec_from_bytes(buf, n) == VR_CODEC_MP3);
    /* RED control: without the ID3 header the planted ADTS pair wins. */
    CHECK("id3_red_control_adts_inside", sniff_codec_from_bytes(buf + 10, n - 10) == VR_CODEC_AAC);

    n = put_id3(buf, 1000, 1);
    n += put_mp3_v1(buf + n, 0);
    n += put_mp3_v1(buf + n, 0);
    CHECK("id3_footer_then_mp3", sniff_codec_from_bytes(buf, n) == VR_CODEC_MP3);

    n = put_id3(buf, 1000, 0);
    CHECK("id3_only_unknown", sniff_codec_from_bytes(buf, n) == VR_CODEC_UNKNOWN);

    /* ---- noise ---------------------------------------------------- */
    {
        unsigned x = 0x12345678u;
        for (i = 0; i < 65536; i++) {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            buf[i] = (unsigned char)x;
        }
        CHECK("noise_unknown", sniff_codec_from_bytes(buf, 65536) == VR_CODEC_UNKNOWN);
        CHECK("empty_unknown", sniff_codec_from_bytes(buf, 0) == VR_CODEC_UNKNOWN);
        CHECK("noise_body_audio", sniff_body_kind_from_bytes(buf, 65536) == VR_BODY_AUDIO);
    }

    /* ---- text bodies ---------------------------------------------- */
    {
        const char *hls = "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:10\nseg1.aac\n";
        const char *m3u = "\xEF\xBB\xBF  \r\n#EXTM3U\n#EXTINF:-1,Radio\nhttp://example.com/stream\n";
        const char *url = "http://example.com:8000/live.mp3\r\n";
        const char *url2 = "# comment\nhttps://example.com/live\n";
        const char *pls = "[Playlist]\nNumberOfEntries=1\nFile1=http://example.com/s\n";
        const char *html = "\n<!DOCTYPE html><html><body>http://x</body></html>";
        const char *junk = "hello world";
        CHECK("body_hls", sniff_body_kind_from_bytes((const unsigned char *)hls, strlen(hls)) == VR_BODY_HLS);
        CHECK("body_m3u_bom_ws", sniff_body_kind_from_bytes((const unsigned char *)m3u, strlen(m3u)) == VR_BODY_M3U);
        CHECK("body_m3u_bare_url", sniff_body_kind_from_bytes((const unsigned char *)url, strlen(url)) == VR_BODY_M3U);
        CHECK("body_m3u_url_later_line", sniff_body_kind_from_bytes((const unsigned char *)url2, strlen(url2)) == VR_BODY_M3U);
        CHECK("body_pls_case", sniff_body_kind_from_bytes((const unsigned char *)pls, strlen(pls)) == VR_BODY_PLS);
        CHECK("body_html_text", sniff_body_kind_from_bytes((const unsigned char *)html, strlen(html)) == VR_BODY_TEXT);
        /* An all-text body with no playlist line in it is a message - almost
         * always an error page - not audio. Calling it audio costs 50 decode
         * errors before the player gives up. */
        CHECK("body_other_text", sniff_body_kind_from_bytes((const unsigned char *)junk, strlen(junk)) == VR_BODY_TEXT);
        CHECK("body_empty_audio", sniff_body_kind_from_bytes((const unsigned char *)"", 0) == VR_BODY_AUDIO);
    }
    {
        const char *err   = "Error 404";
        const char *plain = "Station temporarily unavailable\r\nTry again later\n";
        const char *utf8  = "Fehler: Sender nicht verf\xC3\xBCgbar";
        CHECK("body_plain_error_text", sniff_body_kind_from_bytes((const unsigned char *)err, strlen(err)) == VR_BODY_TEXT);
        CHECK("body_plain_lines_text", sniff_body_kind_from_bytes((const unsigned char *)plain, strlen(plain)) == VR_BODY_TEXT);
        CHECK("body_utf8_error_text", sniff_body_kind_from_bytes((const unsigned char *)utf8, strlen(utf8)) == VR_BODY_TEXT);

        /* RED controls: real audio must still be audio. A 0xFF sync byte is
         * not a legal UTF-8 lead byte, which is what keeps them apart. */
        n = 0;
        n += put_adts(buf + n, 200);
        n += put_adts(buf + n, 200);
        CHECK("body_adts_bytes_audio", sniff_body_kind_from_bytes(buf, n) == VR_BODY_AUDIO);
        n = 0;
        n += put_mp3_v1(buf + n, 0);
        n += put_mp3_v1(buf + n, 0);
        CHECK("body_mp3_bytes_audio", sniff_body_kind_from_bytes(buf, n) == VR_BODY_AUDIO);
    }

    printf("test_sniff: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
