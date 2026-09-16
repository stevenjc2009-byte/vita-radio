#ifndef VR_FAVOURITES_H
#define VR_FAVOURITES_H

#include "station_list.h"

/* Favourites persisted to the memory card.
 *
 * Format is one station per line, tab-separated name/url/kind, because it has
 * to survive a half-written file after a power-off: a truncated last line is
 * dropped and the rest still load. Saving writes a .tmp beside it and renames. */

#define VR_FAV_DIR  "ux0:data/VitaRadio"
#define VR_FAV_FILE "ux0:data/VitaRadio/favourites.tsv"

/* Creates the directory if needed. Returns 0, -1 on failure. */
int fav_ensure_dir(void);

/* Appends the saved favourites to l (with is_fav set). A missing file is not an
 * error - it returns 0 and adds nothing. Returns -1 only on a real read error. */
int fav_load(StationList *l, const char *path);

/* Writes every entry of l whose is_fav is set. Returns 0, -1 on failure. */
int fav_save(const StationList *l, const char *path);

/* path is a parameter so the host tests can use a temp file; the app passes
 * VR_FAV_FILE. */

#endif
