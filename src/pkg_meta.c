#include "pkg_meta.h"
#include "sha1.h"

#include <stdio.h>
#include <string.h>

#define SFO_MAGIC 0x46535000u   /* "\0PSF" little-endian */

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

int sfo_get_string(const uint8_t *sfo, size_t size, const char *name, char *out, size_t outsz)
{
    if (!sfo || !name || !out || !outsz || size < 20 || le32(sfo) != SFO_MAGIC)
        return -1;
    uint32_t keyofs = le32(sfo + 8), valofs = le32(sfo + 12), count = le32(sfo + 16);
    if (keyofs > size || valofs > size || count > (size - 20) / 16)
        return -1;

    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = sfo + 20 + i * 16;
        size_t key = (size_t)keyofs + le16(e);
        size_t val = (size_t)valofs + le32(e + 12);
        if (key >= size || val >= size)
            return -1;
        size_t keymax = size - key;
        if (strnlen((const char *)sfo + key, keymax) == keymax)
            return -1;
        if (strcmp((const char *)sfo + key, name) != 0)
            continue;
        size_t vmax = size - val;
        size_t n = strnlen((const char *)sfo + val, vmax);
        if (n == vmax || n + 1 > outsz)
            return -1;
        memcpy(out, sfo + val, n);
        out[n] = '\0';
        return 0;
    }
    return -1;
}

int title_id_valid(const char *id)
{
    if (!id || strlen(id) != 9)
        return 0;
    for (int i = 0; i < 9; i++)
        if (!((id[i] >= 'A' && id[i] <= 'Z') || (id[i] >= '0' && id[i] <= '9')))
            return 0;
    return 1;
}

/* The fake-PKG "HMAC": SHA-1 over data, key bytes shuffled into a 64-byte
 * block, SHA-1 again, first 16 bytes kept. */
static void fpkg_hmac(const uint8_t *data, size_t len, uint8_t hmac[16])
{
    Sha1 ctx;
    uint8_t sha[20], buf[64];

    sha1_init(&ctx);
    sha1_update(&ctx, data, len);
    sha1_final(&ctx, sha);

    memset(buf, 0, sizeof(buf));
    memcpy(&buf[0], &sha[4], 8);
    memcpy(&buf[8], &sha[4], 8);
    memcpy(&buf[16], &sha[12], 4);
    buf[20] = sha[16];
    buf[21] = sha[1];
    buf[22] = sha[2];
    buf[23] = sha[3];
    memcpy(&buf[24], &buf[16], 8);

    sha1_init(&ctx);
    sha1_update(&ctx, buf, sizeof(buf));
    sha1_final(&ctx, sha);
    memcpy(hmac, sha, 16);
}

int headbin_make(const uint8_t *tmpl, size_t size, const char *title_id,
                 const char *content_id, uint8_t *out)
{
    uint8_t hmac[16];
    char full_id[48];

    if (!tmpl || !out || size < 0xF0 || !title_id_valid(title_id))
        return -1;
    memcpy(out, tmpl, size);

    /* 48 bytes at 0x30: the content ID, zero-padded */
    if (content_id && content_id[0])
        snprintf(full_id, sizeof(full_id), "%s", content_id);
    else
        snprintf(full_id, sizeof(full_id), "EP9000-%s_00-0000000000000000", title_id);
    memset(out + 0x30, 0, 48);
    memcpy(out + 0x30, full_id, strlen(full_id));

    /* Every bound below is a subtraction against a value already known to fit,
     * never an addition: size_t is 32-bit on the Vita, so a template offset
     * near 0xFFFFFFFF would wrap an addition and pass. size >= 0xF0 here. */

    /* header */
    uint32_t len = be32(out + 0xD0);
    if (len > size - 16)
        return -1;
    fpkg_hmac(out, len, hmac);
    memcpy(out + len, hmac, 16);

    /* package info */
    uint32_t off = be32(out + 0x8), ilen = be32(out + 0x10), dst = be32(out + 0xD4);
    if (ilen < 64 || off > size || ilen - 64 > size - off || dst > size - 16)
        return -1;
    fpkg_hmac(out + off, ilen - 64, hmac);
    memcpy(out + dst, hmac, 16);

    /* everything */
    len = be32(out + 0xE8);
    if (len > size - 16)
        return -1;
    fpkg_hmac(out, len, hmac);
    memcpy(out + len, hmac, 16);
    return 0;
}

/* ---- update handover token ------------------------------------------------ */

_Static_assert(sizeof(UpdateToken) == 76,
               "UpdateToken must be the same size in the app and the updater title");

static void sfo_digest(const uint8_t *sfo, size_t sfo_len, uint8_t out[20])
{
    Sha1 ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, sfo, sfo_len);
    sha1_final(&ctx, out);
}

void update_token_build(UpdateToken *t, const char *tag, const uint8_t nonce[16],
                        const uint8_t *sfo, size_t sfo_len)
{
    memset(t, 0, sizeof(*t));
    t->magic = UPDATE_TOKEN_MAGIC;
    t->version = UPDATE_TOKEN_VERSION;
    memcpy(t->nonce, nonce, sizeof(t->nonce));
    snprintf(t->tag, sizeof(t->tag), "%s", tag ? tag : "");
    sfo_digest(sfo, sfo_len, t->sfo_sha1);
}

int update_token_check(const UpdateToken *t, size_t len, const uint8_t *sfo, size_t sfo_len)
{
    uint8_t want[20];

    if (!t || len != sizeof(*t) || !sfo || sfo_len == 0)
        return -1;
    if (t->magic != UPDATE_TOKEN_MAGIC || t->version != UPDATE_TOKEN_VERSION ||
        t->tag[UPDATE_TOKEN_TAG_LEN - 1] != '\0')
        return -1;
    sfo_digest(sfo, sfo_len, want);
    return memcmp(want, t->sfo_sha1, sizeof(want)) == 0 ? 0 : -1;
}
