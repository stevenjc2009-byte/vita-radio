#include "station_list.h"
#include "stations.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define SL_INITIAL_CAP 8

/* A NULL string is stored as "" so nothing downstream has to null-check. */
static char *sl_dup(const char *s)
{
    size_t n;
    char *p;

    if (!s)
        s = "";
    n = strlen(s) + 1;
    p = (char *)malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

static void sl_free_item(Station *s)
{
    free(s->name);
    free(s->url);
    free(s->kind);
    s->name = NULL;
    s->url = NULL;
    s->kind = NULL;
    s->is_fav = 0;
}

/* Geometric growth. Returns 0, or -1 if the list could not be grown - in which
 * case the list is left exactly as it was. */
static int sl_reserve(StationList *l, int need)
{
    int cap;
    Station *p;

    if (need <= l->cap)
        return 0;
    cap = l->cap ? l->cap : SL_INITIAL_CAP;
    while (cap < need) {
        if (cap > INT_MAX / 2)
            return -1;
        cap *= 2;
    }
    if ((size_t)cap > (size_t)-1 / sizeof *p)
        return -1;
    p = (Station *)realloc(l->items, (size_t)cap * sizeof *p);
    if (!p)
        return -1;
    l->items = p;
    l->cap = cap;
    return 0;
}

void sl_init(StationList *l)
{
    if (!l)
        return;
    l->items = NULL;
    l->count = 0;
    l->cap = 0;
}

void sl_clear(StationList *l)
{
    int i;

    if (!l)
        return;
    for (i = 0; i < l->count; i++)
        sl_free_item(&l->items[i]);
    l->count = 0;
}

void sl_free(StationList *l)
{
    if (!l)
        return;
    sl_clear(l);
    free(l->items);
    l->items = NULL;
    l->cap = 0;
}

int sl_add(StationList *l, const char *name, const char *url, const char *kind)
{
    Station st;

    if (!l || l->count == INT_MAX)
        return -1;
    if (sl_reserve(l, l->count + 1) != 0)
        return -1;

    /* Build the entry off to one side: a half-built one must never be visible. */
    st.name = sl_dup(name);
    st.url = sl_dup(url);
    st.kind = sl_dup(kind);
    st.is_fav = 0;
    if (!st.name || !st.url || !st.kind) {
        free(st.name);
        free(st.url);
        free(st.kind);
        return -1;
    }

    l->items[l->count] = st;
    return l->count++;
}

int sl_add_builtins(StationList *l)
{
    int i, added = 0;

    if (!l)
        return 0;
    for (i = 0; i < g_builtin_station_count; i++) {
        if (sl_add(l, g_builtin_stations[i].name,
                      g_builtin_stations[i].url,
                      g_builtin_stations[i].kind) < 0)
            break;
        added++;
    }
    return added;
}

int sl_find_url(const StationList *l, const char *url)
{
    int i;

    if (!l || !url)
        return -1;
    for (i = 0; i < l->count; i++) {
        if (l->items[i].url && strcmp(l->items[i].url, url) == 0)
            return i;
    }
    return -1;
}

int sl_remove(StationList *l, int index)
{
    if (!l || index < 0 || index >= l->count)
        return -1;
    sl_free_item(&l->items[index]);
    if (index < l->count - 1) {
        memmove(&l->items[index], &l->items[index + 1],
                (size_t)(l->count - index - 1) * sizeof *l->items);
    }
    l->count--;
    return 0;
}
