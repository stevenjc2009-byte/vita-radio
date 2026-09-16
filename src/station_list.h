#ifndef VR_STATION_LIST_H
#define VR_STATION_LIST_H

/* A growable station list on the heap. Replaces the fixed 7-entry array as the
 * thing the UI draws, so built-ins, search results and favourites are all the
 * same type. Strings are owned copies. */

typedef struct {
    char *name;
    char *url;
    char *kind;     /* short label shown in the UI: "MP3", "AAC", "HLS" */
    int   is_fav;
} Station;

typedef struct {
    Station *items;
    int      count;
    int      cap;
} StationList;

/* Indices are positions, not handles. sl_remove shifts everything after the
 * removed entry down by one, and sl_clear and sl_free empty the list outright,
 * so any index a caller was holding across one of those three calls refers to a
 * different station afterwards, or to nothing. Nothing detects that: an index
 * past the end is caught, but a stale one that still happens to be in range is
 * indistinguishable from a live one. A caller that keeps a selection across a
 * removal has to re-derive it - sl_find_url is the way back from a URL to a
 * current index - or clamp it to count - 1 itself. Adding never invalidates:
 * sl_add only appends, so existing indices stay put. */

void sl_init(StationList *l);
void sl_free(StationList *l);
void sl_clear(StationList *l);                      /* keeps the allocation */

/* Copies all three strings. Returns the new index, or -1 on allocation failure. */
int  sl_add(StationList *l, const char *name, const char *url, const char *kind);

/* Appends the compiled-in list from stations.h. Returns the number added. */
int  sl_add_builtins(StationList *l);

int  sl_find_url(const StationList *l, const char *url);   /* index, or -1 */

/* Removes one entry and closes the gap. Returns 0, or -1 if index is out of
 * range. Every index above the removed one drops by one - see the note above. */
int  sl_remove(StationList *l, int index);

#endif
