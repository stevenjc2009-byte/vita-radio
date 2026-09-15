/* Host tests for src/zip_extract.c and src/fs_util.c.
 * Fixtures come from tests/data/make_zip_fixtures.py (Python zipfile).
 *   test_zip_extract                 run the tests
 *   test_zip_extract <zip> <outdir>  just extract (used to diff a real VPK) */
#include "fs_util.h"
#include "zip_extract.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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

    fs_rm_tree(root);
    CHECK(!exists(root), "rm_tree left %s", root);

    printf("test_zip_extract: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
