#include "adts_scan.h"

#include <stdlib.h>
#include <string.h>

/* An ADTS frame length is 13 bits, so one frame is at most 8191 bytes. While
 * sync has not been confirmed we also need the following frame's header, hence
 * the extra room: the working buffer can always hold the largest item it has to
 * decide about, which is what guarantees the scanner always makes progress. */
#define ADTS_HDR   7u
#define ADTS_MAX   8191u
#define ADTS_LOOK  10u       /* enough to test an ADTS header (7) or an ID3 one (10) */
#define SCAN_CAP   (ADTS_MAX + ADTS_LOOK + 8)

struct AdtsScan {
    unsigned char buf[SCAN_CAP];
    size_t        n;          /* bytes carried over / staged in buf */
    size_t        skip_left;  /* tag bytes still to discard (tag > buf) */
    int           locked;     /* sync confirmed: a valid header is enough */
    unsigned      tags;
    unsigned      skipped;
};

/* Returns the ADTS frame length at p, or 0 if that is not a valid header.
 * Same field layout as sniff.c, plus the payload-sanity checks this module
 * needs before it will trust a length and hand bytes to the decoder. */
static size_t adts_len(const unsigned char *p, size_t avail)
{
    unsigned freq, chan;
    size_t fl;

    if (avail < ADTS_HDR)
        return 0;
    if (p[0] != 0xFF || (p[1] & 0xF6) != 0xF0)   /* sync 0xFFF, layer bits 00 */
        return 0;
    freq = (unsigned)(p[2] >> 2) & 0x0F;
    chan = (unsigned)((p[2] & 1) << 2) | (unsigned)(p[3] >> 6);
    if (freq > 12)                  /* 13, 14 reserved; 15 is escape */
        return 0;
    if (chan == 0 || chan > 7)      /* 0 means an in-band PCE: not a radio feed */
        return 0;
    fl = ((size_t)(p[3] & 3) << 11) | ((size_t)p[4] << 3) | ((size_t)p[5] >> 5);
    return fl >= ADTS_HDR ? fl : 0;
}

/* Returns the total ID3v2 tag length at p (may exceed avail), or 0 if that is
 * not a valid tag header. Needs at least 10 bytes. */
static size_t id3_len(const unsigned char *p, size_t avail)
{
    size_t size;

    if (avail < 10 || p[0] != 'I' || p[1] != 'D' || p[2] != '3')
        return 0;
    if (p[3] == 0xFF || p[4] == 0xFF || ((p[6] | p[7] | p[8] | p[9]) & 0x80))
        return 0;
    /* syncsafe: 7 bits per byte */
    size = ((size_t)p[6] << 21) | ((size_t)p[7] << 14) | ((size_t)p[8] << 7) | (size_t)p[9];
    size += 10;
    if (p[5] & 0x10)
        size += 10;   /* footer present */
    return size;
}

/* True if the avail (< 10) bytes at p are still consistent with "ID3". */
static int id3_prefix(const unsigned char *p, size_t avail)
{
    static const unsigned char id3[3] = { 'I', 'D', '3' };
    size_t i;

    for (i = 0; i < avail && i < 3; i++)
        if (p[i] != id3[i])
            return 0;
    return 1;
}

/* Consumes as much of a->buf as can be decided on, emitting whole frames.
 * Returns 1 if fn asked to stop, else 0. Whatever could not be decided yet
 * (a split header, an incomplete frame) is moved to the front of the buffer. */
static int drain(AdtsScan *a, AdtsFn fn, void *user)
{
    size_t off = 0;
    int stop = 0;

    while (off < a->n) {
        const unsigned char *p = a->buf + off;
        size_t avail = a->n - off;

        if (p[0] == 'I') {
            if (avail < 10) {
                if (id3_prefix(p, avail))
                    break;                  /* tag header split across feeds */
            } else {
                size_t tl = id3_len(p, avail);
                if (tl != 0) {
                    a->tags++;
                    if (tl <= avail) {
                        off += tl;
                        continue;
                    }
                    a->skip_left = tl - avail;   /* tag longer than the buffer */
                    off = a->n;
                    break;
                }
            }
        } else if (p[0] == 0xFF) {
            size_t fl;
            if (avail < ADTS_HDR)
                break;                      /* frame header split across feeds */
            fl = adts_len(p, avail);
            if (fl != 0) {
                int ok = 1;
                if (!a->locked) {
                    /* A lone 0xFFF is junk unless the length it declares lands
                     * on something real: the next frame, or an interleaved ID3
                     * tag, which is exactly how these feeds carry metadata.
                     * Only needed while re-acquiring sync; once locked a valid
                     * header is trusted, so the last frame is not stranded. */
                    if (avail < fl + ADTS_LOOK)
                        break;
                    ok = adts_len(p + fl, avail - fl) != 0 ||
                         id3_len(p + fl, avail - fl) != 0;
                    if (ok)
                        a->locked = 1;
                } else if (avail < fl) {
                    break;                  /* hold the partial frame back */
                }
                if (ok) {
                    off += fl;
                    if (fn && fn(user, p, fl)) {
                        stop = 1;
                        break;
                    }
                    continue;
                }
            }
            a->locked = 0;                  /* bad header: re-acquire sync */
        }

        a->skipped++;                       /* junk: resync one byte at a time */
        off++;
    }

    if (off > 0) {
        memmove(a->buf, a->buf + off, a->n - off);
        a->n -= off;
    }
    return stop;
}

AdtsScan *adts_scan_open(void)
{
    return (AdtsScan *)calloc(1, sizeof(AdtsScan));
}

void adts_scan_close(AdtsScan *a)
{
    free(a);
}

/* Clears buffered bytes and sync state. The dropped/skipped counters are
 * cumulative stream statistics, not buffered state, so they are left alone. */
void adts_scan_reset(AdtsScan *a)
{
    if (!a)
        return;
    a->n = 0;
    a->skip_left = 0;
    a->locked = 0;
}

int adts_scan_feed(AdtsScan *a, const unsigned char *data, size_t len,
                   AdtsFn fn, void *user)
{
    size_t pos = 0;

    if (!a || (!data && len != 0))
        return -1;

    for (;;) {
        size_t before, k;

        if (a->skip_left != 0) {            /* still inside an oversized tag */
            k = a->skip_left < a->n ? a->skip_left : a->n;
            if (k != 0) {
                memmove(a->buf, a->buf + k, a->n - k);
                a->n -= k;
                a->skip_left -= k;
            }
            k = len - pos;
            if (k > a->skip_left)
                k = a->skip_left;
            pos += k;
            a->skip_left -= k;
            if (a->skip_left != 0)
                return 0;                   /* whole feed swallowed by the tag */
        }

        if (pos < len && a->n < SCAN_CAP) {
            k = SCAN_CAP - a->n;
            if (k > len - pos)
                k = len - pos;
            memcpy(a->buf + a->n, data + pos, k);
            a->n += k;
            pos += k;
        }

        if (a->n == 0)
            return 0;

        before = a->n;
        if (drain(a, fn, user))
            return 1;                       /* caller aborted: drop the rest */
        if (a->n == before) {
            /* No progress. After the top-up above, that means either the input
             * is exhausted (wait for more bytes) or the buffer is full without
             * a decision being possible, which the capacity rules out. */
            if (pos >= len)
                return 0;
            return -1;
        }
    }
}

unsigned adts_scan_tags_dropped(const AdtsScan *a)
{
    return a ? a->tags : 0;
}

unsigned adts_scan_bytes_skipped(const AdtsScan *a)
{
    return a ? a->skipped : 0;
}
