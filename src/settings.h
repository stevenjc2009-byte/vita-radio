#ifndef VR_SETTINGS_H
#define VR_SETTINGS_H

/* User settings persisted to the memory card.
 *
 * Same shape and the same reasoning as favourites.tsv: one "key<TAB>value" per
 * line, saved via a .tmp and a rename, and a truncated final line from a
 * power-off is dropped while every complete line before it still loads. An
 * unknown key is skipped rather than rejected, so a settings file written by a
 * newer build still loads what this one understands.
 *
 * The file living beside favourites.tsv (not inside it) is deliberate: the
 * favourites format is already on users' memory cards and adding rows to it
 * would break older builds reading the same file. */

#define VR_SETTINGS_FILE "ux0:data/VitaRadio/settings.tsv"

typedef struct {
    int theme;      /* index into theme_at(); clamped on load */
} Settings;

/* Fills s with the defaults. Always succeeds. */
void settings_defaults(Settings *s);

/* Loads path over the defaults. A missing file is not an error: s is left at
 * the defaults and 0 is returned. Returns -1 only on a real read error.
 * Values that do not parse, or fall outside the valid range, are ignored and
 * leave that field at its default -- a corrupt line must never be able to put
 * the UI into a state with no valid theme. */
int settings_load(Settings *s, const char *path);

/* Writes s to path atomically. Returns 0, or -1 on failure. */
int settings_save(const Settings *s, const char *path);

/* path is a parameter so the host tests can use a temp file; the app passes
 * VR_SETTINGS_FILE. */

#endif
