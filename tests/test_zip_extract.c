/* Host tests for src/zip_extract.c and src/fs_util.c.
 * Fixtures come from tests/data/make_zip_fixtures.py (Python zipfile).
 *   test_zip_extract                 run the tests
 *   test_zip_extract <zip> <outdir>  just extract (used to diff a real VPK) */
#include "fs_util.h"
#include "zip_extract.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

static int passed, failed;

#define CHECK(cond, ...) do { \
    if (cond) passed++; \
    else { failed++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static int ticks;

static int count_tick(void *user)
{
    (void)user;
    ticks++;
    return 0;
}

static int abort_tick(void *user)
{
    (void)user;
    return 1;
}

static int exists(const char *path)
{
    struct stat sb;
    return stat(path, &sb) == 0;
}

static int is_dir(const char *path)
{
    struct stat sb;
    return stat(path, &sb) == 0 && S_ISDIR(sb.st_mode);
}

static int file_equals(const char *path, const char *want)
{
    char buf[256];
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    return n == strlen(want) && memcmp(buf, want, n) == 0;
}

static int b_bin_ok(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    long i = 0;
    int c, ok = 1;
    while ((c = fgetc(f)) != EOF) {
        if (c != ((i * 31 + 7) & 0xFF)) {
            ok = 0;
            break;
        }
        i++;
    }
    fclose(f);
    return ok && i == 200000;
}

/* ---- hand-built archives ---------------------------------------------------
 * The Python fixtures can only describe archives Python is willing to write.
 * These tests need ones it isn't: a lying uncompressed size, a name longer than
 * the old 255-byte limit, a deflated entry with no compressed bytes at all.
 * Every recorded field is therefore set explicitly by the caller. */

typedef struct {
    const char *name;
    const void *data;      /* the entry payload, stored verbatim */
    uint32_t    len;       /* bytes of payload actually written */
    uint16_t    method;    /* 0 stored, 8 deflate */
    uint32_t    crc;       /* CRC-32 recorded in both headers */
    uint32_t    csize;     /* compressed size recorded in both headers */
    uint32_t    usize;     /* uncompressed size recorded in both headers */
} ZipEnt;

static void put16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/* Writes a one-entry archive. Returns 0 on success. */
static int write_zip(const char *path, const ZipEnt *e)
{
    uint8_t lh[30] = {0}, ch[46] = {0}, eocd[22] = {0};
    size_t nlen = strlen(e->name);
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;

    put32(lh, 0x04034b50u);
    put16(lh + 4, 20);
    put16(lh + 8, e->method);
    put32(lh + 14, e->crc);
    put32(lh + 18, e->csize);
    put32(lh + 22, e->usize);
    put16(lh + 26, (unsigned)nlen);
    fwrite(lh, 1, sizeof(lh), f);
    fwrite(e->name, 1, nlen, f);
    if (e->len)
        fwrite(e->data, 1, e->len, f);

    long cd_off = ftell(f);
    put32(ch, 0x02014b50u);
    put16(ch + 4, 20);
    put16(ch + 6, 20);
    put16(ch + 10, e->method);
    put32(ch + 16, e->crc);
    put32(ch + 20, e->csize);
    put32(ch + 24, e->usize);
    put16(ch + 28, (unsigned)nlen);
    put32(ch + 42, 0);                  /* local header offset */
    fwrite(ch, 1, sizeof(ch), f);
    fwrite(e->name, 1, nlen, f);

    long cd_end = ftell(f);
    put32(eocd, 0x06054b50u);
    put16(eocd + 8, 1);
    put16(eocd + 10, 1);
    put32(eocd + 12, (uint32_t)(cd_end - cd_off));
    put32(eocd + 16, (uint32_t)cd_off);
    fwrite(eocd, 1, sizeof(eocd), f);
    return fclose(f) == 0 ? 0 : -1;
}

/* Fills in the honest crc/csize/usize for a stored entry. */
static ZipEnt stored(const char *name, const void *data, uint32_t len)
{
    ZipEnt e = {name, data, len, 0, 0, len, len};
    e.crc = (uint32_t)crc32(crc32(0L, Z_NULL, 0), (const Bytef *)data, (uInt)len);
    return e;
}

/* "aaa.../aaa.../file" - `parts` components of 99 bytes, 100n-1 bytes overall. */
static void make_name(char *out, size_t outsz, int parts)
{
    size_t n = 0;
    for (int i = 0; i < parts; i++) {
        if (i)
            out[n++] = '/';
        memset(out + n, 'a' + i, 99);
        n += 99;
    }
    if (n < outsz)
        out[n] = '\0';
}

static void test_crafted(const char *root)
{
    char zip[512], dir[512], path[1400], name[700];
    static const char BODY[] = "payload";
    ZipEnt e;
    int r;

    snprintf(zip, sizeof(zip), "%s/crafted.zip", root);

    /* A deflated entry that is genuinely empty has no stream to inflate. */
    e = stored("empty.txt", "", 0);
    e.method = 8;
    CHECK(write_zip(zip, &e) == 0, "write empty deflate zip");
    snprintf(dir, sizeof(dir), "%s/empty", root);
    r = zip_extract(zip, dir, NULL, NULL);
    CHECK(r == ZIP_OK, "empty deflated entry: %d %s", r, zip_strerror(r));
    snprintf(path, sizeof(path), "%s/empty.txt", dir);
    CHECK(exists(path), "empty.txt not created");

    /* A 299-byte name is legal (the spec allows 65535) and fits FS_PATH_LEN.
     * Split into components, because 255 bytes is the per-component limit of
     * every filesystem involved - it is the whole name that used to be
     * rejected, and only for being over 255. */
    make_name(name, sizeof(name), 3);
    CHECK(strlen(name) == 299, "long name is %zu bytes", strlen(name));
    e = stored(name, BODY, (uint32_t)strlen(BODY));
    CHECK(write_zip(zip, &e) == 0, "write long-name zip");
    snprintf(dir, sizeof(dir), "%s/longname", root);
    r = zip_extract(zip, dir, NULL, NULL);
    CHECK(r == ZIP_OK, "299-byte name: %d %s", r, zip_strerror(r));
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    CHECK(file_equals(path, BODY), "long-name entry content");

    /* Longer than the target path can hold: refused, but as "unsupported". */
    make_name(name, sizeof(name), 6);
    e = stored(name, BODY, (uint32_t)strlen(BODY));
    CHECK(write_zip(zip, &e) == 0, "write over-long-name zip");
    snprintf(dir, sizeof(dir), "%s/toolong", root);
    r = zip_extract(zip, dir, NULL, NULL);
    CHECK(r == ZIP_ERR_UNSUPPORTED, "599-byte name: %d %s", r, zip_strerror(r));

    /* A zip bomb: 200 MB declared uncompressed, refused before anything is
     * written. usize alone is overridden, so the old code got as far as
     * creating (and truncating) the output file. */
    e = stored("bomb.bin", BODY, (uint32_t)strlen(BODY));
    e.usize = 200u * 1024 * 1024;
    CHECK(write_zip(zip, &e) == 0, "write bomb zip");
    snprintf(dir, sizeof(dir), "%s/bomb", root);
    r = zip_extract(zip, dir, NULL, NULL);
    CHECK(r == ZIP_ERR_TOO_BIG, "declared 200 MB: %d %s", r, zip_strerror(r));
    snprintf(path, sizeof(path), "%s/bomb.bin", dir);
    CHECK(!exists(path), "bomb.bin was created before the size was checked");

    /* Just under the cap still extracts (the cap must not reject real VPKs). */
    e = stored("ok.bin", BODY, (uint32_t)strlen(BODY));
    CHECK(write_zip(zip, &e) == 0, "write ok zip");
    snprintf(dir, sizeof(dir), "%s/undercap", root);
    r = zip_extract(zip, dir, NULL, NULL);
    CHECK(r == ZIP_OK, "small archive rejected by the cap: %d %s", r, zip_strerror(r));
}

int main(int argc, char **argv)
{
    if (argc == 3) {
        int r = zip_extract(argv[1], argv[2], NULL, NULL);
        printf("extract %s -> %s: %d (%s)\n", argv[1], argv[2], r, zip_strerror(r));
        return r == ZIP_OK ? 0 : 1;
    }

    char root_tmpl[] = "/tmp/vr_zip_XXXXXX";
    char *root = mkdtemp(root_tmpl);
    char dir[256], path[512];
    if (!root) {
        printf("mkdtemp failed\n");
        return 1;
    }

    /* stored + deflated + directory entry */
    snprintf(dir, sizeof(dir), "%s/good", root);
    int r = zip_extract("data/zip_good.zip", dir, count_tick, NULL);
    CHECK(r == ZIP_OK, "good: %d %s", r, zip_strerror(r));
    snprintf(path, sizeof(path), "%s/a.txt", dir);
    CHECK(file_equals(path, "hello vita radio\n"), "a.txt content");
    snprintf(path, sizeof(path), "%s/sub/dir/b.bin", dir);
    CHECK(b_bin_ok(path), "b.bin content");
    snprintf(path, sizeof(path), "%s/emptydir", dir);
    CHECK(is_dir(path), "emptydir not created");
    CHECK(ticks > 0, "tick never called");

    /* ".." entry is refused and nothing escapes */
    snprintf(dir, sizeof(dir), "%s/evil/inner", root);
    r = zip_extract("data/zip_evil.zip", dir, NULL, NULL);
    CHECK(r == ZIP_ERR_UNSAFE_NAME, "evil: %d", r);
    snprintf(path, sizeof(path), "%s/evil/evil.txt", root);
    CHECK(!exists(path), "evil.txt escaped the target dir");

    snprintf(dir, sizeof(dir), "%s/crc", root);
    r = zip_extract("data/zip_crc.zip", dir, NULL, NULL);
    CHECK(r == ZIP_ERR_CRC, "stored corruption: %d", r);

    snprintf(dir, sizeof(dir), "%s/deflate_bad", root);
    r = zip_extract("data/zip_deflate_bad.zip", dir, NULL, NULL);
    CHECK(r == ZIP_ERR_DATA || r == ZIP_ERR_CRC, "deflate corruption: %d", r);

    snprintf(dir, sizeof(dir), "%s/trunc", root);
    r = zip_extract("data/zip_truncated.zip", dir, NULL, NULL);
    CHECK(r == ZIP_ERR_FORMAT, "truncated: %d", r);

    r = zip_extract("data/param.sfo", dir, NULL, NULL);
    CHECK(r == ZIP_ERR_FORMAT, "not a zip: %d", r);

    r = zip_extract("data/no_such.zip", dir, NULL, NULL);
    CHECK(r == ZIP_ERR_OPEN, "missing: %d", r);

    snprintf(dir, sizeof(dir), "%s/abort", root);
    r = zip_extract("data/zip_good.zip", dir, abort_tick, NULL);
    CHECK(r == ZIP_ERR_ABORTED, "abort: %d", r);

    CHECK(zip_name_safe("sce_sys/param.sfo"), "normal name");
    CHECK(zip_name_safe("a..b/c"), "dots inside a name");
    CHECK(!zip_name_safe("/abs"), "absolute");
    CHECK(!zip_name_safe("ux0:data/x"), "device");
    CHECK(!zip_name_safe("a/../b"), "inner ..");
    CHECK(!zip_name_safe("a\\..\\b"), "backslash ..");
    CHECK(!zip_name_safe(""), "empty");

    test_crafted(root);

    fs_rm_tree(root);
    CHECK(!exists(root), "rm_tree left %s", root);

    printf("test_zip_extract: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
