#ifndef VR_UI_H
#define VR_UI_H

#include "player.h"
#include "station_list.h"
#include "updater.h"

void ui_init(void);

/* Draws one full frame (start drawing .. swap buffers).
 * list_label names which list is on screen ("Built-in", "Favourites",
 * "Search: jazz"); notice is a transient line for things like "Searching..."
 * or a search failure, or NULL for none. */
void ui_draw(const StationList *list, const char *list_label, int selected,
             const PlayerStatus *st, const UpdateStatus *upd, const char *notice);

void ui_shutdown(void);

#endif
