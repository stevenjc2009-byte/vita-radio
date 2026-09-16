#include "favourites.h"
#include "station_list.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(_WIN32)
#include <direct.h>
#define VR_TEST_MKDIR(p) _mkdir(p)
#else
#define VR_TEST_MKDIR(p) mkdir((p), 0777)
#endif

static int g_fail = 0, g_pass = 0;

#define CHECK(name, cond) do { \
    if (cond) { printf("PASS %s\n", name); g_pass++; } \
    else { printf("FAIL %s (%s:%d)\n", name, __FILE__, __LINE__); g_fail++; } \
} while (0)

#define P_MAIN   "/tmp/vr_fav_test.tsv"
#define P_TRUNC  "/tmp/vr_fav_test_trunc.tsv"
#define P_EMPTY  "/tmp/vr_fav_test_empty.tsv"
#define P_MISS   "/tmp/vr_fav_test_missing.tsv"
#define P_WEIRD  "/tmp/vr_fav_test_weird.tsv"
#define P_LOCK   "/tmp/vr_fav_test_locked.tsv"   /* stood up as a directory */

static void write_raw(const char *path, const char *bytes)
{
    FILE *f = fopen(path, "wb");
    if (!f) { printf("FAIL setup_write_raw(%s)\n", path); g_fail++; return; }
    fwrite(bytes, 1, strlen(bytes), f);
    fclose(f);
}

static int file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static void tmp_path_of(const char *path, char *out, size_t n)
{
    snprintf(out, n, "%s.tmp", path);
}

/* The save keeps the outgoing file under this name across the rename rather
 * than deleting it, so a rename that fails still leaves something to recover. */
static void bak_path_of(const char *path, char *out, size_t n)
{
    snprintf(out, n, "%s.bak", path);
}

int main(void)
{
    StationList a, b;
    char tmppath[256], bakpath[256];

    remove(P_MAIN); remove(P_TRUNC); remove(P_EMPTY); remove(P_MISS); remove(P_WEIRD);
    tmp_path_of(P_MAIN, tmppath, sizeof tmppath);
    bak_path_of(P_MAIN, bakpath, sizeof bakpath);
    remove(tmppath);
    remove(bakpath);

    /* ---- missing file is not an error ------------------------------ */
    sl_init(&a);
    sl_add(&a, "Pre-existing", "http://pre/", "MP3");
    CHECK("missing_file_returns_0", fav_load(&a, P_MISS) == 0);
    CHECK("missing_file_adds_nothing", a.count == 1);
    sl_free(&a);

    /* ---- save / load round trip, only is_fav entries saved --------- */
    sl_init(&a);
    sl_add(&a, "Alpha", "http://a/", "MP3");
    sl_add(&a, "Bravo", "http://b/", "AAC");
    sl_add(&a, "Charlie", "http://c/", "HLS");
    a.items[0].is_fav = 1;
    a.items[2].is_fav = 1;             /* Bravo deliberately left unfavourited */
    CHECK("save_returns_0", fav_save(&a, P_MAIN) == 0);
    CHECK("save_wrote_file", file_exists(P_MAIN));

    /* .tmp-and-rename leaves no .tmp behind */
    CHECK("save_leaves_no_tmp", !file_exists(tmppath));
    /* Nor a .bak: there was no previous file to stand aside. */
    CHECK("first_save_leaves_no_bak", !file_exists(bakpath));

    sl_init(&b);
    CHECK("load_returns_0", fav_load(&b, P_MAIN) == 0);
    CHECK("load_only_favs", b.count == 2);
    CHECK("load_roundtrip_0", b.count == 2
                           && strcmp(b.items[0].name, "Alpha") == 0
                           && strcmp(b.items[0].url, "http://a/") == 0
                           && strcmp(b.items[0].kind, "MP3") == 0);
    CHECK("load_roundtrip_1", b.count == 2
                           && strcmp(b.items[1].name, "Charlie") == 0
                           && strcmp(b.items[1].url, "http://c/") == 0
                           && strcmp(b.items[1].kind, "HLS") == 0);
    CHECK("load_sets_is_fav", b.count == 2 && b.items[0].is_fav == 1 && b.items[1].is_fav == 1);
    CHECK("load_did_not_save_bravo", sl_find_url(&b, "http://b/") == -1);

    /* load appends rather than replacing */
    CHECK("load_appends", fav_load(&b, P_MAIN) == 0 && b.count == 4);
    sl_free(&b);

    /* ---- overwriting an existing file still leaves no .tmp --------- */
    a.items[2].is_fav = 0;
    CHECK("resave_over_existing", fav_save(&a, P_MAIN) == 0);
    CHECK("resave_leaves_no_tmp", !file_exists(tmppath));
    /* The old copy stands aside as .bak during the rename and is cleared once
     * the new one is in place - leaving it behind would grow forever. */
    CHECK("resave_leaves_no_bak", !file_exists(bakpath));
    /* And the target is never absent at the end of a save. */
    CHECK("resave_target_present", file_exists(P_MAIN));
    sl_init(&b);
    CHECK("resave_content", fav_load(&b, P_MAIN) == 0 && b.count == 1
                         && strcmp(b.items[0].name, "Alpha") == 0);
    sl_free(&b);
    sl_free(&a);

    /* ---- nothing favourited: an empty file that loads as 0 --------- */
    sl_init(&a);
    sl_add(&a, "None", "http://n/", "MP3");
    CHECK("save_no_favs", fav_save(&a, P_MAIN) == 0);
    sl_free(&a);
    sl_init(&b);
    CHECK("load_file_with_no_entries", fav_load(&b, P_MAIN) == 0 && b.count == 0);
    sl_free(&b);

    /* ---- a zero-byte file ------------------------------------------ */
    write_raw(P_EMPTY, "");
    sl_init(&b);
    CHECK("load_zero_byte_file", fav_load(&b, P_EMPTY) == 0 && b.count == 0);
    sl_free(&b);

    /* ---- an empty FIRST line ---------------------------------------- */
    /* A blank line later in the file is survivable because an earlier record
     * has already grown the read buffer. When the very first line is empty
     * there is no buffer yet, and fav_load runs at startup, so a single stray
     * "\n" would take the app down on every boot. */
    write_raw(P_EMPTY, "\n");
    sl_init(&b);
    CHECK("load_leading_empty_line_only", fav_load(&b, P_EMPTY) == 0 && b.count == 0);
    sl_free(&b);

    write_raw(P_EMPTY, "\n\nOne\thttp://1/\tMP3\n");
    sl_init(&b);
    CHECK("load_leading_empty_lines_then_row", fav_load(&b, P_EMPTY) == 0 && b.count == 1
                                            && strcmp(b.items[0].name, "One") == 0);
    sl_free(&b);

    write_raw(P_EMPTY, "\r\nOne\thttp://1/\tMP3\n");
    sl_init(&b);
    CHECK("load_leading_empty_crlf_line", fav_load(&b, P_EMPTY) == 0 && b.count == 1
                                       && strcmp(b.items[0].name, "One") == 0);
    sl_free(&b);

    /* ---- truncated final line is dropped, earlier lines survive ---- */
    write_raw(P_TRUNC,
              "One\thttp://1/\tMP3\n"
              "Two\thttp://2/\tAAC\n"
              "Thr\thttp://3/\tMP");            /* power cut mid-line */
    sl_init(&b);
    CHECK("truncated_returns_0", fav_load(&b, P_TRUNC) == 0);
    CHECK("truncated_keeps_earlier", b.count == 2);
    CHECK("truncated_earlier_values", b.count == 2
                                   && strcmp(b.items[0].name, "One") == 0
                                   && strcmp(b.items[1].name, "Two") == 0);
    CHECK("truncated_dropped_partial", sl_find_url(&b, "http://3/") == -1);
    sl_free(&b);

    /* A last line that is complete but unterminated is also dropped -
     * we cannot tell it apart from a power cut, so the rule is uniform. */
    write_raw(P_TRUNC, "One\thttp://1/\tMP3\nTwo\thttp://2/\tAAC");
    sl_init(&b);
    CHECK("unterminated_last_line_dropped", fav_load(&b, P_TRUNC) == 0 && b.count == 1
                                         && strcmp(b.items[0].name, "One") == 0);
    sl_free(&b);

    /* A line with too few fields is skipped, the rest still load. */
    write_raw(P_TRUNC, "One\thttp://1/\tMP3\nbroken-line\nTwo\thttp://2/\tAAC\n");
    sl_init(&b);
    CHECK("malformed_line_skipped", fav_load(&b, P_TRUNC) == 0 && b.count == 2
                                 && strcmp(b.items[1].name, "Two") == 0);
    sl_free(&b);

    /* ---- tab / newline in a field: sanitised to spaces on save ----- */
    sl_init(&a);
    sl_add(&a, "Bad\tName\nHere\r!", "http://w/\tx", "M\nP3");
    a.items[0].is_fav = 1;
    sl_add(&a, "After", "http://after/", "AAC");
    a.items[1].is_fav = 1;
    CHECK("weird_save", fav_save(&a, P_WEIRD) == 0);
    sl_free(&a);

    sl_init(&b);
    CHECK("weird_load", fav_load(&b, P_WEIRD) == 0);
    CHECK("weird_row_count", b.count == 2);
    CHECK("weird_name_sanitised", b.count == 2 && strcmp(b.items[0].name, "Bad Name Here !") == 0);
    CHECK("weird_url_sanitised", b.count == 2 && strcmp(b.items[0].url, "http://w/ x") == 0);
    CHECK("weird_kind_sanitised", b.count == 2 && strcmp(b.items[0].kind, "M P3") == 0);
    CHECK("weird_next_row_intact", b.count == 2 && strcmp(b.items[1].name, "After") == 0
                                && strcmp(b.items[1].url, "http://after/") == 0);
    sl_free(&b);

    /* ---- a very long line (past any internal read buffer) ---------- */
    {
        char *big = (char *)malloc(9000);
        size_t i;
        for (i = 0; i < 8191; i++) big[i] = 'L';
        big[8191] = 0;
        sl_init(&a);
        sl_add(&a, big, "http://long/", "MP3");
        a.items[0].is_fav = 1;
        CHECK("long_save", fav_save(&a, P_MAIN) == 0);
        sl_free(&a);
        sl_init(&b);
        CHECK("long_load", fav_load(&b, P_MAIN) == 0 && b.count == 1);
        CHECK("long_roundtrip", b.count == 1 && strcmp(b.items[0].name, big) == 0);
        sl_free(&b);
        free(big);
    }

    /* ---- NULL arguments do not crash ------------------------------- */
    sl_init(&b);
    CHECK("load_null_path", fav_load(&b, NULL) == -1);
    CHECK("save_null_path", fav_save(&b, NULL) == -1);
    CHECK("load_null_list", fav_load(NULL, P_MAIN) == -1);
    CHECK("save_null_list", fav_save(NULL, P_MAIN) == -1);
    sl_free(&b);

    /* ---- a save that cannot complete keeps the good new file -------- */
    /* The only way to make both renames fail on the host is to put something
     * unrenameable in the way, so the target and its .bak are non-empty
     * directories. What is being checked is not the directories - it is that
     * the failure path no longer deletes the finished .tmp. That file is the
     * only copy of the new favourites, and on the Vita this branch is the
     * normal path, so throwing it away loses the list outright. */
    {
        char lock_tmp[256], lock_bak[256];

        tmp_path_of(P_LOCK, lock_tmp, sizeof lock_tmp);
        bak_path_of(P_LOCK, lock_bak, sizeof lock_bak);
        remove(lock_tmp);
        VR_TEST_MKDIR(P_LOCK);
        write_raw(P_LOCK "/keep", "x");
        VR_TEST_MKDIR(lock_bak);
        write_raw(P_LOCK ".bak/keep", "x");

        sl_init(&a);
        sl_add(&a, "Fresh", "http://fresh/", "MP3");
        a.items[0].is_fav = 1;
        CHECK("blocked_save_reports_failure", fav_save(&a, P_LOCK) == -1);
        CHECK("blocked_save_keeps_new_copy", file_exists(lock_tmp));
        sl_free(&a);

        /* And what it left behind is the complete new list, not a fragment. */
        sl_init(&b);
        CHECK("blocked_save_copy_is_complete",
              fav_load(&b, lock_tmp) == 0 && b.count == 1
              && strcmp(b.items[0].name, "Fresh") == 0
              && strcmp(b.items[0].url, "http://fresh/") == 0);
        sl_free(&b);

        remove(lock_tmp);
        remove(P_LOCK "/keep");   rmdir(P_LOCK);
        remove(P_LOCK ".bak/keep"); rmdir(lock_bak);
    }

    remove(P_MAIN); remove(P_TRUNC); remove(P_EMPTY); remove(P_WEIRD);
    remove(tmppath);
    remove(bakpath);

    printf("test_favourites: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
