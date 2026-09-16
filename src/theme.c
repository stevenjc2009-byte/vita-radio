#include "theme.h"

/* vita2d's RGBA8 packs r | g<<8 | b<<16 | a<<24. Spelling it out here instead
 * of including <vita2d.h> keeps this translation unit host-buildable, which is
 * what lets test_theme run on the PC. All themes are fully opaque. */
#define VR_RGBA(r, g, b) \
    ((unsigned int)((r) | ((g) << 8) | ((b) << 16) | (0xFFu << 24)))

/* Four themes, ordered as they appear in the System tab. Index 0 is the
 * default for a fresh install and for an unreadable settings file. */
static const Theme s_themes[VR_THEME_COUNT] = {
    {
        "Midnight",
        VR_RGBA(0x0E, 0x0B, 0x14),   /* bg     */
        VR_RGBA(0x16, 0x12, 0x22),   /* panel  */
        VR_RGBA(0x1E, 0x18, 0x30),   /* bar    */
        VR_RGBA(0x2E, 0x24, 0x48),   /* sel    */
        VR_RGBA(0xED, 0xE9, 0xF5),   /* text   */
        VR_RGBA(0x9A, 0x92, 0xB4),   /* dim    */
        VR_RGBA(0xFF, 0x3B, 0x2F),   /* accent */
        VR_RGBA(0x4E, 0xD6, 0x7E),   /* ok     */
        VR_RGBA(0xFF, 0x5A, 0x4E),   /* err    */
    },
    {
        "Deep blue",
        VR_RGBA(0x08, 0x0D, 0x1A),
        VR_RGBA(0x10, 0x1A, 0x33),
        VR_RGBA(0x16, 0x24, 0x3F),
        VR_RGBA(0x1E, 0x34, 0x59),
        VR_RGBA(0xE6, 0xEC, 0xF7),
        VR_RGBA(0x8A, 0x9B, 0xBA),
        VR_RGBA(0x3D, 0x8B, 0xFF),
        VR_RGBA(0x3E, 0xD9, 0xA4),
        VR_RGBA(0xFF, 0x5F, 0x56),
    },
    {
        /* For OLED-style contrast: a true 0,0,0 ground, not a near-black. */
        "True black",
        VR_RGBA(0x00, 0x00, 0x00),
        VR_RGBA(0x0D, 0x0D, 0x0D),
        VR_RGBA(0x14, 0x14, 0x14),
        VR_RGBA(0x23, 0x23, 0x23),
        VR_RGBA(0xF2, 0xF2, 0xF2),
        VR_RGBA(0x8C, 0x8C, 0x8C),
        VR_RGBA(0xFF, 0x3B, 0x2F),
        VR_RGBA(0x4E, 0xD6, 0x7E),
        VR_RGBA(0xFF, 0x5A, 0x4E),
    },
    {
        "Graphite",
        VR_RGBA(0x12, 0x12, 0x14),
        VR_RGBA(0x1C, 0x1C, 0x20),
        VR_RGBA(0x24, 0x24, 0x2A),
        VR_RGBA(0x32, 0x32, 0x3A),
        VR_RGBA(0xEC, 0xEC, 0xEF),
        VR_RGBA(0x97, 0x97, 0x9F),
        VR_RGBA(0xFF, 0x8A, 0x3D),
        VR_RGBA(0x5C, 0xCB, 0x8A),
        VR_RGBA(0xFF, 0x5F, 0x56),
    },
};

const Theme *theme_at(int index)
{
    if (index < 0) index = 0;
    if (index >= VR_THEME_COUNT) index = VR_THEME_COUNT - 1;
    return &s_themes[index];
}

int theme_count(void)
{
    return VR_THEME_COUNT;
}

const char *theme_name(int index)
{
    return theme_at(index)->name;
}
