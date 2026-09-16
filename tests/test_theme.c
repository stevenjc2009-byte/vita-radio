#include "theme.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0, g_pass = 0;

#define CHECK(name, cond) do { \
    if (cond) { printf("PASS %s\n", name); g_pass++; } \
    else { printf("FAIL %s (%s:%d)\n", name, __FILE__, __LINE__); g_fail++; } \
} while (0)

/* Every colour a theme carries, so the opacity sweep below cannot quietly miss
 * a field that was added to the struct later. */
#define COLOUR_FIELDS 9   /* bg panel bar sel text dim accent ok err */

static unsigned int colour_n(const Theme *t, int n)
{
    switch (n) {
    case 0:  return t->bg;
    case 1:  return t->panel;
    case 2:  return t->bar;
    case 3:  return t->sel;
    case 4:  return t->text;
    case 5:  return t->dim;
    case 6:  return t->accent;
    case 7:  return t->ok;
    default: return t->err;
    }
}

int main(void)
{
    int i, n;

    /* ---- the count the rest of the app indexes against ---------------- */
    CHECK("theme_count_is_4", theme_count() == 4);
    CHECK("theme_count_matches_macro", theme_count() == VR_THEME_COUNT);

    /* ---- theme_at clamps instead of reading out of bounds ------------- */
    /* settings_load already rejects out-of-range values, but theme_at is what
     * stands between a future settings file (or a UI off-by-one) and a read
     * past the array, so it is checked on its own. */
    CHECK("at_negative_one_clamps_low", theme_at(-1) == theme_at(0));
    CHECK("at_large_negative_clamps_low", theme_at(-99999) == theme_at(0));
    CHECK("at_count_clamps_high", theme_at(theme_count()) == theme_at(theme_count() - 1));
    CHECK("at_large_clamps_high", theme_at(99999) == theme_at(theme_count() - 1));
    CHECK("at_never_null", theme_at(0) != NULL && theme_at(-1) != NULL
                        && theme_at(99999) != NULL);

    /* In-range indices must be distinct objects - a clamp written with the
     * comparisons the wrong way round would hand back one theme for every
     * index and still pass the two clamp checks above. */
    {
        int all_distinct = 1;
        for (i = 0; i < theme_count(); i++)
            for (n = i + 1; n < theme_count(); n++)
                if (theme_at(i) == theme_at(n))
                    all_distinct = 0;
        CHECK("in_range_indices_are_distinct", all_distinct);
    }

    /* ---- names ------------------------------------------------------- */
    {
        int names_ok = 1, matches_helper = 1, names_unique = 1;
        for (i = 0; i < theme_count(); i++) {
            const char *nm = theme_at(i)->name;
            if (!nm || nm[0] == '\0')
                names_ok = 0;
            if (theme_name(i) != nm)
                matches_helper = 0;
            for (n = 0; n < i; n++)
                if (nm && theme_at(n)->name && strcmp(nm, theme_at(n)->name) == 0)
                    names_unique = 0;
        }
        CHECK("every_name_non_empty", names_ok);
        CHECK("theme_name_matches_at", matches_helper);
        /* Two themes with the same label are indistinguishable in the System
         * tab, so the user cannot tell which one they picked. */
        CHECK("names_are_unique", names_unique);
    }

    /* ---- every colour is fully opaque -------------------------------- */
    /* vita2d takes RGBA8; a colour whose alpha byte is 0 draws nothing at all,
     * so a mistyped VR_RGBA would show up as an invisible element rather than
     * a wrong one. */
    {
        int all_opaque = 1;
        for (i = 0; i < theme_count(); i++)
            for (n = 0; n < COLOUR_FIELDS; n++)
                if ((colour_n(theme_at(i), n) >> 24) != 0xFFu)
                    all_opaque = 0;
        CHECK("every_colour_fully_opaque", all_opaque);
    }

    /* ---- the three surfaces are distinguishable ---------------------- */
    /* bg, panel and bar are the page, the cards on it and the bars over it.
     * If any two are the same colour that theme renders as a flat void with no
     * visible card edges - a real authoring mistake, not a style choice. */
    {
        int all_distinct = 1;
        for (i = 0; i < theme_count(); i++) {
            const Theme *t = theme_at(i);
            if (t->bg == t->panel || t->panel == t->bar || t->bg == t->bar)
                all_distinct = 0;
        }
        CHECK("bg_panel_bar_all_differ", all_distinct);
    }

    printf("test_theme: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
