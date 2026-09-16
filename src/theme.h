#ifndef VR_THEME_H
#define VR_THEME_H

/* Colour themes for the UI.
 *
 * Every colour the UI draws comes from one of these structs, so switching the
 * theme is a single pointer swap and no drawing code knows a literal colour.
 *
 * Values are pre-packed in vita2d's RGBA8 byte order (r | g<<8 | b<<16 | a<<24)
 * by the VR_RGBA macro in theme.c rather than by vita2d's own macro, so this
 * module is pure C and builds on the host test rig with no Vita headers. */

typedef struct {
    const char  *name;      /* shown in the System tab */
    unsigned int bg;        /* page background, also vita2d's clear colour */
    unsigned int panel;     /* list and detail card fill */
    unsigned int bar;       /* top/bottom bars, idle chip, empty progress track */
    unsigned int sel;       /* selected row fill */
    unsigned int text;      /* primary text */
    unsigned int dim;       /* secondary text, inactive chip label */
    unsigned int accent;    /* brand colour: active chip, tab underline, bars */
    unsigned int ok;        /* playing, up-to-date */
    unsigned int err;       /* errors */
} Theme;

#define VR_THEME_COUNT 4

/* Index is clamped into range, so a settings file carrying a theme number from
 * a future build degrades to a valid theme instead of reading out of bounds. */
const Theme *theme_at(int index);

int         theme_count(void);
const char *theme_name(int index);

#endif
