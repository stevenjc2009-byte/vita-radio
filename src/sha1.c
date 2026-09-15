#include "sha1.h"

#include <string.h>

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void compress(Sha1 *s, const uint8_t *b)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)b[i * 4] << 24 | (uint32_t)b[i * 4 + 1] << 16 |
               (uint32_t)b[i * 4 + 2] << 8 | b[i * 4 + 3];
    for (int i = 16; i < 80; i++)
        w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = s->h[0], bb = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (bb & c) | (~bb & d);          k = 0x5A827999; }
        else if (i < 40) { f = bb ^ c ^ d;                    k = 0x6ED9EBA1; }
        else if (i < 60) { f = (bb & c) | (bb & d) | (c & d); k = 0x8F1BBCDC; }
        else             { f = bb ^ c ^ d;                    k = 0xCA62C1D6; }
        uint32_t t = ROL(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = ROL(bb, 30);
        bb = a;
        a = t;
    }
    s->h[0] += a;
    s->h[1] += bb;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
}

void sha1_init(Sha1 *s)
{
    static const uint32_t IV[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    memcpy(s->h, IV, sizeof(IV));
    s->total = 0;
    s->used = 0;
}

void sha1_update(Sha1 *s, const uint8_t *data, size_t len)
{
    s->total += len;
    while (len > 0) {
        size_t n = 64 - s->used;
        if (n > len)
            n = len;
        memcpy(s->block + s->used, data, n);
        s->used += n;
        data += n;
        len -= n;
        if (s->used == 64) {
            compress(s, s->block);
            s->used = 0;
        }
    }
}

void sha1_final(Sha1 *s, uint8_t out[20])
{
    uint64_t bits = s->total * 8;
    uint8_t pad = 0x80, zero = 0, len[8];

    sha1_update(s, &pad, 1);
    while (s->used != 56)
        sha1_update(s, &zero, 1);
    for (int i = 0; i < 8; i++)
        len[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_update(s, len, 8);
    for (int i = 0; i < 20; i++)
        out[i] = (uint8_t)(s->h[i / 4] >> (24 - 8 * (i % 4)));
}
