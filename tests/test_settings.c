#include "settings.h"
#include "theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0, g_pass = 0;

#define CHECK(name, cond) do { \
    if (cond) { printf("PASS %s\n", name); g_pass++; } \
    else { printf("FAIL %s (%s:%d)\n", name, __FILE__, __LINE__); g_fail++; } \
} while (0)

#define P_MAIN   "/tmp/vr_settings_test.tsv"
#define P_RAW    "/tmp/vr_settings_test_raw.tsv"
#define P_MISS   "/tmp/vr_settings_test_missing.tsv"
/* The parent directory deliberately does not exist, so the .tmp cannot even be
 * opened. That is the cheapest way to make a save fail on the host without
 * standing up directories that then have to be torn down. */
#define P_NODIR  "/tmp/vr_settings_test_no_such_dir/settings.tsv"

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

static int in_range(int theme)
{
    return theme >= 0 && theme < VR_THEME_COUNT;
}

/* Loads `bytes` over a settings struct whose theme has been pre-set to
 * `start`, and reports what came back. Every corrupt-input case below runs
 * through here so they all assert the same two things: the return code, and
 * that the field was left alone. */
static int load_bytes(const char *bytes, int start, int *out_theme)
{
    Settings s;
    int rc;

    write_raw(P_RAW, bytes);
    settings_defaults(&s);
    s.theme = start;
    rc = settings_load(&s, P_RAW);
    *out_theme = s.theme;
    return rc;
}

int main(void)
{
    Settings s, t;
    char tmppath[256], bakpath[256];
    int theme, rc, i;

    snprintf(tmppath, sizeof tmppath, "%s.tmp", P_MAIN);
    snprintf(bakpath, sizeof bakpath, "%s.bak", P_MAIN);
    remove(P_MAIN); remove(P_RAW); remove(P_MISS);
    remove(tmppath); remove(bakpath);

    /* ---- defaults --------------------------------------------------- */
    settings_defaults(&s);
    CHECK("defaults_theme_in_range", in_range(s.theme));
    CHECK("defaults_theme_is_zero", s.theme == 0);
    /* Called twice it must give the same answer - the UI calls it on every
     * failed load, not only at startup. */
    settings_defaults(&t);
    CHECK("defaults_repeatable", t.theme == s.theme);

    /* ---- save / load round trip ------------------------------------- */
    settings_defaults(&s);
    s.theme = 2;
    CHECK("save_returns_0", settings_save(&s, P_MAIN) == 0);
    CHECK("save_wrote_file", file_exists(P_MAIN));
    CHECK("save_leaves_no_tmp", !file_exists(tmppath));
    CHECK("first_save_leaves_no_bak", !file_exists(bakpath));

    settings_defaults(&t);
    CHECK("load_returns_0", settings_load(&t, P_MAIN) == 0);
    CHECK("load_roundtrip", t.theme == 2);

    /* Every valid index survives the round trip, not just the one above. */
    {
        int all_ok = 1;
        for (i = 0; i < VR_THEME_COUNT; i++) {
            settings_defaults(&s);
            s.theme = i;
            if (settings_save(&s, P_MAIN) != 0) { all_ok = 0; break; }
            settings_defaults(&t);
            if (settings_load(&t, P_MAIN) != 0 || t.theme != i) { all_ok = 0; break; }
        }
        CHECK("roundtrip_every_theme", all_ok);
    }

    /* Re-saving over an existing file must not leave the .tmp or the .bak
     * behind - on the Vita the rename fallback is the normal path. */
    settings_defaults(&s);
    s.theme = 1;
    CHECK("resave_over_existing", settings_save(&s, P_MAIN) == 0);
    CHECK("resave_leaves_no_tmp", !file_exists(tmppath));
    CHECK("resave_leaves_no_bak", !file_exists(bakpath));
    CHECK("resave_target_present", file_exists(P_MAIN));
    settings_defaults(&t);
    CHECK("resave_content", settings_load(&t, P_MAIN) == 0 && t.theme == 1);

    /* ---- a missing file is not an error ----------------------------- */
    settings_defaults(&s);
    s.theme = 2;
    CHECK("missing_file_returns_0", settings_load(&s, P_MISS) == 0);
    CHECK("missing_file_leaves_field", s.theme == 2);

    /* ---- an empty file ---------------------------------------------- */
    rc = load_bytes("", 3, &theme);
    CHECK("empty_file_returns_0", rc == 0);
    CHECK("empty_file_leaves_field", theme == 3);

    /* A lone newline: on the first line of the file nothing has been pushed
     * into the read buffer yet, which is where favourites.c had a bug. */
    rc = load_bytes("\n", 3, &theme);
    CHECK("leading_empty_line_returns_0", rc == 0);
    CHECK("leading_empty_line_leaves_field", theme == 3);
    rc = load_bytes("\n\ntheme\t1\n", 3, &theme);
    CHECK("empty_lines_then_good_line", rc == 0 && theme == 1);
    rc = load_bytes("\r\ntheme\t1\n", 3, &theme);
    CHECK("leading_empty_crlf_line", rc == 0 && theme == 1);

    /* ---- an unknown key is skipped, not an error -------------------- */
    rc = load_bytes("volume\t9\n", 2, &theme);
    CHECK("unknown_key_returns_0", rc == 0);
    CHECK("unknown_key_leaves_field", theme == 2);
    rc = load_bytes("volume\t9\ntheme\t1\nsleep\t30\n", 2, &theme);
    CHECK("unknown_keys_around_good_line", rc == 0 && theme == 1);
    /* A key that merely starts with "theme" is a different key. */
    rc = load_bytes("theme_x\t1\n", 2, &theme);
    CHECK("prefix_key_is_not_theme", rc == 0 && theme == 2);

    /* ---- out of range is ignored ------------------------------------ */
    rc = load_bytes("theme\t-1\n", 2, &theme);
    CHECK("negative_theme_returns_0", rc == 0);
    CHECK("negative_theme_ignored", theme == 2);
    rc = load_bytes("theme\t-99999\n", 2, &theme);
    CHECK("large_negative_theme_ignored", rc == 0 && theme == 2);
    {
        char buf[64];
        snprintf(buf, sizeof buf, "theme\t%d\n", VR_THEME_COUNT);
        rc = load_bytes(buf, 2, &theme);
        CHECK("theme_at_count_ignored", rc == 0 && theme == 2);
    }
    rc = load_bytes("theme\t99\n", 2, &theme);
    CHECK("theme_above_count_ignored", rc == 0 && theme == 2);
    /* Bigger than an int: must be rejected, not wrapped into range. */
    rc = load_bytes("theme\t99999999999999999999\n", 2, &theme);
    CHECK("theme_overflow_ignored", rc == 0 && theme == 2);
    /* The highest valid index is accepted - the range check must not be
     * off by one in the other direction. */
    {
        char buf[64];
        snprintf(buf, sizeof buf, "theme\t%d\n", VR_THEME_COUNT - 1);
        rc = load_bytes(buf, 0, &theme);
        CHECK("theme_at_count_minus_one_accepted", rc == 0 && theme == VR_THEME_COUNT - 1);
    }

    /* ---- values that are not integers ------------------------------- */
    rc = load_bytes("theme\tblue\n", 2, &theme);
    CHECK("non_numeric_returns_0", rc == 0);
    CHECK("non_numeric_ignored", theme == 2);
    rc = load_bytes("theme\t1x\n", 2, &theme);
    CHECK("trailing_junk_ignored", rc == 0 && theme == 2);
    rc = load_bytes("theme\tx1\n", 2, &theme);
    CHECK("leading_junk_ignored", rc == 0 && theme == 2);
    rc = load_bytes("theme\t\n", 2, &theme);
    CHECK("empty_value_ignored", rc == 0 && theme == 2);
    rc = load_bytes("theme\t1.9\n", 2, &theme);
    CHECK("float_value_ignored", rc == 0 && theme == 2);

    /* ---- a line with no tab at all ---------------------------------- */
    rc = load_bytes("theme1\n", 2, &theme);
    CHECK("no_tab_returns_0", rc == 0);
    CHECK("no_tab_ignored", theme == 2);
    rc = load_bytes("garbage\ntheme\t1\n", 2, &theme);
    CHECK("no_tab_then_good_line", rc == 0 && theme == 1);

    /* ---- CRLF ------------------------------------------------------- */
    rc = load_bytes("theme\t1\r\n", 2, &theme);
    CHECK("crlf_value_parsed", rc == 0 && theme == 1);
    rc = load_bytes("volume\t9\r\ntheme\t3\r\n", 0, &theme);
    CHECK("crlf_multi_line", rc == 0 && theme == 3);
    /* An out-of-range value must stay out of range with a CR attached: if the
     * CR were left on, "9\r" would fail to parse for the wrong reason and this
     * check would pass while "1\r" quietly broke. */
    rc = load_bytes("theme\t99\r\n", 2, &theme);
    CHECK("crlf_out_of_range_still_ignored", rc == 0 && theme == 2);

    /* ---- a truncated final line is dropped -------------------------- */
    /* A power cut mid-write. The half-line must not be parsed, but everything
     * before it still loads - so the good line sets 1 and the partial line's
     * 3 must never land. */
    rc = load_bytes("theme\t1\ntheme\t3", 0, &theme);
    CHECK("truncated_final_line_dropped", rc == 0 && theme == 1);
    rc = load_bytes("volume\t9\ntheme\t", 2, &theme);
    CHECK("truncated_mid_value_dropped", rc == 0 && theme == 2);
    rc = load_bytes("theme\t1\r\ntheme\t3\r", 0, &theme);
    CHECK("truncated_crlf_final_line_dropped", rc == 0 && theme == 1);

    /* ---- later lines win over earlier ones -------------------------- */
    rc = load_bytes("theme\t1\ntheme\t3\n", 0, &theme);
    CHECK("last_theme_line_wins", rc == 0 && theme == 3);
    /* A bad line after a good one leaves the good value, it does not reset
     * the field to the default. */
    rc = load_bytes("theme\t3\ntheme\tbogus\n", 0, &theme);
    CHECK("bad_line_after_good_keeps_good", rc == 0 && theme == 3);

    /* ---- a line longer than any plausible stack buffer -------------- */
    {
        size_t big_n = 9000;
        char *big = (char *)malloc(big_n + 32);
        if (!big) { printf("FAIL setup_alloc_big\n"); g_fail++; }
        else {
            memset(big, 'K', big_n);
            memcpy(big + big_n, "\t1\ntheme\t3\n", 12);
            rc = load_bytes(big, 0, &theme);
            CHECK("long_unknown_key_then_good_line", rc == 0 && theme == 3);

            memcpy(big, "theme\t", 6);
            memset(big + 6, '7', big_n - 6);
            memcpy(big + big_n, "\ntheme\t3\n", 10);
            rc = load_bytes(big, 0, &theme);
            CHECK("long_value_then_good_line", rc == 0 && theme == 3);
            free(big);
        }
    }

    /* ---- a corrupt file can never produce an invalid theme ---------- */
    {
        static const char *corrupt[] = {
            "theme\t-1\n", "theme\t4\n", "theme\t999\n", "theme\tblue\n",
            "theme\t\n", "theme\n", "\t\n", "\n\n\n", "theme\t-0\n",
            "theme\t 1\n", "theme\t1 \n", "theme\t+1\n", "theme\t0x1\n"
        };
        size_t n;
        int all_in_range = 1, all_zero_rc = 1;
        for (n = 0; n < sizeof corrupt / sizeof corrupt[0]; n++) {
            rc = load_bytes(corrupt[n], 0, &theme);
            if (rc != 0) all_zero_rc = 0;
            if (!in_range(theme)) all_in_range = 0;
        }
        CHECK("corrupt_lines_never_error", all_zero_rc);
        CHECK("corrupt_lines_never_leave_invalid_theme", all_in_range);
    }

    /* ---- a save that cannot open its .tmp reports failure ----------- */
    settings_defaults(&s);
    s.theme = 1;
    CHECK("save_to_unwritable_path_returns_minus_1", settings_save(&s, P_NODIR) == -1);

    /* ---- NULL arguments do not crash -------------------------------- */
    settings_defaults(&s);
    CHECK("load_null_path", settings_load(&s, NULL) == -1);
    CHECK("save_null_path", settings_save(&s, NULL) == -1);
    CHECK("load_null_settings", settings_load(NULL, P_MAIN) == -1);
    CHECK("save_null_settings", settings_save(NULL, P_MAIN) == -1);
    CHECK("null_path_load_left_field", s.theme == 0);

    remove(P_MAIN); remove(P_RAW);
    remove(tmppath); remove(bakpath);

    printf("test_settings: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
