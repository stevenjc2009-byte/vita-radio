/* MPEG-TS -> audio elementary stream.
 *
 * Written by hand because the VitaSDK's libavformat.a has no ff_mpegts_demuxer
 * (measured 2026-09-16: 22 demuxers present, mpegts/hls/mov all absent), so
 * avformat_open_input() cannot open an HLS .ts segment on this target.
 *
 * Scope on purpose: find the PAT, find the PMT, find the first audio
 * elementary stream, and hand its PES payload bytes out as one unbroken byte
 * stream. Nothing in here looks at the contents of that byte stream - in
 * particular it never hunts for an ADTS sync word, because a PES payload does
 * not begin on an ADTS frame boundary. Frame framing is the decoder's job.
 *
 * Section CRC32s are not verified: a corrupt PSI section shows up as a PID
 * that yields no audio, and the cost of the table is not worth it here. */

#include "ts_demux.h"

#include <stdlib.h>
#include <string.h>

#define TS_PKT      188
#define TS_NPID     8192
#define TS_NULL_PID 0x1FFF
#define SEC_MAX     1024            /* 3-byte header + max section_length 1021 */
#define RESYNC_MAX  (1u << 16)      /* unsynced garbage tolerated before -1 */

typedef struct {
    unsigned char buf[SEC_MAX];
    size_t        have;
    int           open;             /* a section start has been seen */
} Section;

enum { PES_IDLE, PES_HDR, PES_SKIP, PES_DATA };

struct TsDemux {
    /* One candidate packet plus one confirmation packet: a lone 0x47 inside a
     * payload must not be mistaken for a packet start, so a candidate is only
     * accepted when the byte 188 further on also looks like a TS header. */
    unsigned char buf[2 * TS_PKT];
    size_t        have;
    int           synced;
    size_t        junk;             /* bytes discarded in this unsynced run */

    int pmt_pid;                    /* -1 until the PAT names one */
    int aud_pid;                    /* -1 until the PMT names one */
    int stream_type;

    Section pat, pmt;

    int           pes_state;
    unsigned char pes_hdr[9];
    size_t        pes_hdr_have;
    size_t        pes_skip;         /* PES_header_data_length bytes still to drop */

    signed char cc[TS_NPID];        /* last continuity counter per PID, -1 = none */
    unsigned    cc_errors;
};

/* ------------------------------------------------------------- sections -- */

static void sec_reset(Section *s)
{
    s->have = 0;
    s->open = 0;
}

static size_t sec_total(const Section *s)
{
    if (s->have < 3)
        return 0;
    return 3 + (size_t)(((s->buf[1] & 0x0F) << 8) | s->buf[2]);
}

/* Returns 1 once the buffer holds a whole section. */
static int sec_push(Section *s, const unsigned char *p, size_t n)
{
    size_t total;
    if (!s->open)
        return 0;
    if (n > SEC_MAX - s->have)
        n = SEC_MAX - s->have;
    memcpy(s->buf + s->have, p, n);
    s->have += n;
    total = sec_total(s);
    if (total > SEC_MAX) {
        /* section_length is 12 bits but the buffer holds 1021 payload bytes,
         * so this one can never complete. Drop it now instead of leaving the
         * buffer full until the next pointer_field happens to clear it. */
        sec_reset(s);
        return 0;
    }
    return total != 0 && s->have >= total;
}

static void parse_pat(TsDemux *t, const unsigned char *s, size_t total)
{
    size_t i;
    if (s[0] != 0x00 || total < 12)         /* table_id 0x00 = PAT */
        return;
    for (i = 8; i + 4 <= total - 4; i += 4) {
        int prog = (s[i] << 8) | s[i + 1];
        int pid  = ((s[i + 2] & 0x1F) << 8) | s[i + 3];
        if (prog != 0) {                    /* program 0 is the NIT, not a PMT */
            if (pid != t->pmt_pid) {
                t->pmt_pid = pid;
                sec_reset(&t->pmt);
            }
            return;
        }
    }
}

static int is_audio_type(int st)
{
    /* 0x11 (LATM AAC) is deliberately absent: decoder.c only ever asks for
     * AV_CODEC_ID_AAC, so LATM bytes reach an ADTS parser that never finds a
     * sync word - a silent stream with no error and no timeout. Falling
     * through to the next elementary stream is the honest failure. */
    return st == 0x03 || st == 0x04        /* MPEG-1 / MPEG-2 audio */
        || st == 0x0F;                     /* ADTS AAC */
}

static void parse_pmt(TsDemux *t, const unsigned char *s, size_t total)
{
    size_t i, end, pil;
    if (s[0] != 0x02 || total < 16)         /* table_id 0x02 = PMT */
        return;
    pil = (size_t)(((s[10] & 0x0F) << 8) | s[11]);
    i = 12 + pil;
    end = total - 4;                        /* stop before the CRC32 */
    while (i + 5 <= end) {
        int    st   = s[i];
        int    pid  = ((s[i + 1] & 0x1F) << 8) | s[i + 2];
        size_t esil = (size_t)(((s[i + 3] & 0x0F) << 8) | s[i + 4]);
        if (is_audio_type(st)) {
            if (pid != t->aud_pid) {
                t->aud_pid = pid;
                t->pes_state = PES_IDLE;
                t->pes_hdr_have = 0;
                t->pes_skip = 0;
            }
            t->stream_type = st;
            return;
        }
        i += 5 + esil;
    }
}

static void psi_done(TsDemux *t, Section *sec, int is_pat)
{
    size_t total = sec_total(sec);
    if (total == 0 || total > sec->have)
        return;
    if (is_pat)
        parse_pat(t, sec->buf, total);
    else
        parse_pmt(t, sec->buf, total);
    sec_reset(sec);
}

static void psi_feed(TsDemux *t, Section *sec, int is_pat,
                     const unsigned char *p, size_t n, int pusi)
{
    if (pusi) {
        size_t ptr;
        if (n == 0)
            return;
        ptr = p[0];                         /* pointer_field */
        p++; n--;
        if (ptr > n) {                      /* malformed - drop the lot */
            sec_reset(sec);
            return;
        }
        /* The bytes before the pointer belong to the previous section. */
        if (sec->open && sec_push(sec, p, ptr))
            psi_done(t, sec, is_pat);
        p += ptr; n -= ptr;
        sec_reset(sec);
        sec->open = 1;
    }
    if (sec_push(sec, p, n))
        psi_done(t, sec, is_pat);
}

/* ------------------------------------------------------------------ PES -- */

static int pes_feed(TsDemux *t, const unsigned char *p, size_t n,
                    TsEsFn fn, void *user, int pusi)
{
    if (pusi) {
        t->pes_state = PES_HDR;
        t->pes_hdr_have = 0;
        t->pes_skip = 0;
    }
    while (n > 0) {
        switch (t->pes_state) {
        case PES_IDLE:
            return 0;                       /* no PES start seen yet */

        case PES_HDR: {
            size_t want = 9 - t->pes_hdr_have;
            if (want > n)
                want = n;
            memcpy(t->pes_hdr + t->pes_hdr_have, p, want);
            t->pes_hdr_have += want;
            p += want; n -= want;
            if (t->pes_hdr_have < 9)
                return 0;                   /* header spans packets */
            if (t->pes_hdr[0] != 0x00 || t->pes_hdr[1] != 0x00 ||
                t->pes_hdr[2] != 0x01 ||
                t->pes_hdr[3] < 0xC0 || t->pes_hdr[3] > 0xDF) {
                t->pes_state = PES_IDLE;    /* not an audio PES */
                return 0;
            }
            /* [4..5] PES_packet_length, [6..7] flags, [8] header_data_length.
             * The length is deliberately ignored: the next PUSI restarts us,
             * and unbounded ("0") lengths are normal for audio. */
            t->pes_skip = t->pes_hdr[8];
            t->pes_state = PES_SKIP;
            break;
        }

        case PES_SKIP: {
            size_t k = (t->pes_skip < n) ? t->pes_skip : n;
            p += k; n -= k;
            t->pes_skip -= k;
            if (t->pes_skip > 0)
                return 0;                   /* optional header spans packets */
            t->pes_state = PES_DATA;
            break;
        }

        default:                            /* PES_DATA */
            return (fn && fn(user, p, n)) ? 1 : 0;
        }
    }
    return 0;
}

/* --------------------------------------------------------------- packet -- */

static int hdr_ok(const unsigned char *p)
{
    return p[0] == 0x47
        && (p[1] & 0x80) == 0               /* transport_error_indicator */
        && ((p[3] >> 4) & 3) != 0;          /* adaptation_field_control 0 is reserved */
}

static int process_pkt(TsDemux *t, const unsigned char *p, TsEsFn fn, void *user)
{
    int    pid, pusi, afc, cc, prev;
    size_t off;

    if (p[1] & 0x80)
        return 0;                           /* flagged corrupt by the sender */
    pid = ((p[1] & 0x1F) << 8) | p[2];
    if (pid == TS_NULL_PID)
        return 0;
    if ((p[3] & 0xC0) != 0)
        return 0;                           /* transport-scrambled */

    pusi = (p[1] & 0x40) != 0;
    afc  = (p[3] >> 4) & 3;
    cc   = p[3] & 0x0F;

    if ((afc & 1) == 0)
        return 0;                           /* no payload: cc does not advance */

    prev = t->cc[pid];
    if (prev >= 0) {
        if (cc == prev)
            return 0;                       /* legal duplicate packet */
        if (cc != ((prev + 1) & 0x0F)) {
            /* The two sides of the gap do not join. Splicing them hands the
             * decoder a frame with its middle missing, and a PSI section
             * reassembled across the gap can name the wrong audio PID. Wait
             * for the next PUSI instead. A discontinuity the sender signalled
             * is a deliberate break, not lost packets, so it is not counted. */
            int signalled = (afc & 2) && p[4] != 0 && (p[5] & 0x80) != 0;
            if (!signalled)
                t->cc_errors++;
            t->pes_state = PES_IDLE;
            t->pes_hdr_have = 0;
            t->pes_skip = 0;
            if (pid == 0x0000)
                sec_reset(&t->pat);
            else if (pid == t->pmt_pid)
                sec_reset(&t->pmt);
        }
    }
    t->cc[pid] = (signed char)cc;

    off = 4;
    if (afc & 2) {
        off = 5 + (size_t)p[4];             /* adaptation_field_length */
        if (off >= TS_PKT)
            return 0;                       /* fills the packet, or malformed */
    }

    if (pid == 0x0000)
        psi_feed(t, &t->pat, 1, p + off, TS_PKT - off, pusi);
    else if (pid == t->pmt_pid)
        psi_feed(t, &t->pmt, 0, p + off, TS_PKT - off, pusi);
    else if (pid == t->aud_pid)
        return pes_feed(t, p + off, TS_PKT - off, fn, user, pusi);
    return 0;
}

/* ------------------------------------------------------------------ API -- */

TsDemux *ts_demux_open(void)
{
    TsDemux *t = (TsDemux *)calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->pmt_pid = -1;
    t->aud_pid = -1;
    memset(t->cc, -1, sizeof t->cc);
    return t;
}

void ts_demux_close(TsDemux *t)
{
    free(t);
}

void ts_demux_reset(TsDemux *t)
{
    if (!t)
        return;
    t->have = 0;
    t->synced = 0;
    t->junk = 0;
    t->pmt_pid = -1;
    t->aud_pid = -1;
    t->stream_type = 0;
    sec_reset(&t->pat);
    sec_reset(&t->pmt);
    t->pes_state = PES_IDLE;
    t->pes_hdr_have = 0;
    t->pes_skip = 0;
    memset(t->cc, -1, sizeof t->cc);
    /* cc_errors is deliberately kept: it is a cumulative lost-packet counter
     * for the UI, not per-segment state. The per-PID table above is cleared so
     * the first packet after a discontinuity is not counted as an error. */
}

/* Drop bytes up to the next 0x47 in the buffer, starting at index 1. */
static void shift_to_next_sync(TsDemux *t)
{
    size_t k = 1;
    while (k < t->have && t->buf[k] != 0x47)
        k++;
    memmove(t->buf, t->buf + k, t->have - k);
    t->have -= k;
    t->junk += k;
}

int ts_demux_feed(TsDemux *t, const unsigned char *data, size_t len,
                  TsEsFn fn, void *user)
{
    size_t i = 0;

    if (!t || (!data && len))
        return -1;

    /* RESYNC_MAX is this call's budget, not the object's. It used to be
     * cumulative, so one segment that was not TS at all - an HTML error page,
     * an fMP4 chunk - spent the budget for good and every later segment
     * returned -1 as well, silently killing the station for the session. */
    t->junk = 0;

    /* The second clause drains a packet already buffered by the resync path,
     * so a feed that ends on a packet boundary emits it without waiting. */
    while (i < len || (t->synced && t->have == TS_PKT)) {
        size_t want = t->synced ? (size_t)TS_PKT : (size_t)(2 * TS_PKT);
        size_t n;

        if (!t->synced && t->have == 0) {
            while (i < len && data[i] != 0x47) {
                i++;
                t->junk++;
            }
            if (t->junk > RESYNC_MAX) {
                ts_demux_reset(t);          /* leave the object usable */
                return -1;
            }
            if (i == len)
                break;
        }

        n = want - t->have;
        if (n > len - i)
            n = len - i;
        memcpy(t->buf + t->have, data + i, n);
        t->have += n;
        i += n;
        if (t->have < want)
            break;                          /* need more input */

        if (!t->synced) {
            if (hdr_ok(t->buf) && hdr_ok(t->buf + TS_PKT)) {
                int rc;
                t->synced = 1;
                t->junk = 0;
                rc = process_pkt(t, t->buf, fn, user);
                memmove(t->buf, t->buf + TS_PKT, TS_PKT);
                t->have = TS_PKT;           /* already confirmed - keep it */
                if (rc)
                    return 1;
            } else {
                shift_to_next_sync(t);
                if (t->junk > RESYNC_MAX) {
                    ts_demux_reset(t);      /* leave the object usable */
                    return -1;
                }
            }
        } else if (t->buf[0] != 0x47) {
            t->synced = 0;                  /* lost alignment mid-stream */
            shift_to_next_sync(t);
        } else {
            int rc = process_pkt(t, t->buf, fn, user);
            t->have = 0;
            if (rc)
                return 1;
        }
    }
    return 0;
}

int ts_demux_stream_type(const TsDemux *t)
{
    return t ? t->stream_type : 0;
}

unsigned ts_demux_cc_errors(const TsDemux *t)
{
    return t ? t->cc_errors : 0u;
}
