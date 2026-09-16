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

void sl_init(StationList *l);
void sl_free(StationList *l);
void sl_clear(StationList *l);                      /* keeps the allocation */

/* Copies all three strings. Returns the new index, or -1 on allocation failure. */
int  sl_add(StationList *l, const char *name, const char *url, const char *kind);

/* Appends the compiled-in list from stations.h. Returns the number added. */
int  sl_add_builtins(StationList *l);

int  sl_find_url(const StationList *l, const char *url);   /* index, or -1 */
int  sl_remove(StationList *l, int index);                 /* 0, or -1 if out of range */

#endif
