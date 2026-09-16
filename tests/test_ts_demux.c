/* Host unit tests for the hand-written MPEG-TS demuxer.
 *
 * Every byte fed to the demuxer here is synthesised in C: no downloaded
 * segment, no fixtures on disk, so the suite is deterministic and offline.
 * The shapes mirror the measured BBC Radio 1 segment - PAT on PID 0, a PMT,
 * and one audio elementary stream - but the bytes are ours. */

#include "ts_demux.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0, g_pass = 0;

#define CHECK(name, cond) do { \
    if (cond) { printf("PASS %s\n", name); g_pass++; } \
    else { printf("FAIL %s (%s:%d)\n", name, __FILE__, __LINE__); g_fail++; } \
} while (0)

#define PKT       188
#define PMT_PID   0x0020          /* 32  - as measured in the real segment */
#define AUD_PID   0x0022          /* 34 */
#define VID_PID   0x0100

/* ---------------------------------------------------------------- sink -- */

typedef struct {
    unsigned char b[1 << 16];
    size_t        n;
    int           calls;
    int           stop_after;     /* 0 = never stop */
    int           overflow;
} Sink;

static int sink_fn(void *user, const unsigned char *es, size_t len)
{
    Sink *s = (Sink *)user;
    s->calls++;
    if (s->n + len > sizeof s->b) { s->overflow = 1; return 1; }
    memcpy(s->b + s->n, es, len);
    s->n += len;
    if (s->stop_after && s->calls >= s->stop_after) return 1;
    return 0;
}

static void sink_init(Sink *s) { memset(s, 0, sizeof *s); }

/* ------------------------------------------------------------ builders -- */

static unsigned char strm[1 << 16];
static size_t        slen;

static void s_reset(void) { slen = 0; memset(strm, 0, sizeof strm); }

static unsigned char *s_pkt(void)
{
    unsigned char *p = strm + slen;
    slen += PKT;
    return p;
}

/* One 188-byte packet. aflen < 0 means no adaptation field; otherwise an
 * adaptation field of exactly aflen bytes follows its length byte. The
 * payload must fill the packet exactly - the caller does that arithmetic. */
static void ts_pkt(unsigned char *p, int pid, int pusi, int cc, int aflen,
                   const unsigned char *pay, size_t plen)
{
    size_t off = 4;
    int afc = (aflen >= 0) ? 3 : 1;

    p[0] = 0x47;
    p[1] = (unsigned char)((pusi ? 0x40 : 0x00) | ((pid >> 8) & 0x1F));
    p[2] = (unsigned char)(pid & 0xFF);
    p[3] = (unsigned char)((afc << 4) | (cc & 0x0F));
    if (aflen >= 0) {
        p[4] = (unsigned char)aflen;
        if (aflen > 0) {
            p[5] = 0x00;                       /* no adaptation flags */
            memset(p + 6, 0xFF, (size_t)aflen - 1);
        }
        off = 5 + (size_t)aflen;
    }
    if (off + plen != PKT) {
        printf("FAIL builder_packet_size off=%zu plen=%zu\n", off, plen);
        g_fail++;
        return;
    }
    memcpy(p + off, pay, plen);
}

/* Adaptation-field-only packet (afc = 2): carries no payload at all. */
static void ts_pkt_af_only(unsigned char *p, int pid, int cc)
{
    p[0] = 0x47;
    p[1] = (unsigned char)((pid >> 8) & 0x1F);
    p[2] = (unsigned char)(pid & 0xFF);
    p[3] = (unsigned char)((2 << 4) | (cc & 0x0F));
    p[4] = PKT - 5;                            /* adaptation field fills it */
    p[5] = 0x00;
    memset(p + 6, 0xFF, PKT - 6);
}

/* Like ts_pkt() with an adaptation field, but with discontinuity_indicator
 * set: the sender is telling us the break is deliberate, not packet loss. */
static void ts_pkt_disc(unsigned char *p, int pid, int pusi, int cc, int aflen,
                        const unsigned char *pay, size_t plen)
{
    ts_pkt(p, pid, pusi, cc, aflen, pay, plen);
    if (aflen > 0)
        p[5] = 0x80;
}

static void ts_pkt_null(unsigned char *p)
{
    unsigned char pay[184];
    memset(pay, 0xFF, sizeof pay);
    ts_pkt(p, 0x1FFF, 0, 0, -1, pay, sizeof pay);
}

/* A PSI packet: pointer_field, `ptr` filler bytes, the section, 0xFF stuffing. */
static void psi_pkt(unsigned char *p, int pid, int cc, int ptr,
                    const unsigned char *sec, size_t seclen)
{
    unsigned char pay[184];
    memset(pay, 0xFF, sizeof pay);
    pay[0] = (unsigned char)ptr;
    memset(pay + 1, 0xAB, (size_t)ptr);        /* tail of an imaginary section */
    memcpy(pay + 1 + (size_t)ptr, sec, seclen);
    ts_pkt(p, pid, 1, cc, -1, pay, sizeof pay);
}

/* PAT with a program_number 0 (NIT) entry first, which must be skipped. */
static size_t build_pat(unsigned char *s, int pmt_pid)
{
    size_t seclen = 5 + 4 + 4 + 4;             /* header tail + 2 entries + CRC */
    s[0] = 0x00;
    s[1] = (unsigned char)(0xB0 | ((seclen >> 8) & 0x0F));
    s[2] = (unsigned char)(seclen & 0xFF);
    s[3] = 0x00; s[4] = 0x01;                  /* transport_stream_id */
    s[5] = 0xC1; s[6] = 0x00; s[7] = 0x00;     /* version 0, current, sec 0/0 */
    s[8] = 0x00; s[9] = 0x00;                  /* program_number 0 = NIT */
    s[10] = 0xE0; s[11] = 0x10;                /*   -> PID 0x0010, ignore */
    s[12] = 0x00; s[13] = 0x01;                /* program_number 1 */
    s[14] = (unsigned char)(0xE0 | (pmt_pid >> 8));
    s[15] = (unsigned char)(pmt_pid & 0xFF);
    memset(s + 16, 0xAA, 4);                   /* CRC32 - not verified */
    return 3 + seclen;
}

/* PMT with a 4-byte program_info, a video stream with 3 bytes of ES_info
 * ahead of the audio stream, then the audio stream. Both must be stepped
 * over correctly for the audio entry to be found. */
static size_t build_pmt(unsigned char *s, int aud_pid, int stype)
{
    size_t pil = 4, esil_v = 3;
    size_t seclen = 9 + pil + (5 + esil_v) + (5 + 0) + 4;
    size_t i;

    s[0] = 0x02;
    s[1] = (unsigned char)(0xB0 | ((seclen >> 8) & 0x0F));
    s[2] = (unsigned char)(seclen & 0xFF);
    s[3] = 0x00; s[4] = 0x01;                  /* program_number */
    s[5] = 0xC1; s[6] = 0x00; s[7] = 0x00;
    s[8] = 0xE0; s[9] = 0x21;                  /* PCR_PID */
    s[10] = (unsigned char)(0xF0 | ((pil >> 8) & 0x0F));
    s[11] = (unsigned char)(pil & 0xFF);
    i = 12;
    memset(s + i, 0x5A, pil); i += pil;        /* program_info descriptors */

    s[i++] = 0x1B;                             /* H.264 video - not audio */
    s[i++] = (unsigned char)(0xE0 | (VID_PID >> 8));
    s[i++] = (unsigned char)(VID_PID & 0xFF);
    s[i++] = (unsigned char)(0xF0 | ((esil_v >> 8) & 0x0F));
    s[i++] = (unsigned char)(esil_v & 0xFF);
    memset(s + i, 0x6B, esil_v); i += esil_v;

    s[i++] = (unsigned char)stype;
    s[i++] = (unsigned char)(0xE0 | (aud_pid >> 8));
    s[i++] = (unsigned char)(aud_pid & 0xFF);
    s[i++] = 0xF0; s[i++] = 0x00;

    memset(s + i, 0xAA, 4); i += 4;            /* CRC32 */
    return i;
}

static size_t pes_hdr_build(unsigned char *h, size_t es_total, int hdrlen)
{
    size_t plen = es_total + 3 + (size_t)hdrlen;
    int i;
    h[0] = 0x00; h[1] = 0x00; h[2] = 0x01;
    h[3] = 0xC0;                               /* audio stream_id */
    h[4] = (unsigned char)((plen >> 8) & 0xFF);
    h[5] = (unsigned char)(plen & 0xFF);
    h[6] = 0x80;                               /* '10' marker, nothing set */
    h[7] = (unsigned char)(hdrlen ? 0x80 : 0x00);   /* PTS present or not */
    h[8] = (unsigned char)hdrlen;
    for (i = 0; i < hdrlen; i++)
        h[9 + i] = 0x21;                       /* optional header bytes */
    return 9 + (size_t)hdrlen;
}

/* Append the whole of `es` as one PES on `pid`. af_every >= 0 puts an
 * adaptation field of that length on every continuation packet. */
static void emit_es(int pid, int *cc, const unsigned char *es, size_t n,
                    int hdrlen, int af_every)
{
    unsigned char pay[184];
    size_t hl = pes_hdr_build(pay, n, hdrlen);
    size_t take = 184 - hl;
    size_t off;

    if (take > n) take = n;
    memcpy(pay + hl, es, take);
    ts_pkt(s_pkt(), pid, 1, *cc, (int)(184 - hl - take) - 1, pay, hl + take);
    *cc = (*cc + 1) & 0x0F;

    for (off = take; off < n; ) {
        size_t room = (af_every >= 0) ? (size_t)(183 - af_every) : 184;
        size_t k = n - off;
        if (k > room) k = room;
        if (k == 184)
            ts_pkt(s_pkt(), pid, 0, *cc, -1, es + off, k);
        else
            ts_pkt(s_pkt(), pid, 0, *cc, (int)(183 - k), es + off, k);
        *cc = (*cc + 1) & 0x0F;
        off += k;
    }
}

/* ts_pkt() is given aflen = -1 only when the payload is a full 184; the
 * expression above yields -1 exactly then. */

static unsigned char es_src[4096];

static void fill_es(size_t n)
{
    unsigned x = 0xC0FFEEu;
    size_t i;
    for (i = 0; i < n; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        es_src[i] = (unsigned char)x;
    }
    /* Plant 0x47 bytes inside the elementary stream: a demuxer that hunts for
     * sync bytes inside a payload, or tries to align to ADTS, breaks here. */
    for (i = 7; i < n; i += 61)
        es_src[i] = 0x47;
    for (i = 40; i + 1 < n; i += 97) {
        es_src[i] = 0xFF; es_src[i + 1] = 0xF1;   /* stray ADTS-looking sync */
    }
}

/* Build PAT + PMT + one PES carrying es_src[0..n). */
static void build_stream(size_t n, int stype, int hdrlen, int af_every, int ptr)
{
    unsigned char sec[256];
    size_t sl;
    int cc = 0;

    s_reset();
    sl = build_pat(sec, PMT_PID);
    psi_pkt(s_pkt(), 0x0000, 0, ptr, sec, sl);
    sl = build_pmt(sec, AUD_PID, stype);
    psi_pkt(s_pkt(), PMT_PID, 0, ptr, sec, sl);
    emit_es(AUD_PID, &cc, es_src, n, hdrlen, af_every);
}

/* --------------------------------------------------------------- cases -- */

static int run_whole(const unsigned char *d, size_t n, Sink *s, TsDemux **out)
{
    TsDemux *t = ts_demux_open();
    int rc;
    sink_init(s);
    rc = ts_demux_feed(t, d, n, sink_fn, s);
    if (out) *out = t; else ts_demux_close(t);
    return rc;
}

static int es_matches(const Sink *s, size_t n)
{
    return s->n == n && memcmp(s->b, es_src, n) == 0;
}

int main(void)
{
    static unsigned char big[70000];
    Sink sink;
    TsDemux *t;
    size_t n = 727;                /* 175 in the PUSI packet + 3 * 184 */
    size_t i;

    fill_es(sizeof es_src);

    /* ---- PAT/PMT discovery, stream type, exact ES ------------------ */
    build_stream(n, 0x0F, 0, -1, 0);
    CHECK("stream_built_6_packets", slen == 6 * PKT);   /* 2 PSI + 4 audio */
    CHECK("feed_ok", run_whole(strm, slen, &sink, &t) == 0);
    CHECK("stream_type_0F", ts_demux_stream_type(t) == 0x0F);
    CHECK("es_exact", es_matches(&sink, n));
    CHECK("no_cc_errors", ts_demux_cc_errors(t) == 0);

    /* ---- reset clears PAT/PMT and the stream type ------------------ */
    ts_demux_reset(t);
    CHECK("reset_clears_stream_type", ts_demux_stream_type(t) == 0);
    {
        Sink s2;
        sink_init(&s2);
        build_stream(500, 0x03, 0, -1, 0);
        CHECK("reset_refeed_ok", ts_demux_feed(t, strm, slen, sink_fn, &s2) == 0);
        CHECK("reset_refeed_type_03", ts_demux_stream_type(t) == 0x03);
        CHECK("reset_refeed_es_exact", es_matches(&s2, 500));
    }
    ts_demux_close(t);

    /* ---- the other audio stream types ------------------------------ */
    {
        int types[3] = { 0x04, 0x0F, 0x03 };
        int all = 1, k;
        for (k = 0; k < 3; k++) {
            build_stream(400, types[k], 0, -1, 0);
            run_whole(strm, slen, &sink, &t);
            if (ts_demux_stream_type(t) != types[k] || !es_matches(&sink, 400))
                all = 0;
            ts_demux_close(t);
        }
        CHECK("stream_types_04_0F_03", all);
    }

    /* A PMT naming only a video stream must leave stream_type at 0. */
    {
        unsigned char sec[256];
        size_t sl;
        s_reset();
        sl = build_pat(sec, PMT_PID);
        psi_pkt(s_pkt(), 0x0000, 0, 0, sec, sl);
        sl = build_pmt(sec, AUD_PID, 0x1B);        /* "audio" slot is video too */
        psi_pkt(s_pkt(), PMT_PID, 0, 0, sec, sl);
        psi_pkt(s_pkt(), 0x0000, 1, 0, sec, 20);   /* filler packets to sync on */
        run_whole(strm, slen, &sink, &t);
        CHECK("no_audio_stream_type_zero", ts_demux_stream_type(t) == 0);
        CHECK("no_audio_no_es", sink.n == 0);
        ts_demux_close(t);
    }

    /* ---- pointer_field with filler ahead of the section ------------ */
    build_stream(n, 0x0F, 0, -1, 23);
    run_whole(strm, slen, &sink, &t);
    CHECK("pointer_field_23", ts_demux_stream_type(t) == 0x0F && es_matches(&sink, n));
    ts_demux_close(t);

    /* ---- PES header with optional fields --------------------------- */
    build_stream(533, 0x0F, 10, -1, 0);            /* 165 + 2 * 184 */
    CHECK("pes_opt_built", slen == 5 * PKT);
    run_whole(strm, slen, &sink, &t);
    CHECK("pes_with_optional_fields", es_matches(&sink, 533));
    ts_demux_close(t);

    build_stream(1500, 0x0F, 36, -1, 0);           /* a long optional header */
    run_whole(strm, slen, &sink, &t);
    CHECK("pes_long_optional_header", es_matches(&sink, 1500));
    ts_demux_close(t);

    /* ---- adaptation field on every continuation packet ------------- */
    build_stream(1000, 0x0F, 0, 20, 0);
    run_whole(strm, slen, &sink, &t);
    CHECK("adaptation_field_skipped", es_matches(&sink, 1000));
    ts_demux_close(t);

    build_stream(1000, 0x0F, 0, 0, 0);             /* aflen 0: length byte only */
    run_whole(strm, slen, &sink, &t);
    CHECK("adaptation_field_len_zero", es_matches(&sink, 1000));
    ts_demux_close(t);

    /* An adaptation-only packet (afc=2) carries no payload and does not
     * advance the continuity counter. */
    {
        unsigned char sec[256];
        size_t sl;
        int cc = 0;
        s_reset();
        sl = build_pat(sec, PMT_PID);   psi_pkt(s_pkt(), 0x0000, 0, 0, sec, sl);
        sl = build_pmt(sec, AUD_PID, 0x0F); psi_pkt(s_pkt(), PMT_PID, 0, 0, sec, sl);
        emit_es(AUD_PID, &cc, es_src, 175, 0, -1);      /* one PUSI packet */
        ts_pkt_af_only(s_pkt(), AUD_PID, cc);           /* no payload, cc unchanged */
        ts_pkt_null(s_pkt());                           /* PID 0x1FFF, ignored */
        /* continuation of the same PES, cc continues from where it was */
        ts_pkt(s_pkt(), AUD_PID, 0, cc, -1, es_src + 175, 184);
        cc = (cc + 1) & 0x0F;
        ts_pkt(s_pkt(), AUD_PID, 0, cc, -1, es_src + 359, 184);
        run_whole(strm, slen, &sink, &t);
        CHECK("af_only_and_null_pid_ignored", es_matches(&sink, 543));
        CHECK("af_only_no_cc_error", ts_demux_cc_errors(t) == 0);
        ts_demux_close(t);
    }

    /* ---- split across two feed() calls at many offsets -------------- */
    build_stream(n, 0x0F, 0, -1, 0);
    {
        size_t cuts[12] = { 1, 2, 3, 4, 100, 187, 188, 189, 300, 376, 500, 939 };
        int all = 1, bad = -1;
        size_t k;
        for (k = 0; k < 12; k++) {
            Sink s2;
            TsDemux *d = ts_demux_open();
            sink_init(&s2);
            if (ts_demux_feed(d, strm, cuts[k], sink_fn, &s2) != 0) all = 0;
            if (ts_demux_feed(d, strm + cuts[k], slen - cuts[k], sink_fn, &s2) != 0) all = 0;
            if (!es_matches(&s2, n) || ts_demux_stream_type(d) != 0x0F) {
                all = 0; if (bad < 0) bad = (int)cuts[k];
            }
            ts_demux_close(d);
        }
        if (!all) printf("  (first bad split offset: %d)\n", bad);
        CHECK("split_at_two_call_boundaries", all);
    }

    /* ---- fed one byte at a time, and in odd chunk sizes ------------- */
    {
        size_t sizes[8] = { 1, 2, 3, 7, 63, 127, 188, 501 };
        int all = 1;
        size_t k;
        for (k = 0; k < 8; k++) {
            Sink s2;
            TsDemux *d = ts_demux_open();
            size_t off;
            sink_init(&s2);
            for (off = 0; off < slen; off += sizes[k]) {
                size_t c = slen - off;
                if (c > sizes[k]) c = sizes[k];
                if (ts_demux_feed(d, strm + off, c, sink_fn, &s2) != 0) all = 0;
            }
            if (!es_matches(&s2, n)) { all = 0; printf("  (chunk %zu gave %zu bytes)\n", sizes[k], s2.n); }
            ts_demux_close(d);
        }
        CHECK("chunked_feeds", all);
    }

    /* ---- garbage before the stream: resync ------------------------- */
    {
        static unsigned char g[1 << 16];
        size_t glen = 337;
        memset(g, 0x11, glen);
        memcpy(g + glen, strm, slen);
        run_whole(g, glen + slen, &sink, &t);
        CHECK("resync_after_leading_garbage",
              ts_demux_stream_type(t) == 0x0F && es_matches(&sink, n));
        ts_demux_close(t);
    }

    /* ---- garbage injected mid-stream: resync, nothing lost ---------- */
    {
        static unsigned char g[1 << 16];
        size_t at = 2 * PKT;                   /* between the PMT and the PES */
        size_t glen = 37, tot;
        memcpy(g, strm, at);
        memset(g + at, 0x11, glen);
        memcpy(g + at + glen, strm + at, slen - at);
        tot = slen + glen;
        run_whole(g, tot, &sink, &t);
        CHECK("resync_after_mid_stream_garbage",
              ts_demux_stream_type(t) == 0x0F && es_matches(&sink, n));
        ts_demux_close(t);
    }

    /* Resync accepts a candidate only after confirming the packet behind it,
     * so at that moment the confirmation packet is already buffered. It must
     * be emitted inside the same feed() call rather than stranded until the
     * next one: feed exactly up to the end of that confirmation packet. */
    {
        static unsigned char g[1 << 16];
        size_t at = 2 * PKT, glen = 37;
        size_t upto = at + glen + 2 * PKT;     /* candidate + confirmation */
        size_t tot = slen + glen;
        TsDemux *d = ts_demux_open();
        Sink s2;
        memcpy(g, strm, at);
        memset(g + at, 0x11, glen);
        memcpy(g + at + glen, strm + at, slen - at);
        sink_init(&s2);
        CHECK("resync_drain_feed_ok", ts_demux_feed(d, g, upto, sink_fn, &s2) == 0);
        CHECK("resync_drains_confirmation_packet",
              s2.n == 175 + 184 && memcmp(s2.b, es_src, 175 + 184) == 0);
        ts_demux_feed(d, g + upto, tot - upto, sink_fn, &s2);
        CHECK("resync_drain_rest_follows", es_matches(&s2, n));
        ts_demux_close(d);
    }

    /* ---- a stray 0x47 in the garbage must not be taken as sync ------
     * The decoy at g[50] has a plausible-looking header (TEI clear, a
     * non-zero adaptation_field_control), so only checking the NEXT packet
     * also lands on 0x47 rejects it. If it is accepted, the real PAT packet
     * is swallowed as the decoy's payload and, since the PAT appears exactly
     * once in this stream, stream_type stays 0 and no ES is emitted. */
    {
        static unsigned char g[1 << 16];
        size_t glen = 100;
        memset(g, 0x11, glen);
        g[50] = 0x47;
        memcpy(g + glen, strm, slen);
        CHECK("decoy_confirm_byte_is_not_47", g[50 + PKT] != 0x47);
        run_whole(g, glen + slen, &sink, &t);
        CHECK("stray_47_not_false_sync",
              ts_demux_stream_type(t) == 0x0F && es_matches(&sink, n));
        ts_demux_close(t);
    }

    /* ---- continuity counter error counted -------------------------- */
    {
        static unsigned char g[1 << 16];
        size_t at = 3 * PKT;                   /* drop one PES continuation */
        memcpy(g, strm, at);
        memcpy(g + at, strm + at + PKT, slen - at - PKT);
        run_whole(g, slen - PKT, &sink, &t);
        CHECK("cc_error_counted", ts_demux_cc_errors(t) == 1);
        /* The packets after the gap are NOT spliced onto the ones before it:
         * the missing 184 bytes are the middle of an AAC frame, and joining
         * the two halves just hands the decoder a frame that is wrong in a
         * way it cannot see. Only the bytes up to the gap are emitted, and
         * the stream picks up again at the next PUSI. */
        CHECK("cc_error_stops_at_the_gap", sink.n == 175);
        CHECK("cc_error_bytes_are_the_right_ones",
              memcmp(sink.b, es_src, 175) == 0);
        ts_demux_close(t);
    }

    /* two drops in a row count as two discontinuities */
    {
        static unsigned char g[1 << 16];
        size_t at = 3 * PKT;
        build_stream(175 + 6 * 184, 0x0F, 0, -1, 0);
        memcpy(g, strm, at);
        memcpy(g + at, strm + at + PKT, slen - at - PKT);      /* drop one */
        {
            size_t l1 = slen - PKT;
            static unsigned char h[1 << 16];
            size_t at2 = at + 2 * PKT;
            memcpy(h, g, at2);
            memcpy(h + at2, g + at2 + PKT, l1 - at2 - PKT);    /* drop another */
            run_whole(h, l1 - PKT, &sink, &t);
            CHECK("two_cc_errors", ts_demux_cc_errors(t) == 2);
            ts_demux_close(t);
        }
    }

    /* ---- callback asking to stop ----------------------------------- */
    build_stream(n, 0x0F, 0, -1, 0);
    {
        TsDemux *d = ts_demux_open();
        Sink s2;
        sink_init(&s2);
        s2.stop_after = 1;
        CHECK("stop_returns_one", ts_demux_feed(d, strm, slen, sink_fn, &s2) == 1);
        CHECK("stop_delivered_one_chunk", s2.calls == 1 && s2.n == 175);
        ts_demux_close(d);
    }
    {
        TsDemux *d = ts_demux_open();
        Sink s2;
        sink_init(&s2);
        s2.stop_after = 3;
        CHECK("stop_on_third_chunk", ts_demux_feed(d, strm, slen, sink_fn, &s2) == 1);
        CHECK("stop_third_byte_count", s2.n == 175 + 2 * 184);
        ts_demux_close(d);
    }

    /* ---- pathological input ---------------------------------------- */
    {
        TsDemux *d = ts_demux_open();
        memset(big, 0x11, sizeof big);
        CHECK("long_garbage_gives_minus_one",
              ts_demux_feed(d, big, sizeof big, sink_fn, &sink) == -1);
        ts_demux_close(d);
    }
    {
        TsDemux *d = ts_demux_open();
        sink_init(&sink);
        CHECK("empty_feed_ok", ts_demux_feed(d, strm, 0, sink_fn, &sink) == 0);
        CHECK("type_zero_before_pmt", ts_demux_stream_type(d) == 0);
        CHECK("cc_errors_zero_before_data", ts_demux_cc_errors(d) == 0);
        ts_demux_close(d);
    }
    /* Continuation packets before any PUSI: nothing to emit, no crash. */
    {
        TsDemux *d = ts_demux_open();
        unsigned char sec[256];
        size_t sl;
        sink_init(&sink);
        s_reset();
        sl = build_pat(sec, PMT_PID);   psi_pkt(s_pkt(), 0x0000, 0, 0, sec, sl);
        sl = build_pmt(sec, AUD_PID, 0x0F); psi_pkt(s_pkt(), PMT_PID, 0, 0, sec, sl);
        ts_pkt(s_pkt(), AUD_PID, 0, 0, -1, es_src, 184);       /* no PUSI */
        ts_pkt(s_pkt(), AUD_PID, 0, 1, -1, es_src + 184, 184);
        CHECK("mid_pes_without_pusi_ok", ts_demux_feed(d, strm, slen, sink_fn, &sink) == 0);
        CHECK("mid_pes_without_pusi_emits_nothing", sink.n == 0);
        ts_demux_close(d);
    }
    /* A PES whose start code is wrong must not be emitted. */
    {
        TsDemux *d = ts_demux_open();
        build_stream(n, 0x0F, 0, -1, 0);
        strm[2 * PKT + 4 + 3] = 0xE0;          /* stream_id 0xE0 = video */
        sink_init(&sink);
        ts_demux_feed(d, strm, slen, sink_fn, &sink);
        CHECK("wrong_stream_id_not_emitted", sink.n == 0);
        ts_demux_close(d);
        d = ts_demux_open();
        build_stream(n, 0x0F, 0, -1, 0);
        strm[2 * PKT + 4 + 2] = 0x02;          /* start code 00 00 02 */
        sink_init(&sink);
        ts_demux_feed(d, strm, slen, sink_fn, &sink);
        CHECK("bad_start_code_not_emitted", sink.n == 0);
        ts_demux_close(d);
    }

    /* ---- the resync budget is per call, not per object --------------- */
    /* One segment that is not TS at all (an HTML error page, an fMP4 chunk)
     * must not poison the demuxer for the rest of the session. */
    {
        Sink s2;
        TsDemux *d = ts_demux_open();
        memset(big, 0x11, sizeof big);
        sink_init(&s2);
        CHECK("junk_segment_gives_minus_one",
              ts_demux_feed(d, big, sizeof big, sink_fn, &s2) == -1);
        build_stream(359, 0x0F, 0, -1, 0);
        sink_init(&s2);
        CHECK("valid_ts_after_junk_ok",
              ts_demux_feed(d, strm, slen, sink_fn, &s2) == 0);
        CHECK("valid_ts_after_junk_es", es_matches(&s2, 359));
        CHECK("valid_ts_after_junk_type", ts_demux_stream_type(d) == 0x0F);
        ts_demux_close(d);
    }
    /* The same, spread over two junk feeds: neither call exceeds the budget,
     * so neither may fail, and the stream after them still plays. */
    {
        Sink s2;
        TsDemux *d = ts_demux_open();
        memset(big, 0x11, sizeof big);
        sink_init(&s2);
        CHECK("junk_half_one_ok", ts_demux_feed(d, big, 40000, sink_fn, &s2) == 0);
        CHECK("junk_half_two_ok", ts_demux_feed(d, big, 40000, sink_fn, &s2) == 0);
        build_stream(359, 0x0F, 0, -1, 0);
        sink_init(&s2);
        CHECK("valid_ts_after_split_junk_es",
              ts_demux_feed(d, strm, slen, sink_fn, &s2) == 0 && es_matches(&s2, 359));
        ts_demux_close(d);
    }

    /* ---- a continuity break must not splice the two sides together ---- */
    {
        unsigned char sec[256], pay[184];
        size_t sl, hl;
        TsDemux *d = ts_demux_open();
        s_reset();
        sl = build_pat(sec, PMT_PID);       psi_pkt(s_pkt(), 0x0000, 0, 0, sec, sl);
        sl = build_pmt(sec, AUD_PID, 0x0F); psi_pkt(s_pkt(), PMT_PID, 0, 0, sec, sl);
        hl = pes_hdr_build(pay, 600, 0);
        memcpy(pay + hl, es_src, 184 - hl);
        ts_pkt(s_pkt(), AUD_PID, 1, 0, -1, pay, 184);             /* cc 0 */
        ts_pkt(s_pkt(), AUD_PID, 0, 5, -1, es_src + 175, 184);    /* 4 lost */
        ts_pkt(s_pkt(), AUD_PID, 0, 6, -1, es_src + 359, 184);
        sink_init(&sink);
        ts_demux_feed(d, strm, slen, sink_fn, &sink);
        CHECK("cc_gap_counted", ts_demux_cc_errors(d) == 1);
        CHECK("cc_gap_stops_at_the_gap", sink.n == 175);
        /* The next PUSI restarts the PES: bytes flow again. */
        s_reset();
        hl = pes_hdr_build(pay, 400, 0);
        memcpy(pay + hl, es_src, 184 - hl);
        ts_pkt(s_pkt(), AUD_PID, 1, 7, -1, pay, 184);
        ts_demux_feed(d, strm, slen, sink_fn, &sink);
        CHECK("cc_gap_recovers_on_next_pusi", sink.n == 350);
        ts_demux_close(d);
    }
    /* A signalled discontinuity is not packet loss, so it is not counted. */
    {
        unsigned char sec[256], pay[184];
        size_t sl, hl;
        TsDemux *d = ts_demux_open();
        s_reset();
        sl = build_pat(sec, PMT_PID);       psi_pkt(s_pkt(), 0x0000, 0, 0, sec, sl);
        sl = build_pmt(sec, AUD_PID, 0x0F); psi_pkt(s_pkt(), PMT_PID, 0, 0, sec, sl);
        hl = pes_hdr_build(pay, 600, 0);
        memcpy(pay + hl, es_src, 184 - hl);
        ts_pkt(s_pkt(), AUD_PID, 1, 0, -1, pay, 184);
        ts_pkt_disc(s_pkt(), AUD_PID, 0, 5, 1, es_src + 175, 182);
        sink_init(&sink);
        ts_demux_feed(d, strm, slen, sink_fn, &sink);
        CHECK("signalled_discontinuity_not_counted", ts_demux_cc_errors(d) == 0);
        ts_demux_close(d);
    }

    /* ---- LATM AAC (stream_type 0x11) is not a stream we can decode ---- */
    /* decoder.c maps VR_CODEC_AAC to AV_CODEC_ID_AAC only, so claiming a LATM
     * PID gives a silent stream with no error. Fall through to the next ES. */
    {
        TsDemux *d = ts_demux_open();
        build_stream(400, 0x11, 0, -1, 0);
        sink_init(&sink);
        ts_demux_feed(d, strm, slen, sink_fn, &sink);
        CHECK("latm_not_claimed_type", ts_demux_stream_type(d) == 0);
        CHECK("latm_not_emitted", sink.n == 0);
        ts_demux_close(d);
    }

    /* ---- a section too long for the buffer is dropped, not left open -- */
    /* section_length is 12 bits (up to 4093 bytes of payload) but Section.buf
     * holds 1024, so such a section can never complete. */
    {
        unsigned char sec[256], pay[184];
        size_t sl;
        TsDemux *d = ts_demux_open();
        s_reset();
        sl = build_pat(sec, PMT_PID);
        sec[1] = (unsigned char)(0xB0 | ((2000 >> 8) & 0x0F));   /* claims 2000 */
        sec[2] = (unsigned char)(2000 & 0xFF);
        psi_pkt(s_pkt(), 0x0000, 0, 0, sec, sl);
        memset(pay, 0xFF, sizeof pay);
        ts_pkt(s_pkt(), 0x0000, 0, 1, -1, pay, 184);   /* continuation, no PUSI */
        ts_pkt(s_pkt(), 0x0000, 0, 2, -1, pay, 184);
        sl = build_pat(sec, PMT_PID);                  /* a real PAT, at last */
        psi_pkt(s_pkt(), 0x0000, 3, 0, sec, sl);
        sl = build_pmt(sec, AUD_PID, 0x0F); psi_pkt(s_pkt(), PMT_PID, 0, 0, sec, sl);
        {
            int cc = 0;
            emit_es(AUD_PID, &cc, es_src, 400, 0, -1);
        }
        sink_init(&sink);
        ts_demux_feed(d, strm, slen, sink_fn, &sink);
        CHECK("oversized_section_then_real_pat", ts_demux_stream_type(d) == 0x0F);
        CHECK("oversized_section_es_exact", es_matches(&sink, 400));
        ts_demux_close(d);
    }

    /* A whole-stream sweep of every single-byte split point, as a last
     * sanity net over the partial-packet buffer. */
    build_stream(n, 0x0F, 0, -1, 0);
    {
        int all = 1;
        for (i = 0; i <= slen; i++) {
            Sink s2;
            TsDemux *d = ts_demux_open();
            sink_init(&s2);
            ts_demux_feed(d, strm, i, sink_fn, &s2);
            ts_demux_feed(d, strm + i, slen - i, sink_fn, &s2);
            if (!es_matches(&s2, n)) { all = 0; printf("  (split %zu failed)\n", i); break; }
            ts_demux_close(d);
            if (!all) break;
        }
        CHECK("every_split_offset", all);
    }

    printf("test_ts_demux: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
