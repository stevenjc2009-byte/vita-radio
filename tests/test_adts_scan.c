#include "adts_scan.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0, g_pass = 0;

#define CHECK(name, cond) do { \
    if (cond) { printf("PASS %s\n", name); g_pass++; } \
    else { printf("FAIL %s (%s:%d)\n", name, __FILE__, __LINE__); g_fail++; } \
} while (0)

/* ---- fixtures (all synthesised, deterministic, offline) --------------- */

/* Writes the 7-byte ADTS header only: MPEG-4, layer 00, no CRC, AAC-LC,
 * 44.1 kHz (freq index 4), channel config 2. Same field layout as sniff.c. */
static void put_adts_hdr(unsigned char *b, size_t fl)
{
    b[0] = 0xFF;
    b[1] = 0xF1;
    b[2] = (unsigned char)((1 << 6) | (4 << 2));
    b[3] = (unsigned char)((2 << 6) | ((fl >> 11) & 3));
    b[4] = (unsigned char)((fl >> 3) & 0xFF);
    b[5] = (unsigned char)(((fl & 7) << 5) | 0x1F);
    b[6] = 0xFC;
}

/* A whole frame: header plus a deterministic payload that never reaches 0x80,
 * so no payload byte can be mistaken for a sync word. */
static size_t put_adts(unsigned char *b, size_t fl, unsigned seed)
{
    size_t i;
    put_adts_hdr(b, fl);
    for (i = 7; i < fl; i++)
        b[i] = (unsigned char)((seed + i) & 0x7F);
    return fl;
}

/* ID3v2.4 tag with a syncsafe size. Two chained 7-byte ADTS frames are planted
 * in the body: if the tag is not skipped whole, they leak into the output. */
static size_t put_id3(unsigned char *b, size_t body, int footer)
{
    size_t n = 10 + body + (footer ? 10 : 0);
    b[0] = 'I'; b[1] = 'D'; b[2] = '3'; b[3] = 4; b[4] = 0;
    b[5] = (unsigned char)(footer ? 0x10 : 0);
    b[6] = (unsigned char)((body >> 21) & 0x7F);
    b[7] = (unsigned char)((body >> 14) & 0x7F);
    b[8] = (unsigned char)((body >> 7) & 0x7F);
    b[9] = (unsigned char)(body & 0x7F);
    memset(b + 10, 0, body);
    if (body >= 14) {
        put_adts(b + 10, 7, 0);
        put_adts(b + 17, 7, 0);
    }
    if (footer)
        memcpy(b + 10 + body, "3DI\x04\x00\x10\x00\x00\x07\x68", 10);
    return n;
}

/* ---- sink ------------------------------------------------------------ */

typedef struct {
    unsigned char out[1 << 16];
    size_t        n;
    int           calls;
    int           stop_at;   /* 1-based call index that returns non-zero */
} Sink;

static int sink_fn(void *user, const unsigned char *frames, size_t len)
{
    Sink *s = (Sink *)user;
    s->calls++;
    if (s->n + len <= sizeof s->out) {
        memcpy(s->out + s->n, frames, len);
        s->n += len;
    }
    return (s->stop_at && s->calls >= s->stop_at) ? 1 : 0;
}

static int same(const Sink *s, const unsigned char *exp, size_t n)
{
    return s->n == n && memcmp(s->out, exp, n) == 0;
}

static unsigned char in[1 << 16];
static unsigned char exp[1 << 16];

int main(void)
{
    Sink s;
    AdtsScan *a;
    size_t n, e, i;

    /* ---- clean run passes through byte-identical ---------------------- */
    e = 0;
    e += put_adts(exp + e, 200, 1);
    e += put_adts(exp + e, 311, 2);
    e += put_adts(exp + e, 7,   3);
    e += put_adts(exp + e, 450, 4);
    memcpy(in, exp, e);
    memset(&s, 0, sizeof s);
    a = adts_scan_open();
    CHECK("open_not_null", a != NULL);
    CHECK("clean_feed_ok", adts_scan_feed(a, in, e, sink_fn, &s) == 0);
    CHECK("clean_identical", same(&s, exp, e));
    CHECK("clean_no_tags", adts_scan_tags_dropped(a) == 0);
    CHECK("clean_no_skips", adts_scan_bytes_skipped(a) == 0);
    adts_scan_close(a);

    /* ---- leading ID3v2.4 tag dropped ---------------------------------- */
    e = 0;
    e += put_adts(exp + e, 200, 5);
    e += put_adts(exp + e, 200, 6);
    n = put_id3(in, 1000, 0);
    memcpy(in + n, exp, e);
    n += e;
    memset(&s, 0, sizeof s);
    a = adts_scan_open();
    CHECK("id3_lead_feed_ok", adts_scan_feed(a, in, n, sink_fn, &s) == 0);
    CHECK("id3_lead_output", same(&s, exp, e));
    CHECK("id3_lead_counted", adts_scan_tags_dropped(a) == 1);
    CHECK("id3_lead_no_skips", adts_scan_bytes_skipped(a) == 0);
    adts_scan_close(a);

    /* ---- tag BETWEEN two frames dropped ------------------------------- */
    e = 0;
    e += put_adts(exp + e, 220, 7);
    e += put_adts(exp + e, 260, 8);
    e += put_adts(exp + e, 180, 9);
    n = 0;
    memcpy(in + n, exp, 220);            n += 220;
    n += put_id3(in + n, 500, 0);
    memcpy(in + n, exp + 220, e - 220);  n += e - 220;
    memset(&s, 0, sizeof s);
    a = adts_scan_open();
    CHECK("id3_mid_feed_ok", adts_scan_feed(a, in, n, sink_fn, &s) == 0);
    CHECK("id3_mid_output", same(&s, exp, e));
    CHECK("id3_mid_counted", adts_scan_tags_dropped(a) == 1);
    CHECK("id3_mid_no_skips", adts_scan_bytes_skipped(a) == 0);
    adts_scan_close(a);

    /* ---- footer flag: 20 extra bytes, not 10 -------------------------- */
    e = 0;
    e += put_adts(exp + e, 200, 10);
    e += put_adts(exp + e, 200, 11);
    n = put_id3(in, 300, 1);
    memcpy(in + n, exp, e);
    n += e;
    memset(&s, 0, sizeof s);
    a = adts_scan_open();
    CHECK("id3_footer_feed_ok", adts_scan_feed(a, in, n, sink_fn, &s) == 0);
    CHECK("id3_footer_output", same(&s, exp, e));
    CHECK("id3_footer_counted", adts_scan_tags_dropped(a) == 1);
    /* If only 10+body were skipped, the 10 footer bytes become junk. */
    CHECK("id3_footer_no_skips", adts_scan_bytes_skipped(a) == 0);
    adts_scan_close(a);

    /* ---- syncsafe size: 00 00 02 01 is 257, not 513 ------------------- */
    e = 0;
    e += put_adts(exp + e, 200, 12);
    e += put_adts(exp + e, 200, 13);
    n = put_id3(in, 257, 0);
    CHECK("syncsafe_fixture", in[6] == 0x00 && in[7] == 0x00 && in[8] == 0x02 && in[9] == 0x01);
    memcpy(in + n, exp, e);
    n += e;
    memset(&s, 0, sizeof s);
    a = adts_scan_open();
    CHECK("syncsafe_feed_ok", adts_scan_feed(a, in, n, sink_fn, &s) == 0);
    CHECK("syncsafe_output", same(&s, exp, e));
    CHECK("syncsafe_counted", adts_scan_tags_dropped(a) == 1);
    adts_scan_close(a);

    /* ---- partial frame held back, completed by the next feed ---------- */
    e = 0;
    e += put_adts(exp + e, 200, 14);
    e += put_adts(exp + e, 200, 15);
    e += put_adts(exp + e, 300, 16);
    memcpy(in, exp, e);
    memset(&s, 0, sizeof s);
    a = adts_scan_open();
    CHECK("partial_feed1_ok", adts_scan_feed(a, in, 400 + 150, sink_fn, &s) == 0);
    CHECK("partial_holds_back", same(&s, exp, 400));
    CHECK("partial_feed2_ok", adts_scan_feed(a, in + 550, e - 550, sink_fn, &s) == 0);
    CHECK("partial_completed", same(&s, exp, e));
    adts_scan_close(a);

    /* ---- split in the middle of an ID3 header ------------------------- */
    e = 0;
    e += put_adts(exp + e, 200, 17);
    e += put_adts(exp + e, 200, 18);
    n = put_id3(in, 120, 0);
    memcpy(in + n, exp, e);
    n += e;
    {
        int all = 1;
        size_t cut;
        for (cut = 1; cut <= 9; cut++) {          /* every split inside the header */
            memset(&s, 0, sizeof s);
            a = adts_scan_open();
            if (adts_scan_feed(a, in, cut, sink_fn, &s) != 0)
                all = 0;
            if (adts_scan_feed(a, in + cut, n - cut, sink_fn, &s) != 0)
                all = 0;
            if (!same(&s, exp, e) || adts_scan_tags_dropped(a) != 1 ||
                adts_scan_bytes_skipped(a) != 0)
                all = 0;
            adts_scan_close(a);
        }
        CHECK("id3_header_split_any_offset", all);
    }

    /* ---- tag bigger than any internal buffer, split across feeds ------ */
    e = 0;
    e += put_adts(exp + e, 200, 19);
    e += put_adts(exp + e, 200, 20);
    n = put_id3(in, 9000, 0);
    memcpy(in + n, exp, e);
    n += e;
    memset(&s, 0, sizeof s);
    a = adts_scan_open();
    CHECK("id3_big_feed1_ok", adts_scan_feed(a, in, 4000, sink_fn, &s) == 0);
    CHECK("id3_big_nothing_yet", s.n == 0);
    CHECK("id3_big_feed2_ok", adts_scan_feed(a, in + 4000, n - 4000, sink_fn, &s) == 0);
    CHECK("id3_big_output", same(&s, exp, e));
    CHECK("id3_big_counted", adts_scan_tags_dropped(a) == 1);
    adts_scan_close(a);

    /* ---- junk before the first frame is skipped and counted ----------- */
    e = 0;
    e += put_adts(exp + e, 200, 21);
    e += put_adts(exp + e, 200, 22);
    for (i = 0; i < 37; i++)
        in[i] = (unsigned char)((i * 7 + 1) & 0x7F);
    memcpy(in + 37, exp, e);
    n = 37 + e;
    memset(&s, 0, sizeof s);
    a = adts_scan_open();
    CHECK("junk_feed_ok", adts_scan_feed(a, in, n, sink_fn, &s) == 0);
    CHECK("junk_output", same(&s, exp, e));
    CHECK("junk_counted", adts_scan_bytes_skipped(a) == 37);
    adts_scan_close(a);

    /* ---- a lone 0xFFF that nothing confirms is junk ------------------- */
    e = 0;
    e += put_adts(exp + e, 200, 23);
    e += put_adts(exp + e, 200, 24);
    put_adts_hdr(in, 100);                 /* claims 100 bytes, nothing follows it */
    for (i = 7; i < 57; i++)
        in[i] = 0x11;
    memcpy(in + 57, exp, e);
    n = 57 + e;
    memset(&s, 0, sizeof s);
    a = adts_scan_open();
    CHECK("false_sync_feed_ok", adts_scan_feed(a, in, n, sink_fn, &s) == 0);
    CHECK("false_sync_not_emitted", same(&s, exp, e));
    CHECK("false_sync_counted", adts_scan_bytes_skipped(a) == 57);
    adts_scan_close(a);

    /* ---- bad layer bits rejected (with a control that must pass) ------ */
    e = 0;
    e += put_adts(exp + e, 200, 25);
    e += put_adts(exp + e, 200, 26);
    e += put_adts(exp + e, 200, 27);
    memcpy(in, exp, e);
    memset(&s, 0, sizeof s);
    a = adts_scan_open();
    CHECK("layer_control_emits", adts_scan_feed(a, in, e, sink_fn, &s) == 0 && same(&s, exp, e));
    adts_scan_close(a);
    in[1] = in[201] = in[401] = 0xF3;      /* layer 01: not ADTS */
    memset(&s, 0, sizeof s);
    a = adts_scan_open();
    CHECK("layer_bad_feed_ok", adts_scan_feed(a, in, e, sink_fn, &s) == 0);
    CHECK("layer_bad_nothing_emitted", s.n == 0);
    CHECK("layer_bad_skipped", adts_scan_bytes_skipped(a) >= e - 7);
    adts_scan_close(a);

    /* ---- bad sampling frequency index rejected ------------------------ */
    memcpy(in, exp, e);
    in[2] = (unsigned char)((1 << 6) | (13 << 2));      /* freq index 13 */
    in[202] = in[2];
    in[402] = in[2];
    memset(&s, 0, sizeof s);
    a = adts_scan_open();
    CHECK("freq_bad_feed_ok", adts_scan_feed(a, in, e, sink_fn, &s) == 0);
    CHECK("freq_bad_nothing_emitted", s.n == 0);
    adts_scan_close(a);

    /* ---- reset() clears a buffered partial frame ---------------------- */
    e = 0;
    e += put_adts(exp + e, 200, 28);
    e += put_adts(exp + e, 200, 29);
    e += put_adts(exp + e, 300, 30);
    memcpy(in, exp, e);
    memset(&s, 0, sizeof s);
    a = adts_scan_open();
    CHECK("reset_feed1_ok", adts_scan_feed(a, in, 400 + 150, sink_fn, &s) == 0);
    CHECK("reset_feed1_output", same(&s, exp, 400));
    adts_scan_reset(a);
    CHECK("reset_tail_ok", adts_scan_feed(a, in + 550, e - 550, sink_fn, &s) == 0);
    CHECK("reset_dropped_partial", s.n == 400);        /* orphan tail emits nothing */
    {
        size_t f = 0;
        f += put_adts(exp + 400, 200, 31);
        f += put_adts(exp + 400 + 200, 200, 32);
        memcpy(in, exp + 400, f);
        CHECK("reset_resyncs_ok", adts_scan_feed(a, in, f, sink_fn, &s) == 0);
        CHECK("reset_resyncs", same(&s, exp, 400 + f));
    }
    adts_scan_close(a);

    /* ---- callback returning non-zero stops the feed ------------------- */
    e = 0;
    e += put_adts(exp + e, 200, 33);
    e += put_adts(exp + e, 200, 34);
    e += put_adts(exp + e, 200, 35);
    e += put_adts(exp + e, 200, 36);
    memcpy(in, exp, e);
    memset(&s, 0, sizeof s);
    s.stop_at = 2;
    a = adts_scan_open();
    CHECK("stop_returns_one", adts_scan_feed(a, in, e, sink_fn, &s) == 1);
    CHECK("stop_two_frames_only", same(&s, exp, 400));
    CHECK("stop_call_count", s.calls == 2);
    adts_scan_close(a);

    /* ---- fully stateful: one byte per feed ---------------------------- */
    e = 0;
    e += put_adts(exp + e, 200, 37);
    e += put_adts(exp + e, 311, 38);
    e += put_adts(exp + e, 200, 39);
    n = 0;
    n += put_id3(in + n, 40, 0);
    memcpy(in + n, exp, 200);           n += 200;
    n += put_id3(in + n, 30, 1);
    memcpy(in + n, exp + 200, e - 200); n += e - 200;
    memset(&s, 0, sizeof s);
    a = adts_scan_open();
    {
        int all = 1;
        for (i = 0; i < n; i++)
            if (adts_scan_feed(a, in + i, 1, sink_fn, &s) != 0)
                all = 0;
        CHECK("bytewise_feeds_ok", all);
    }
    CHECK("bytewise_output", same(&s, exp, e));
    CHECK("bytewise_tags", adts_scan_tags_dropped(a) == 2);
    CHECK("bytewise_no_skips", adts_scan_bytes_skipped(a) == 0);
    adts_scan_close(a);

    /* ---- degenerate inputs -------------------------------------------- */
    a = adts_scan_open();
    memset(&s, 0, sizeof s);
    CHECK("empty_feed_ok", adts_scan_feed(a, in, 0, sink_fn, &s) == 0);
    CHECK("empty_no_output", s.n == 0);
    adts_scan_close(a);
    adts_scan_close(NULL);

    printf("test_adts_scan: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
