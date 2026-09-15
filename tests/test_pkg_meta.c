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

int main(void)
{
    test_sha1();
    test_sfo();
    test_title_id();
    test_headbin();
    printf("test_pkg_meta: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
