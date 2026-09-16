/* Host tests for src/sha1.c and src/pkg_meta.c.
 * The expected head.bin comes from tests/data/make_headbin_expected.py, an
 * independent hashlib implementation of VitaShell's makeHeadBin(). */
#include "pkg_meta.h"
#include "sha1.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int passed, failed;

#define CHECK(cond, ...) do { \
    if (cond) passed++; \
    else { failed++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static size_t read_file(const char *path, uint8_t *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("cannot open %s\n", path);
        return 0;
    }
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return n;
}

static void hex(const uint8_t *d, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++)
        sprintf(out + i * 2, "%02x", d[i]);
}

static void sha1_hex(const uint8_t *data, size_t len, size_t chunk, char *out)
{
    Sha1 s;
    uint8_t dg[20];
    sha1_init(&s);
    for (size_t off = 0; off < len; off += chunk)
        sha1_update(&s, data + off, len - off < chunk ? len - off : chunk);
    sha1_final(&s, dg);
    hex(dg, 20, out);
}

static void test_sha1(void)
{
    char out[41];
    const char *abc = "abc";
    const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";

    sha1_hex((const uint8_t *)"", 0, 1, out);
    CHECK(strcmp(out, "da39a3ee5e6b4b0d3255bfef95601890afd80709") == 0, "empty: %s", out);
    sha1_hex((const uint8_t *)abc, 3, 3, out);
    CHECK(strcmp(out, "a9993e364706816aba3e25717850c26c9cd0d89d") == 0, "abc: %s", out);
    sha1_hex((const uint8_t *)two, strlen(two), 7, out);
    CHECK(strcmp(out, "84983e441c3bd26ebaae4aa1f95129e5e54670f1") == 0, "448-bit: %s", out);

    uint8_t *m = malloc(1000000);
    memset(m, 'a', 1000000);
    sha1_hex(m, 1000000, 4093, out);
    free(m);
    CHECK(strcmp(out, "34aa973cd4c4daa4f61eeb2bdbad27316534016f") == 0, "million a: %s", out);
}

static void test_sfo(void)
{
    uint8_t sfo[8192];
    char v[64];
    size_t n = read_file("data/param.sfo", sfo, sizeof(sfo));
    CHECK(n > 20, "fixture size %zu", n);

    CHECK(sfo_get_string(sfo, n, "TITLE_ID", v, sizeof(v)) == 0 && strcmp(v, "VRAD00001") == 0,
          "TITLE_ID '%s'", v);
    CHECK(sfo_get_string(sfo, n, "TITLE", v, sizeof(v)) == 0 && strcmp(v, "Vita Radio") == 0,
          "TITLE '%s'", v);
    CHECK(sfo_get_string(sfo, n, "NO_SUCH_KEY", v, sizeof(v)) == -1, "missing key found");
    CHECK(sfo_get_string(sfo, n, "TITLE_ID", v, 9) == -1, "9-byte buffer accepted 9 chars");
    CHECK(sfo_get_string(sfo, 40, "TITLE_ID", v, sizeof(v)) == -1, "truncated image accepted");

    uint8_t bad[8192];
    memcpy(bad, sfo, n);
    bad[0] = 'X';
    CHECK(sfo_get_string(bad, n, "TITLE_ID", v, sizeof(v)) == -1, "bad magic accepted");
    CHECK(sfo_get_string(NULL, n, "TITLE_ID", v, sizeof(v)) == -1, "NULL image accepted");
}

static void test_title_id(void)
{
    CHECK(title_id_valid("VRAD00001"), "VRAD00001");
    CHECK(title_id_valid("VRADUPDTR"), "VRADUPDTR");
    CHECK(!title_id_valid("vrad00001"), "lowercase");
    CHECK(!title_id_valid("VRAD0001"), "8 chars");
    CHECK(!title_id_valid("VRAD000011"), "10 chars");
    CHECK(!title_id_valid("VRAD-0001"), "dash");
    CHECK(!title_id_valid(NULL), "NULL");
}

static void test_headbin(void)
{
    uint8_t tmpl[2048], expect[2048], out[2048];
    size_t tn = read_file("../assets/head.bin", tmpl, sizeof(tmpl));
    size_t en = read_file("data/headbin_VRAD00001.bin", expect, sizeof(expect));
    CHECK(tn == 1072 && en == 1072, "sizes %zu %zu", tn, en);

    CHECK(headbin_make(tmpl, tn, "VRAD00001", NULL, out) == 0, "make failed");
    CHECK(memcmp(out, expect, 1072) == 0, "head.bin differs from Python reference");
    CHECK(memcmp(out, tmpl, 1072) != 0, "output equals template");

    /* empty content ID takes the same default */
    CHECK(headbin_make(tmpl, tn, "VRAD00001", "", out) == 0 && memcmp(out, expect, 1072) == 0,
          "empty content id differs");

    /* explicit content ID lands at 0x30 and changes the HMACs */
    const char *cid = "EP9000-VRADUPDTR_00-0000000000000000";
    CHECK(headbin_make(tmpl, tn, "VRADUPDTR", cid, out) == 0, "make with content id failed");
    CHECK(memcmp(out + 0x30, cid, strlen(cid)) == 0 && out[0x30 + strlen(cid)] == 0, "content id not at 0x30");
    CHECK(memcmp(out + 0x100, expect + 0x100, 16) != 0, "header hmac unchanged for other title");

    CHECK(headbin_make(tmpl, tn, "bad", NULL, out) == -1, "bad title id accepted");
    CHECK(headbin_make(tmpl, 0x200, "VRAD00001", NULL, out) == -1, "short template accepted");
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* head.bin is shipped read-only inside the app, so a bad template is not
 * network-reachable - these pin the guard for whoever next edits that asset.
 * NOTE: the wrap these guards exist for needs a 32-bit size_t (the Vita); this
 * host has a 64-bit one, so the huge-offset cases below are merely rejected
 * here, not wrapped. The boundary cases are what this can really measure. */
static void test_headbin_bounds(void)
{
    uint8_t tmpl[2048], mod[2048], out[2048];
    size_t tn = read_file("../assets/head.bin", tmpl, sizeof(tmpl));

    /* header HMAC lands at `len`, so len + 16 must fit in the template */
    memcpy(mod, tmpl, tn);
    put_be32(mod + 0xD0, (uint32_t)(tn - 16));
    CHECK(headbin_make(mod, tn, "VRAD00001", NULL, out) == 0, "len == size-16 rejected");
    memcpy(mod, tmpl, tn);
    put_be32(mod + 0xD0, (uint32_t)(tn - 15));
    CHECK(headbin_make(mod, tn, "VRAD00001", NULL, out) == -1, "len == size-15 accepted");
    memcpy(mod, tmpl, tn);
    put_be32(mod + 0xD0, 0xFFFFFFF8u);
    CHECK(headbin_make(mod, tn, "VRAD00001", NULL, out) == -1, "header len 0xFFFFFFF8 accepted");

    /* package info: offset, length and destination all bounded */
    memcpy(mod, tmpl, tn);
    put_be32(mod + 0x10, 63);
    CHECK(headbin_make(mod, tn, "VRAD00001", NULL, out) == -1, "info len < 64 accepted");
    memcpy(mod, tmpl, tn);
    put_be32(mod + 0x8, 0xFFFFFF00u);
    CHECK(headbin_make(mod, tn, "VRAD00001", NULL, out) == -1, "info offset 0xFFFFFF00 accepted");
    memcpy(mod, tmpl, tn);
    put_be32(mod + 0xD4, 0xFFFFFFF8u);
    CHECK(headbin_make(mod, tn, "VRAD00001", NULL, out) == -1, "info dst 0xFFFFFFF8 accepted");

    /* the third HMAC covers "everything" and is bounded the same way */
    memcpy(mod, tmpl, tn);
    put_be32(mod + 0xE8, 0xFFFFFFF8u);
    CHECK(headbin_make(mod, tn, "VRAD00001", NULL, out) == -1, "total len 0xFFFFFFF8 accepted");
    memcpy(mod, tmpl, tn);
    put_be32(mod + 0xE8, (uint32_t)(tn - 15));
    CHECK(headbin_make(mod, tn, "VRAD00001", NULL, out) == -1, "total len == size-15 accepted");
}

/* The handover token: the only thing standing between the permanently
 * installed updater bubble and whatever else is sitting in the package dir. */
static void test_update_token(void)
{
    uint8_t sfo[8192], other[8192];
    static const uint8_t NONCE[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    UpdateToken t, bad;
    size_t n = read_file("data/param.sfo", sfo, sizeof(sfo));

    memcpy(other, sfo, n);
    other[n - 1] ^= 0xFF;   /* a different package with the same size */

    update_token_build(&t, "v2.1.0", NONCE, sfo, n);
    CHECK(update_token_check(&t, sizeof(t), sfo, n) == 0, "fresh token rejected");
    CHECK(strcmp(t.tag, "v2.1.0") == 0, "tag '%s'", t.tag);
    CHECK(memcmp(t.nonce, NONCE, sizeof(NONCE)) == 0, "nonce not carried");

    /* the whole point: a token must not validate a different package */
    CHECK(update_token_check(&t, sizeof(t), other, n) == -1, "token accepted another package");
    CHECK(update_token_check(&t, sizeof(t), sfo, n - 1) == -1, "token accepted a truncated sfo");

    /* a token that is absent, short, or from another build is not a token */
    CHECK(update_token_check(&t, sizeof(t) - 1, sfo, n) == -1, "short token accepted");
    CHECK(update_token_check(&t, sizeof(t) + 1, sfo, n) == -1, "over-long token accepted");
    CHECK(update_token_check(NULL, sizeof(t), sfo, n) == -1, "NULL token accepted");
    CHECK(update_token_check(&t, sizeof(t), NULL, 0) == -1, "NULL sfo accepted");

    bad = t;
    bad.magic ^= 1;
    CHECK(update_token_check(&bad, sizeof(bad), sfo, n) == -1, "bad magic accepted");
    bad = t;
    bad.version = UPDATE_TOKEN_VERSION + 1;
    CHECK(update_token_check(&bad, sizeof(bad), sfo, n) == -1, "future version accepted");
    bad = t;
    memset(bad.tag, 'x', sizeof(bad.tag));
    CHECK(update_token_check(&bad, sizeof(bad), sfo, n) == -1, "unterminated tag accepted");
    bad = t;
    bad.sfo_sha1[19] ^= 1;
    CHECK(update_token_check(&bad, sizeof(bad), sfo, n) == -1, "tampered digest accepted");

    /* an all-zero file (a half-written token) must not validate anything */
    memset(&bad, 0, sizeof(bad));
    CHECK(update_token_check(&bad, sizeof(bad), sfo, n) == -1, "zeroed token accepted");

    /* a long tag is truncated, not overflowed, and stays terminated */
    update_token_build(&t, "v1234567890123456789012345678901234567890", NONCE, sfo, n);
    CHECK(t.tag[UPDATE_TOKEN_TAG_LEN - 1] == '\0' && update_token_check(&t, sizeof(t), sfo, n) == 0,
          "long tag broke the token");
}

int main(void)
{
    test_sha1();
    test_sfo();
    test_title_id();
    test_headbin();
    test_headbin_bounds();
    test_update_token();
    printf("test_pkg_meta: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
