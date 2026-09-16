#ifndef VR_UI_H
#define VR_UI_H

#include "player.h"
#include "station_list.h"
#include "theme.h"
#include "updater.h"

/* The four top-level tabs, in the order they are drawn, and the order the
 * L/R triggers cycle through. */
typedef enum {
    TAB_STATIONS = 0,
    TAB_FAVOURITES,
    TAB_SEARCH,
    TAB_SYSTEM,
    TAB_COUNT
} UiTab;

/* Rows of the System tab, top to bottom. */
typedef enum {
    SYS_UPDATE = 0,     /* check for / install an update */
    SYS_THEME,          /* cycle the colour theme */
    SYS_ABOUT,          /* version and credits, not activatable */
    SYS_ROW_COUNT
} SysRow;

/* Everything one frame needs. Passed as a struct rather than a dozen
 * arguments so adding a field later does not touch every call site. */
typedef struct {
    UiTab tab;

    /* The station list for the current tab, and what to call it. NULL on the
     * System tab, which draws settings rows instead of stations. */
    const StationList *list;
    const char        *list_label;
    int                selected;

    /* Filter chips along the top of the Stations tab. chips points at
     * chip_count NUL-terminated labels. chip_active is the filter whose
     * results are currently on screen (-1 if none); chip_focus is the one the
     * d-pad is sitting on. Ignored on every other tab. */
    const char *const *chips;
    int                chip_count;
    int                chip_active;
    int                chip_focus;

    /* System tab state. */
    int sys_row;        /* a SysRow */
    int theme_index;    /* index into theme_at() */

    const PlayerStatus *player;
    const UpdateStatus *upd;

    /* Transient line in the header: "Searching...", an error, a confirmation.
     * NULL or empty for none. Outranks the updater's own message. */
    const char *notice;

    /* Never NULL. Every colour drawn this frame comes from here. */
    const Theme *theme;
} UiFrame;

/* t sets the initial clear colour; must not be NULL. */
void ui_init(const Theme *t);

/* Re-points the clear colour when the user picks a different theme. Drawing
 * colours come from UiFrame.theme each frame, so this only exists because the
 * clear colour is vita2d state rather than a per-frame argument. */
void ui_set_theme(const Theme *t);

/* Draws one full frame (start drawing .. swap buffers). Swaps at vsync. */
void ui_draw(const UiFrame *f);

void ui_shutdown(void);

#endif
