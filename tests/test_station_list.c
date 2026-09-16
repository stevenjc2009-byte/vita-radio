#include "station_list.h"
#include "stations.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0, g_pass = 0;

#define CHECK(name, cond) do { \
    if (cond) { printf("PASS %s\n", name); g_pass++; } \
    else { printf("FAIL %s (%s:%d)\n", name, __FILE__, __LINE__); g_fail++; } \
} while (0)

static int seeded(StationList *l)
{
    sl_init(l);
    return sl_add(l, "Alpha", "http://a/", "MP3") == 0
        && sl_add(l, "Bravo", "http://b/", "AAC") == 1
        && sl_add(l, "Charlie", "http://c/", "HLS") == 2;
}

int main(void)
{
    StationList l;
    int i;

    /* ---- init / add / find ---------------------------------------- */
    sl_init(&l);
    CHECK("init_empty", l.count == 0 && l.items == NULL && l.cap == 0);

    CHECK("add_returns_index_0", sl_add(&l, "Alpha", "http://a/", "MP3") == 0);
    CHECK("add_returns_index_1", sl_add(&l, "Bravo", "http://b/", "AAC") == 1);
    CHECK("count_after_two", l.count == 2);
    CHECK("add_copies_name", strcmp(l.items[0].name, "Alpha") == 0);
    CHECK("add_copies_url", strcmp(l.items[0].url, "http://a/") == 0);
    CHECK("add_copies_kind", strcmp(l.items[0].kind, "MP3") == 0);
    CHECK("add_is_fav_clear", l.items[0].is_fav == 0);

    /* Copies, not aliases: the caller's buffer may go away. */
    {
        char tmp[32];
        int idx;
        strcpy(tmp, "Volatile");
        idx = sl_add(&l, tmp, "http://v/", "MP3");
        memset(tmp, 'x', sizeof tmp - 1);
        tmp[sizeof tmp - 1] = 0;
        CHECK("add_string_is_owned_copy", idx == 2 && strcmp(l.items[2].name, "Volatile") == 0);
    }

    CHECK("find_url_first", sl_find_url(&l, "http://a/") == 0);
    CHECK("find_url_last", sl_find_url(&l, "http://v/") == 2);
    CHECK("find_url_miss", sl_find_url(&l, "http://nope/") == -1);
    CHECK("find_url_null", sl_find_url(&l, NULL) == -1);
    sl_free(&l);

    /* ---- remove first / middle / last ------------------------------ */
    CHECK("seed_for_remove_first", seeded(&l));
    CHECK("remove_first_ok", sl_remove(&l, 0) == 0);
    CHECK("remove_first_count", l.count == 2);
    CHECK("remove_first_shifted", strcmp(l.items[0].name, "Bravo") == 0
                               && strcmp(l.items[1].name, "Charlie") == 0);
    CHECK("remove_first_gone_from_find", sl_find_url(&l, "http://a/") == -1);
    sl_free(&l);

    CHECK("seed_for_remove_mid", seeded(&l));
    CHECK("remove_mid_ok", sl_remove(&l, 1) == 0);
    CHECK("remove_mid_shifted", l.count == 2
                             && strcmp(l.items[0].name, "Alpha") == 0
                             && strcmp(l.items[1].name, "Charlie") == 0);
    sl_free(&l);

    CHECK("seed_for_remove_last", seeded(&l));
    CHECK("remove_last_ok", sl_remove(&l, 2) == 0);
    CHECK("remove_last_shifted", l.count == 2
                              && strcmp(l.items[0].name, "Alpha") == 0
                              && strcmp(l.items[1].name, "Bravo") == 0);

    /* ---- out of range ---------------------------------------------- */
    CHECK("remove_negative", sl_remove(&l, -1) == -1);
    CHECK("remove_at_count", sl_remove(&l, l.count) == -1);
    CHECK("remove_far_past_end", sl_remove(&l, 9999) == -1);
    CHECK("remove_out_of_range_kept_count", l.count == 2);
    sl_free(&l);

    sl_init(&l);
    CHECK("remove_from_empty", sl_remove(&l, 0) == -1);
    sl_free(&l);

    /* ---- growth past the initial capacity -------------------------- */
    sl_init(&l);
    {
        int all_idx = 1;
        for (i = 0; i < 100; i++) {
            char name[32], url[32];
            sprintf(name, "st%d", i);
            sprintf(url, "http://h/%d", i);
            if (sl_add(&l, name, url, "MP3") != i)
                all_idx = 0;
        }
        CHECK("grow_100_indices", all_idx);
        CHECK("grow_100_count", l.count == 100);
        CHECK("grow_100_cap", l.cap >= 100);
    }
    {
        int all_ok = 1;
        for (i = 0; i < 100; i++) {
            char name[32], url[32];
            sprintf(name, "st%d", i);
            sprintf(url, "http://h/%d", i);
            if (sl_find_url(&l, url) != i || strcmp(l.items[i].name, name) != 0)
                all_ok = 0;
        }
        CHECK("grow_100_contents_intact", all_ok);
    }

    /* ---- clear keeps the allocation, then reuse -------------------- */
    {
        int cap_before = l.cap;
        sl_clear(&l);
        CHECK("clear_count_zero", l.count == 0);
        CHECK("clear_keeps_allocation", l.cap == cap_before && l.items != NULL);
        CHECK("clear_then_find_miss", sl_find_url(&l, "http://h/0") == -1);
        CHECK("reuse_after_clear", sl_add(&l, "Fresh", "http://f/", "AAC") == 0);
        CHECK("reuse_after_clear_value", strcmp(l.items[0].name, "Fresh") == 0
                                      && strcmp(l.items[0].url, "http://f/") == 0);
    }
    sl_free(&l);
    CHECK("free_resets", l.count == 0 && l.cap == 0 && l.items == NULL);

    /* ---- NULL strings become empty strings ------------------------- */
    sl_init(&l);
    CHECK("add_all_null", sl_add(&l, NULL, NULL, NULL) == 0);
    CHECK("null_name_empty", l.items[0].name != NULL && l.items[0].name[0] == 0);
    CHECK("null_url_empty", l.items[0].url != NULL && l.items[0].url[0] == 0);
    CHECK("null_kind_empty", l.items[0].kind != NULL && l.items[0].kind[0] == 0);
    CHECK("add_null_kind_only", sl_add(&l, "N", "http://n/", NULL) == 1);
    CHECK("null_kind_only_empty", l.items[1].kind[0] == 0 && strcmp(l.items[1].name, "N") == 0);
    CHECK("find_empty_url_hits_null_entry", sl_find_url(&l, "") == 0);
    CHECK("remove_null_entry", sl_remove(&l, 0) == 0 && l.count == 1);
    sl_free(&l);

    /* ---- built-ins -------------------------------------------------- */
    sl_init(&l);
    CHECK("builtins_return_value", sl_add_builtins(&l) == g_builtin_station_count);
    CHECK("builtins_count", l.count == g_builtin_station_count);
    {
        int all_ok = 1;
        for (i = 0; i < g_builtin_station_count; i++) {
            const char *n = g_builtin_stations[i].name;
            const char *u = g_builtin_stations[i].url;
            const char *k = g_builtin_stations[i].kind;
            if (strcmp(l.items[i].name, n ? n : "") != 0) all_ok = 0;
            if (strcmp(l.items[i].url,  u ? u : "") != 0) all_ok = 0;
            if (strcmp(l.items[i].kind, k ? k : "") != 0) all_ok = 0;
            if (l.items[i].is_fav != 0) all_ok = 0;
        }
        CHECK("builtins_fields_copied", all_ok);
    }
    /* Appends, never replaces. */
    CHECK("builtins_append_again", sl_add_builtins(&l) == g_builtin_station_count);
    CHECK("builtins_appended_count", l.count == 2 * g_builtin_station_count);
    sl_free(&l);

    printf("test_station_list: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
