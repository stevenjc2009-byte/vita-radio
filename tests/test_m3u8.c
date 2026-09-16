#include "m3u8.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0, g_pass = 0;

#define CHECK(name, cond) do { \
    if (cond) { printf("PASS %s\n", name); g_pass++; } \
    else { printf("FAIL %s (%s:%d)\n", name, __FILE__, __LINE__); g_fail++; } \
} while (0)

static void chk_str(const char *name, const char *got, const char *want)
{
    if (got && strcmp(got, want) == 0) {
        printf("PASS %s\n", name);
        g_pass++;
    } else {
        printf("FAIL %s: got \"%s\" want \"%s\"\n", name, got ? got : "(null)", want);
        g_fail++;
    }
}

/* Out-of-range access returns a zeroed record so a broken parser fails the
 * assertion instead of crashing the whole suite. */
static const M3u8Segment ZERO_SEG;
static const M3u8Variant ZERO_VAR;

static const M3u8Segment *sg(const M3u8 *m, int i)
{
    if (m->segments && i >= 0 && i < m->segment_count)
        return &m->segments[i];
    return &ZERO_SEG;
}

static const M3u8Variant *vr(const M3u8 *m, int i)
{
    if (m->variants && i >= 0 && i < m->variant_count)
        return &m->variants[i];
    return &ZERO_VAR;
}

static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 1 if iv equals the 32-hex-digit string. */
static int iv_is(const unsigned char *iv, const char *hex32)
{
    int i;
    if (strlen(hex32) != 32)
        return 0;
    for (i = 0; i < 16; i++) {
        int hi = hexv(hex32[2 * i]), lo = hexv(hex32[2 * i + 1]);
        if (hi < 0 || lo < 0 || iv[i] != (unsigned char)((hi << 4) | lo))
            return 0;
    }
    return 1;
}

/* 1 if iv is seq big-endian, zero-padded into 16 bytes. */
static int iv_is_seq(const unsigned char *iv, unsigned long long seq)
{
    unsigned char want[16];
    int i;
    memset(want, 0, sizeof(want));
    for (i = 0; i < 8; i++)
        want[15 - i] = (unsigned char)((seq >> (8 * i)) & 0xFFu);
    return memcmp(iv, want, 16) == 0;
}

static int parse(const char *text, const char *base, M3u8 *m)
{
    return m3u8_parse(text, strlen(text), base, m);
}

/* ---- fixtures: captured from the BBC live HLS service ------------- */

#define BBC_MEDIA_URL "http://as-hls-uk.live.cf.md.bbci.co.uk/pool_01505109/live/uk/" \
                      "bbc_radio_one/bbc_radio_one.isml/bbc_radio_one-audio%3d96000.norewind.m3u8"
#define BBC_DIR       "http://as-hls-uk.live.cf.md.bbci.co.uk/pool_01505109/live/uk/" \
                      "bbc_radio_one/bbc_radio_one.isml/"
#define BBC_VARIANT   BBC_DIR "bbc_radio_one-audio%3d320000.norewind.m3u8"

static const char BBC_MEDIA[] =
    "#EXTM3U\n"
    "#EXT-X-VERSION:3\n"
    "## Created with Unified Streaming Platform  (version=1.13.5-30103)\n"
    "#EXT-X-MEDIA-SEQUENCE:279612681\n"
    "#EXT-X-INDEPENDENT-SEGMENTS\n"
    "#EXT-X-TARGETDURATION:6\n"
    "#USP-X-TIMESTAMP-MAP:MPEGTS=4220914592,LOCAL=2026-09-16T01:12:32Z\n"
    "#EXT-X-PROGRAM-DATE-TIME:2026-09-16T01:12:32Z\n"
    "#EXTINF:6.4, no desc\n"
    "bbc_radio_one-audio=96000-279612681.ts\n"
    "#EXTINF:6.4, no desc\n"
    "bbc_radio_one-audio=96000-279612682.ts\n";

static const char BBC_MASTER[] =
    "#EXTM3U\n"
    "#EXT-X-VERSION:3\n"
    "#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=339200,CODECS=\"mp4a.40.2\"\n"
    BBC_VARIANT "\n";

/* Quoted values containing commas: a strtok(",") split gets these wrong. */
static const char MASTER_CODECS[] =
    "#EXTM3U\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=1280000,CODECS=\"mp4a.40.2,avc1.4d401f\",RESOLUTION=640x360\n"
    "hi.m3u8\n"
    "#EXT-X-STREAM-INF:CODECS=\"mp4a.40.5,avc1.42c00d\",BANDWIDTH=96000\n"
    "lo.m3u8\n"
    "#EXT-X-STREAM-INF:PROGRAM-ID=1\n"
    "nobw.m3u8\n";

static const char ENC[] =
    "#EXTM3U\n"
    "#EXT-X-TARGETDURATION:10\n"
    "#EXT-X-MEDIA-SEQUENCE:5\n"
    "#EXT-X-KEY:METHOD=AES-128,URI=\"keys/k1.bin\"\n"
    "#EXTINF:10.0,\n"
    "s5.ts\n"
    "#EXT-X-KEY:METHOD=AES-128,URI=\"keys/k2.bin\",IV=0x0123456789ABCDEF0123456789abcdef\n"
    "#EXTINF:10.0,\n"
    "s6.ts\n"
    "#EXT-X-KEY:METHOD=NONE\n"
    "#EXTINF:10.0,\n"
    "s7.ts\n"
    "#EXT-X-KEY:METHOD=SAMPLE-AES,URI=\"keys/k3.bin\"\n"
    "#EXTINF:10.0,\n"
    "s8.ts\n"
    "#EXT-X-ENDLIST\n";

/* UTF-8 BOM, CRLF endings, a blank line, unknown tags, and all three URI
 * shapes. EXT-X-DISCONTINUITY-SEQUENCE must not read as a discontinuity. */
static const char MIXED[] =
    "\xEF\xBB\xBF#EXTM3U\r\n"
    "\r\n"
    "#EXT-X-TARGETDURATION:4\r\n"
    "#EXT-X-DISCONTINUITY-SEQUENCE:3\r\n"
    "#EXT-X-SOMETHING-NOT-INVENTED-YET:whatever=1,2\r\n"
    "#EXTINF:4.0,\r\n"
    "a/rel.ts\r\n"
    "#EXTINF:4.0,\r\n"
    "/root.ts\r\n"
    "#EXT-X-DISCONTINUITY\r\n"
    "#EXTINF:4.0,\r\n"
    "http://other.test/abs.ts\r\n";

int main(void)
{
    M3u8 m;

    /* ---- BBC media playlist ---------------------------------------- */
    CHECK("bbc_media_rc", parse(BBC_MEDIA, BBC_MEDIA_URL, &m) == 0);
    CHECK("bbc_media_kind", m.kind == VR_M3U8_MEDIA);
    CHECK("bbc_media_target", m.target_duration > 5.99 && m.target_duration < 6.01);
    CHECK("bbc_media_sequence", m.media_sequence == 279612681LL);
    CHECK("bbc_media_no_variants", m.variant_count == 0 && m.variants == NULL);
    CHECK("bbc_media_segcount", m.segment_count == 2);
    CHECK("bbc_media_duration", sg(&m, 0)->duration > 6.39 && sg(&m, 0)->duration < 6.41);
    CHECK("bbc_media_seq0", sg(&m, 0)->seq == 279612681LL);
    CHECK("bbc_media_seq1", sg(&m, 1)->seq == 279612682LL);
    chk_str("bbc_media_uri0", sg(&m, 0)->uri,
            BBC_DIR "bbc_radio_one-audio=96000-279612681.ts");
    chk_str("bbc_media_uri1", sg(&m, 1)->uri,
            BBC_DIR "bbc_radio_one-audio=96000-279612682.ts");
    CHECK("bbc_media_plain", sg(&m, 0)->encrypted == 0 && sg(&m, 0)->key_uri == NULL);
    CHECK("bbc_media_no_disc", sg(&m, 0)->discontinuity == 0 && sg(&m, 1)->discontinuity == 0);
    CHECK("bbc_media_no_endlist", m.endlist == 0);
    CHECK("bbc_media_pick_none", m3u8_pick_variant(&m) == -1);
    m3u8_free(&m);
    CHECK("bbc_media_free_clears", m.segments == NULL && m.segment_count == 0);
    m3u8_free(&m);   /* idempotent: ASan would trip on a double free */

    /* ---- BBC master playlist --------------------------------------- */
    CHECK("bbc_master_rc", parse(BBC_MASTER, BBC_MEDIA_URL, &m) == 0);
    CHECK("bbc_master_kind", m.kind == VR_M3U8_MASTER);
    CHECK("bbc_master_varcount", m.variant_count == 1);
    CHECK("bbc_master_no_segments", m.segment_count == 0 && m.segments == NULL);
    CHECK("bbc_master_bandwidth", vr(&m, 0)->bandwidth == 339200L);
    chk_str("bbc_master_codecs", vr(&m, 0)->codecs, "mp4a.40.2");
    chk_str("bbc_master_uri", vr(&m, 0)->uri, BBC_VARIANT);
    CHECK("bbc_master_pick", m3u8_pick_variant(&m) == 0);
    m3u8_free(&m);

    /* ---- attribute lists with quoted commas ------------------------- */
    CHECK("codecs_rc", parse(MASTER_CODECS, "http://ex.test/m/master.m3u8", &m) == 0);
    CHECK("codecs_kind", m.kind == VR_M3U8_MASTER);
    CHECK("codecs_varcount", m.variant_count == 3);
    CHECK("codecs_bw0", vr(&m, 0)->bandwidth == 1280000L);
    chk_str("codecs_quoted_comma0", vr(&m, 0)->codecs, "mp4a.40.2,avc1.4d401f");
    chk_str("codecs_uri0", vr(&m, 0)->uri, "http://ex.test/m/hi.m3u8");
    CHECK("codecs_bw1", vr(&m, 1)->bandwidth == 96000L);
    chk_str("codecs_quoted_comma1", vr(&m, 1)->codecs, "mp4a.40.5,avc1.42c00d");
    chk_str("codecs_uri1", vr(&m, 1)->uri, "http://ex.test/m/lo.m3u8");
    CHECK("codecs_missing_bw", vr(&m, 2)->bandwidth == 0L);
    chk_str("codecs_missing_codecs", vr(&m, 2)->codecs, "");
    chk_str("codecs_uri2", vr(&m, 2)->uri, "http://ex.test/m/nobw.m3u8");
    CHECK("codecs_pick_lowest", m3u8_pick_variant(&m) == 1);
    m3u8_free(&m);

    /* ---- EXT-X-KEY -------------------------------------------------- */
    CHECK("enc_rc", parse(ENC, "http://ex.test/hls/live.m3u8", &m) == 0);
    CHECK("enc_kind", m.kind == VR_M3U8_MEDIA);
    CHECK("enc_segcount", m.segment_count == 4);
    CHECK("enc_endlist", m.endlist == 1);

    CHECK("enc_seg0_encrypted", sg(&m, 0)->encrypted == 1);
    chk_str("enc_seg0_key", sg(&m, 0)->key_uri, "http://ex.test/hls/keys/k1.bin");
    CHECK("enc_seg0_seq", sg(&m, 0)->seq == 5);
    CHECK("enc_seg0_iv_from_seq", iv_is_seq(sg(&m, 0)->iv, 5));

    CHECK("enc_seg1_encrypted", sg(&m, 1)->encrypted == 1);
    chk_str("enc_seg1_key", sg(&m, 1)->key_uri, "http://ex.test/hls/keys/k2.bin");
    CHECK("enc_seg1_iv_explicit",
          iv_is(sg(&m, 1)->iv, "0123456789ABCDEF0123456789ABCDEF"));

    CHECK("enc_seg2_none_clears", sg(&m, 2)->encrypted == 0);
    CHECK("enc_seg2_key_null", sg(&m, 2)->key_uri == NULL);
    CHECK("enc_seg2_iv_from_seq", iv_is_seq(sg(&m, 2)->iv, 7));

    CHECK("enc_seg3_sample_aes_encrypted", sg(&m, 3)->encrypted == 1);
    CHECK("enc_seg3_sample_aes_key_null", sg(&m, 3)->key_uri == NULL);
    m3u8_free(&m);

    /* An explicit IV must not leak onto a later segment that has none. */
    {
        static const char IVRESET[] =
            "#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:100\n"
            "#EXT-X-KEY:METHOD=AES-128,URI=\"k\",IV=0xFF00FF00FF00FF00FF00FF00FF00FF00\n"
            "#EXTINF:2,\na.ts\n"
            "#EXT-X-KEY:METHOD=AES-128,URI=\"k\"\n"
            "#EXTINF:2,\nb.ts\n";
        CHECK("ivreset_rc", parse(IVRESET, "http://ex.test/p.m3u8", &m) == 0);
        CHECK("ivreset_explicit", iv_is(sg(&m, 0)->iv, "FF00FF00FF00FF00FF00FF00FF00FF00"));
        CHECK("ivreset_back_to_seq", iv_is_seq(sg(&m, 1)->iv, 101));
        m3u8_free(&m);
    }

    /* ---- BOM, CRLF, blank lines, unknown tags, URI shapes ----------- */
    CHECK("mixed_rc", parse(MIXED, "http://ex.test/x/y/play.m3u8", &m) == 0);
    CHECK("mixed_kind", m.kind == VR_M3U8_MEDIA);
    CHECK("mixed_target", m.target_duration > 3.99 && m.target_duration < 4.01);
    CHECK("mixed_segcount", m.segment_count == 3);
    CHECK("mixed_seq_default_zero", m.media_sequence == 0 && sg(&m, 0)->seq == 0);
    CHECK("mixed_seq_increments", sg(&m, 1)->seq == 1 && sg(&m, 2)->seq == 2);
    chk_str("mixed_uri_relative", sg(&m, 0)->uri, "http://ex.test/x/y/a/rel.ts");
    chk_str("mixed_uri_root_relative", sg(&m, 1)->uri, "http://ex.test/root.ts");
    chk_str("mixed_uri_absolute", sg(&m, 2)->uri, "http://other.test/abs.ts");
    CHECK("mixed_disc_seq_is_not_disc", sg(&m, 0)->discontinuity == 0 &&
                                        sg(&m, 1)->discontinuity == 0);
    CHECK("mixed_disc_set", sg(&m, 2)->discontinuity == 1);
    CHECK("mixed_no_endlist", m.endlist == 0);
    m3u8_free(&m);

    /* ---- rejections and degenerate input ---------------------------- */
    {
        const char *nohdr = "#EXT-X-TARGETDURATION:6\n#EXTINF:6,\nseg.ts\n";
        const char *html  = "<html><body>not a playlist</body></html>\n";
        CHECK("no_extm3u_rejected", parse(nohdr, "http://ex.test/p.m3u8", &m) == -1);
        CHECK("no_extm3u_out_safe", m.segment_count == 0 && m.segments == NULL);
        m3u8_free(&m);
        CHECK("html_rejected", parse(html, "http://ex.test/p.m3u8", &m) == -1);
        m3u8_free(&m);
        CHECK("empty_rejected", m3u8_parse("", 0, "http://ex.test/p.m3u8", &m) == -1);
        m3u8_free(&m);
        CHECK("blank_only_rejected", parse("\r\n   \n\n", "http://ex.test/p.m3u8", &m) == -1);
        m3u8_free(&m);
        CHECK("bom_only_rejected", parse("\xEF\xBB\xBF", "http://ex.test/p.m3u8", &m) == -1);
        m3u8_free(&m);
        CHECK("header_only_unknown",
              parse("#EXTM3U\n#EXT-X-VERSION:3\n", "http://ex.test/p.m3u8", &m) == 0);
        CHECK("header_only_kind", m.kind == VR_M3U8_UNKNOWN);
        CHECK("header_only_pick", m3u8_pick_variant(&m) == -1);
        m3u8_free(&m);
        CHECK("null_text_rejected", m3u8_parse(NULL, 0, "http://ex.test/p.m3u8", &m) == -1);
    }

    /* ---- a dropped URI line still consumes its sequence number ------ */
    {
        /* b.ts has no EXTINF so it cannot be kept, but the server still
         * numbered it: c.ts is segment 102, not 101. Getting this wrong gives
         * every later segment the wrong default IV. */
        static const char DROPSEQ[] =
            "#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXT-X-MEDIA-SEQUENCE:100\n"
            "#EXTINF:6,\na.ts\nb.ts\n#EXTINF:6,\nc.ts\n";
        CHECK("dropseq_rc", parse(DROPSEQ, "http://ex.test/p.m3u8", &m) == 0);
        CHECK("dropseq_segcount", m.segment_count == 2);
        CHECK("dropseq_first", sg(&m, 0)->seq == 100);
        CHECK("dropseq_gap_counted", sg(&m, 1)->seq == 102);
        chk_str("dropseq_uri1", sg(&m, 1)->uri, "http://ex.test/c.ts");
        CHECK("dropseq_iv_follows", iv_is_seq(sg(&m, 1)->iv, 102));
        m3u8_free(&m);
    }

    /* ---- spaces around '=' in an attribute list --------------------- */
    {
        static const char KEYSPACE[] =
            "#EXTM3U\n#EXT-X-TARGETDURATION:10\n#EXT-X-MEDIA-SEQUENCE:1\n"
            "#EXT-X-KEY: METHOD = AES-128 , URI = \"k.bin\"\n"
            "#EXTINF:10,\ns.ts\n";
        CHECK("keyspace_rc", parse(KEYSPACE, "http://ex.test/hls/live.m3u8", &m) == 0);
        CHECK("keyspace_encrypted", sg(&m, 0)->encrypted == 1);
        chk_str("keyspace_key_uri", sg(&m, 0)->key_uri, "http://ex.test/hls/k.bin");
        m3u8_free(&m);
    }

    /* ---- EXT-X-MEDIA-SEQUENCE out of range or negative -------------- */
    {
        static const char SEQHUGE[] =
            "#EXTM3U\n#EXT-X-TARGETDURATION:6\n"
            "#EXT-X-MEDIA-SEQUENCE:99999999999999999999999\n"
            "#EXTINF:6,\na.ts\n#EXTINF:6,\nb.ts\n";
        static const char SEQNEG[] =
            "#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXT-X-MEDIA-SEQUENCE:-3\n"
            "#EXTINF:6,\na.ts\n";
        CHECK("seqhuge_rc", parse(SEQHUGE, "http://ex.test/p.m3u8", &m) == 0);
        CHECK("seqhuge_ignored", m.media_sequence == 0);
        CHECK("seqhuge_seq0", sg(&m, 0)->seq == 0);
        CHECK("seqhuge_seq1", sg(&m, 1)->seq == 1);
        m3u8_free(&m);
        CHECK("seqneg_rc", parse(SEQNEG, "http://ex.test/p.m3u8", &m) == 0);
        CHECK("seqneg_ignored", m.media_sequence == 0);
        CHECK("seqneg_iv_not_negative", iv_is_seq(sg(&m, 0)->iv, 0));
        m3u8_free(&m);
    }

    /* ---- a second EXT-X-MEDIA-SEQUENCE mid-playlist is ignored ------ */
    {
        /* RFC 8216 4.3.3.2: it must precede the first Media Segment. Honouring
         * a later one makes sequence numbers go backwards inside one playlist. */
        static const char SEQTWICE[] =
            "#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:10\n"
            "#EXTINF:2,\na.ts\n"
            "#EXT-X-MEDIA-SEQUENCE:1\n"
            "#EXTINF:2,\nb.ts\n";
        CHECK("seqtwice_rc", parse(SEQTWICE, "http://ex.test/p.m3u8", &m) == 0);
        CHECK("seqtwice_first_wins", m.media_sequence == 10);
        CHECK("seqtwice_segcount", m.segment_count == 2);
        CHECK("seqtwice_monotonic", sg(&m, 0)->seq == 10 && sg(&m, 1)->seq == 11);
        m3u8_free(&m);
    }

    /* ---- an embedded NUL is a truncated response, not a short playlist */
    {
        static const char NULMID[] =
            "#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXTINF:6,\na.ts\n"
            "\0#EXTINF:6,\nb.ts\n";
        CHECK("nulmid_rejected",
              m3u8_parse(NULMID, sizeof(NULMID) - 1, "http://ex.test/p.m3u8", &m) == -1);
        m3u8_free(&m);
    }

    /* ---- an over-long EXT-X-KEY URI is refused, not truncated ------- */
    {
        char longkey[2600];
        char path[2100];
        size_t i;

        for (i = 0; i < sizeof(path) - 1; i++)
            path[i] = 'k';
        path[sizeof(path) - 1] = '\0';
        snprintf(longkey, sizeof(longkey),
                 "#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXT-X-MEDIA-SEQUENCE:1\n"
                 "#EXT-X-KEY:METHOD=AES-128,URI=\"%s\"\n"
                 "#EXTINF:6,\ns.ts\n", path);
        CHECK("longkey_rc", parse(longkey, "http://ex.test/hls/live.m3u8", &m) == 0);
        CHECK("longkey_encrypted", sg(&m, 0)->encrypted == 1);
        CHECK("longkey_refused", sg(&m, 0)->key_uri == NULL);
        m3u8_free(&m);
    }

    /* ---- an over-long IV is a malformed key, not a silent fallback -- */
    {
        static const char LONGIV[] =
            "#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXT-X-MEDIA-SEQUENCE:7\n"
            "#EXT-X-KEY:METHOD=AES-128,URI=\"k.bin\",IV=0x"
            "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"
            "0123456789ABCDEF0123456789ABCDEF\n"
            "#EXTINF:6,\ns.ts\n";
        CHECK("longiv_rc", parse(LONGIV, "http://ex.test/hls/live.m3u8", &m) == 0);
        CHECK("longiv_encrypted", sg(&m, 0)->encrypted == 1);
        CHECK("longiv_refused", sg(&m, 0)->key_uri == NULL);
        m3u8_free(&m);
    }

    /* ---- an unrecognised METHOD names itself ------------------------ */
    {
        /* RFC 8216 enumerated-strings are case-sensitive, so "aes-128" is
         * correctly refused. The value has to reach the caller so the device
         * can say which one it was instead of just "unsupported". */
        static const char LOWERM[] =
            "#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXT-X-MEDIA-SEQUENCE:1\n"
            "#EXT-X-KEY:METHOD=aes-128,URI=\"k.bin\"\n"
            "#EXTINF:6,\ns.ts\n";
        CHECK("lowerm_rc", parse(LOWERM, "http://ex.test/hls/live.m3u8", &m) == 0);
        CHECK("lowerm_encrypted", sg(&m, 0)->encrypted == 1);
        CHECK("lowerm_no_key", sg(&m, 0)->key_uri == NULL);
        chk_str("lowerm_method_named", sg(&m, 0)->key_method, "aes-128");
        m3u8_free(&m);
    }

    printf("test_m3u8: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
