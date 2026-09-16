#include "ui.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vita2d.h>

#include "version.h"

/* ---------------------------------------------------------------------------
 * Layout.
 *
 * The screen is 960x544 and every band below is a half-open [start, start+size)
 * range, laid out top to bottom with no overlap:
 *
 *     0 ..  40   header bar        (title, version, notice / updater line)
 *    40 ..  80   tab strip         (Stations Favourites Search System)
 *    86 .. 116   filter chip row   (Stations tab only)
 *   122 .. 412   body, chips tab   (list panel + detail panel)
 *    88 .. 412   body, other tabs
 *   422 .. 506   now-playing card
 *   514 .. 544   button hints
 *
 * Horizontally: the list panel is 16..616, the detail panel 628..944, the card
 * 16..944 and the chip viewport 40..920 with the scroll arrows in the margins.
 * Nothing is drawn outside those numbers - see the per-rectangle notes.
 *
 * No colour is spelled out in this file. Every fill and every glyph takes its
 * colour from the Theme handed in with the frame, so switching theme repaints
 * the whole UI without any drawing code knowing what changed.
 * --------------------------------------------------------------------------- */

#define SCREEN_W 960
#define SCREEN_H 544

#define HDR_H     40                /* 0 .. 40 */

#define TAB_Y     40
#define TAB_H     40                /* 40 .. 80 */
#define TAB_X0    16
#define TAB_W     166
#define TAB_ADV   176               /* last tab: 16 + 3*176 + 166 = 710 */

#define CHIP_Y       86
#define CHIP_H       30             /* 86 .. 116 */
#define CHIP_X0      40
#define CHIP_VIEW_W  880            /* 40 .. 920 */
#define CHIP_GAP     8
#define CHIP_PAD     14
#define CHIP_MIN_W   56
#define CHIP_SCALE   0.78f
#define CHIP_MAX     64             /* chips past this are not drawn */

#define BODY_Y_CHIPS 122
#define BODY_Y_PLAIN 88
#define BODY_BOT     412

#define LEFT_X    16
#define LEFT_W    600               /* 16 .. 616 */
#define RIGHT_X   628
#define RIGHT_W   316               /* 628 .. 944 */

#define HEAD_H    26                /* panel header band */
#define ROW_H     44
#define SYS_ROW_H 62

#define CARD_X    16
#define CARD_Y    422
#define CARD_W    928               /* 16 .. 944 */
#define CARD_H    84                /* 422 .. 506 */

#define HINT_Y    514
#define HINT_H    30                /* 514 .. 544 */

static vita2d_pgf *s_font;

/* Frame counter, used only to animate the indeterminate download bar. */
static unsigned s_tick;

/* Backs len up to the start of the UTF-8 sequence it lands in. */
static size_t lead_back(const char *s, size_t len)
{
    while (len > 0 && ((unsigned char)s[len] & 0xC0) == 0x80) len--;
    return len;
}

/* Start of the character after the one beginning at len, or total if none. */
static size_t lead_next(const char *s, size_t total, size_t len)
{
    if (len >= total) return total;
    len++;
    while (len < total && ((unsigned char)s[len] & 0xC0) == 0x80) len++;
    return len;
}

/* Measures dst as it would read cut at len with "..." spliced on, then puts the
 * overwritten bytes back. Callers guarantee len + 4 fits the buffer.
 * Stays in float: rounding a width down to int accepts strings a glyph too wide. */
static float width_cut(char *dst, size_t total, size_t len, float scale)
{
    char save[4];
    size_t n = total + 1 - len;     /* only what is live, through the terminator */
    float w;

    if (n > 4) n = 4;
    memcpy(save, dst + len, n);
    memcpy(dst + len, "...", 4);
    w = vita2d_pgf_text_width(s_font, scale, dst);
    memcpy(dst + len, save, n);
    return w;
}

/* Copies src into dst, shortened with "..." so it renders no wider than max_w.
 *
 * The answer is the longest UTF-8 character boundary whose prefix plus "..."
 * still fits. Measuring every boundary from the end down is O(n) width calls of
 * O(n) glyph lookups each; scaling the full width down to max_w lands within a
 * glyph or two of the answer, so the walk below is typically three measurements
 * whatever the length. Never cuts inside a UTF-8 sequence. */
static const char *fit(char *dst, size_t dst_sz, const char *src, float scale, int max_w)
{
    size_t total, hi, len;
    float w;

    if (!src) src = "";
    snprintf(dst, dst_sz, "%s", src);
    if (!s_font) return dst;
    w = vita2d_pgf_text_width(s_font, scale, dst);
    if (w <= max_w) return dst;

    /* The cut needs room for "..." plus the terminator, and the whole string is
     * never a candidate - it is the thing that just failed to fit. */
    total = strlen(dst);
    if (total == 0 || dst_sz < 4) {
        snprintf(dst, dst_sz, "...");
        return dst;
    }
    hi = total - 1;
    if (hi > dst_sz - 4) hi = dst_sz - 4;
    hi = lead_back(dst, hi);

    /* max_w <= 0 would make the guess negative, which is not a size_t. */
    len = (max_w > 0 && w > 0.0f) ? (size_t)((double)total * max_w / w) : 0;
    if (len > hi) len = hi;
    len = lead_back(dst, len);

    if (width_cut(dst, total, len, scale) <= max_w) {
        /* Guess was short: grow while the next glyph still leaves room. */
        for (;;) {
            size_t next = lead_next(dst, total, len);
            if (next > hi || width_cut(dst, total, next, scale) > max_w) break;
            len = next;
        }
    } else {
        /* Guess was long: shrink to the first boundary that fits. len reaching
         * 0 leaves "..." on its own, which is what the caller gets. */
        while (len > 0) {
            len = lead_back(dst, len - 1);
            if (width_cut(dst, total, len, scale) <= max_w) break;
        }
    }

    memcpy(dst + len, "...", 4);
    return dst;
}

/* --- fitted-text cache ------------------------------------------------------
 * ui_draw re-fits the same strings every frame - the title, the version, the
 * footer hint and each visible row's name and kind - and fit() is the most
 * expensive thing in the frame. Slots are indexed by the source pointer and
 * validated against a copy of the source, so a string that has not changed
 * costs one memcmp instead of a glyph walk, and a reused allocation cannot
 * serve stale text. */
#define FIT_SLOTS 32
#define FIT_MAX   600

typedef struct {
    const char *src;
    float       scale;
    int         max_w;
    size_t      len;            /* strlen of the source when it was fitted */
    int         used;
    char        key[FIT_MAX];   /* the source, as fit() saw it */
    char        out[FIT_MAX];   /* the fitted result */
} FitSlot;

static FitSlot s_fit[FIT_SLOTS];

static const char *fit_cached(const char *src, float scale, int max_w)
{
    FitSlot *e;
    size_t n, cmp;

    if (!src) src = "";
    e = &s_fit[((uintptr_t)src >> 3) % FIT_SLOTS];
    n = strlen(src);
    cmp = n + 1 < FIT_MAX ? n + 1 : FIT_MAX - 1;

    if (e->used && e->src == src && e->scale == scale && e->max_w == max_w &&
        e->len == n && memcmp(e->key, src, cmp) == 0)
        return e->out;

    fit(e->out, sizeof(e->out), src, scale, max_w);
    snprintf(e->key, sizeof(e->key), "%s", src);
    e->src = src;
    e->scale = scale;
    e->max_w = max_w;
    e->len = n;
    e->used = 1;
    return e->out;
}

static void text(int x, int y, unsigned int col, float scale, int max_w, const char *s)
{
    char buf[600];
    vita2d_pgf_draw_text(s_font, x, y, col, scale, fit(buf, sizeof(buf), s, scale, max_w));
}

/* Same as text(), for strings that hold still from frame to frame. */
static void text_cached(int x, int y, unsigned int col, float scale, int max_w, const char *s)
{
    vita2d_pgf_draw_text(s_font, x, y, col, scale, fit_cached(s, scale, max_w));
}

static int text_w(const char *s, float scale)
{
    return s_font ? vita2d_pgf_text_width(s_font, scale, s) : 0;
}

/* Right edge of the drawn glyphs lands on right_x. fit() bounds the width at
 * max_w, so the leftmost pixel is never further left than right_x - max_w. */
static void text_right(int right_x, int y, unsigned int col, float scale, int max_w, const char *s)
{
    char buf[600];
    const char *p = fit(buf, sizeof(buf), s, scale, max_w);
    vita2d_pgf_draw_text(s_font, right_x - text_w(p, scale), y, col, scale, p);
}

/* Centred on cx; bounded to cx +/- max_w/2 for the same reason as above. */
static void text_center(int cx, int y, unsigned int col, float scale, int max_w, const char *s)
{
    char buf[600];
    const char *p = fit(buf, sizeof(buf), s, scale, max_w);
    vita2d_pgf_draw_text(s_font, cx - text_w(p, scale) / 2, y, col, scale, p);
}

/* A th-pixel border drawn just inside (x, y, w, h) - four rectangles, so it
 * cannot extend past the box it outlines. */
static void outline(int x, int y, int w, int h, int th, unsigned int col)
{
    if (w <= 2 * th || h <= 2 * th) return;
    vita2d_draw_rectangle(x, y, w, th, col);
    vita2d_draw_rectangle(x, y + h - th, w, th, col);
    vita2d_draw_rectangle(x, y + th, th, h - 2 * th, col);
    vita2d_draw_rectangle(x + w - th, y + th, th, h - 2 * th, col);
}

/* Panel with a header band across its top. Returns the y of the first row. */
static int panel(int x, int y, int w, int h, const Theme *t, const char *title,
                 const char *right_note)
{
    vita2d_draw_rectangle(x, y, w, h, t->panel);
    vita2d_draw_rectangle(x, y, w, HEAD_H, t->bar);
    vita2d_draw_rectangle(x, y + HEAD_H - 2, w, 2, t->accent);
    text(x + 14, y + 18, t->accent, 0.78f, w - 150, title);
    if (right_note) text_right(x + w - 14, y + 18, t->dim, 0.72f, 120, right_note);
    return y + HEAD_H;
}

static const char *state_word(PlayerState st)
{
    switch (st) {
    case PLAYER_IDLE:       return "Idle";
    case PLAYER_CONNECTING: return "Connecting";
    case PLAYER_BUFFERING:  return "Buffering";
    case PLAYER_PLAYING:    return "Playing";
    case PLAYER_ERROR:      return "Error";
    }
    return "?";
}

static unsigned int state_color(const Theme *t, PlayerState st)
{
    switch (st) {
    case PLAYER_PLAYING: return t->ok;
    case PLAYER_ERROR:   return t->err;
    case PLAYER_IDLE:    return t->dim;
    default:             return t->accent;
    }
}

static const char *update_word(UpdateState st)
{
    switch (st) {
    case UPD_IDLE:        return "Not checked";
    case UPD_CHECKING:    return "Checking...";
    case UPD_UP_TO_DATE:  return "Up to date";
    case UPD_AVAILABLE:   return "Update available";
    case UPD_DOWNLOADING: return "Downloading";
    case UPD_INSTALLING:  return "Installing";
    case UPD_READY:       return "Ready - restarting";
    case UPD_ERROR:       return "Failed";
    }
    return "?";
}

static unsigned int update_color(const Theme *t, UpdateState st)
{
    switch (st) {
    case UPD_AVAILABLE:
    case UPD_UP_TO_DATE:
    case UPD_READY:   return t->ok;
    case UPD_ERROR:   return t->err;
    case UPD_IDLE:    return t->dim;
    default:          return t->accent;
    }
}

void ui_init(const Theme *t)
{
    if (!t) t = theme_at(0);
    vita2d_init();
    vita2d_set_clear_color(t->bg);
    s_font = vita2d_load_default_pgf();
}

void ui_set_theme(const Theme *t)
{
    if (!t) t = theme_at(0);
    vita2d_set_clear_color(t->bg);
}

/* --- header + tabs --------------------------------------------------------- */

static const char *const TAB_NAMES[TAB_COUNT] = {
    "Stations", "Favourites", "Search", "System"
};

static void draw_header(const UiFrame *f, const Theme *t)
{
    /* (0,0,960,40) - the full width of the screen, by definition in bounds. */
    vita2d_draw_rectangle(0, 0, SCREEN_W, HDR_H, t->bar);
    vita2d_draw_rectangle(0, HDR_H - 2, SCREEN_W, 2, t->accent);

    /* The version starts at x=152, so the title has 136px of room less the
     * 16px inset and a gap - any wider and 1.15f scale runs under it. */
    text_cached(16, 27, t->accent, 1.15f, 122, "Vita Radio");
    text_cached(150, 27, t->dim, 0.72f, 70, "v" VR_VERSION);

    /* A transient notice (searching, search failed) outranks the updater
     * message here - it is the thing the user just asked for.
     * x=236 .. 236+708 = 944. */
    if (f->notice && f->notice[0])
        text(236, 26, t->accent, 0.82f, SCREEN_W - 252, f->notice);
    else if (f->upd && f->upd->message[0])
        text(236, 26, update_color(t, f->upd->state), 0.82f, SCREEN_W - 252,
             f->upd->message);
}

static void draw_tabs(const UiFrame *f, const Theme *t)
{
    int i;

    /* Hairline under the whole strip, so the active tab's accent underline
     * reads as a break in a continuous rule rather than a floating dash. */
    vita2d_draw_rectangle(0, TAB_Y + TAB_H - 2, SCREEN_W, 2, t->bar);

    for (i = 0; i < TAB_COUNT; i++) {
        /* x = 16 + i*176, width 166: the last tab ends at 710 < 960. */
        int x = TAB_X0 + i * TAB_ADV;
        int on = (i == (int)f->tab);

        if (on) {
            vita2d_draw_rectangle(x, TAB_Y + 4, TAB_W, TAB_H - 8, t->bar);
            vita2d_draw_rectangle(x, TAB_Y + TAB_H - 4, TAB_W, 4, t->accent);
        }
        text_center(x + TAB_W / 2, TAB_Y + 26, on ? t->text : t->dim,
                    on ? 0.95f : 0.88f, TAB_W - 16, TAB_NAMES[i]);
    }
}

/* --- filter chips ---------------------------------------------------------- */

/* Drawn width of one chip, padding included. Clamped to the viewport so a
 * pathologically long label cannot produce a box wider than the window it has
 * to be scrolled inside - chip_scroll relies on that to guarantee visibility. */
static int chip_box_w(const char *label)
{
    int w = text_w(label ? label : "", CHIP_SCALE) + 2 * CHIP_PAD;
    if (w < CHIP_MIN_W) w = CHIP_MIN_W;
    if (w > CHIP_VIEW_W - CHIP_GAP) w = CHIP_VIEW_W - CHIP_GAP;
    return w;
}

/* Horizontal scroll offset for the chip row.
 *
 * w[i] is chip i's advance (box + gap); the row is laid out at running offsets
 * from 0. The result is the smallest shift that puts the focused chip's whole
 * advance inside a view_w-wide window, then clamped so the row never scrolls
 * past either end. Returns 0 for an empty row or an out-of-range focus, which
 * is also what an unfocused row wants.
 *
 * Verified on the host by scratchpad/vr-ui-chipscroll.c over counts 0/1/5/40
 * and both focus extremes: the returned offset never puts a chip's left edge
 * left of the viewport or its right edge past it. */
static int chip_scroll(const int *w, int n, int focus, int view_w)
{
    int total = 0, cx = 0, i, scroll = 0;

    for (i = 0; i < n; i++) {
        if (i == focus) cx = total;
        total += w[i];
    }
    if (focus >= 0 && focus < n) {
        if (cx + w[focus] > view_w) scroll = cx + w[focus] - view_w;
        if (cx < scroll) scroll = cx;
    }
    if (scroll > total - view_w) scroll = total - view_w;
    if (scroll < 0) scroll = 0;
    return scroll;
}

static void draw_chips(const UiFrame *f, const Theme *t)
{
    int widths[CHIP_MAX];
    int n = f->chip_count;
    int i, pos, scroll, total = 0;

    if (!f->chips || n <= 0) return;
    if (n > CHIP_MAX) n = CHIP_MAX;

    for (i = 0; i < n; i++) {
        widths[i] = chip_box_w(f->chips[i]) + CHIP_GAP;
        total += widths[i];
    }
    scroll = chip_scroll(widths, n, f->chip_focus, CHIP_VIEW_W);

    pos = 0;
    for (i = 0; i < n; i++) {
        int bw = widths[i] - CHIP_GAP;
        int x  = CHIP_X0 + pos - scroll;
        pos += widths[i];

        /* Partly-scrolled chips are skipped rather than clipped: vita2d's clip
         * state is global and easy to leave set, and the focused chip is
         * guaranteed whole by chip_scroll, so nothing the user is aiming at
         * disappears. The arrows below say there is more either side. */
        if (x < CHIP_X0 || x + bw > CHIP_X0 + CHIP_VIEW_W) continue;

        if (i == f->chip_active) {
            vita2d_draw_rectangle(x, CHIP_Y, bw, CHIP_H, t->accent);
            if (i == f->chip_focus) outline(x, CHIP_Y, bw, CHIP_H, 2, t->text);
            text_center(x + bw / 2, CHIP_Y + 21, t->bg, CHIP_SCALE, bw - 8,
                        f->chips[i]);
        } else {
            vita2d_draw_rectangle(x, CHIP_Y, bw, CHIP_H, t->bar);
            if (i == f->chip_focus) {
                outline(x, CHIP_Y, bw, CHIP_H, 2, t->accent);
                text_center(x + bw / 2, CHIP_Y + 21, t->text, CHIP_SCALE,
                            bw - 8, f->chips[i]);
            } else {
                text_center(x + bw / 2, CHIP_Y + 21, t->dim, CHIP_SCALE,
                            bw - 8, f->chips[i]);
            }
        }
    }

    /* Arrows live in the 24px margins either side of the viewport: the left one
     * spans 20..36, the right 926..942. */
    if (scroll > 0)
        text_cached(CHIP_X0 - 20, CHIP_Y + 21, t->dim, 0.8f, 16, "<");
    if (total - scroll > CHIP_VIEW_W)
        text_cached(CHIP_X0 + CHIP_VIEW_W + 6, CHIP_Y + 21, t->dim, 0.8f, 16, ">");
}

/* --- station list ---------------------------------------------------------- */

static void empty_state(UiTab tab, const char **line1, const char **line2)
{
    switch (tab) {
    case TAB_FAVOURITES:
        *line1 = "No favourites yet.";
        *line2 = "Press TRIANGLE on any station to keep it here.";
        return;
    case TAB_SEARCH:
        *line1 = "Nothing searched yet.";
        *line2 = "Press SQUARE to look up a station by name.";
        return;
    case TAB_STATIONS:
        *line1 = "No stations in this filter.";
        *line2 = "Press Left or Right to pick another filter.";
        return;
    case TAB_SYSTEM:
    case TAB_COUNT:
        break;
    }
    *line1 = "Nothing here.";
    *line2 = "";
}

static void draw_list(const UiFrame *f, const Theme *t, int body_y, int body_h)
{
    const StationList *list = f->list;
    int count = list && list->items ? list->count : 0;
    int tab = (int)f->tab;
    int rows_y, rows_h, visible, first, i;
    char note[48];

    if (count < 0) count = 0;
    if (tab < 0 || tab >= TAB_COUNT) tab = TAB_STATIONS;
    snprintf(note, sizeof(note), "%d", count);

    rows_y = panel(LEFT_X, body_y, LEFT_W, body_h, t,
                   f->list_label ? f->list_label : TAB_NAMES[tab], note);
    rows_h = body_h - HEAD_H;

    if (count == 0) {
        const char *l1, *l2;
        empty_state(f->tab, &l1, &l2);
        text(LEFT_X + 20, rows_y + 44, t->text, 0.9f, LEFT_W - 40, l1);
        text(LEFT_X + 20, rows_y + 72, t->dim, 0.75f, LEFT_W - 40, l2);
        return;
    }

    visible = rows_h / ROW_H;
    if (visible < 1) visible = 1;

    first = 0;
    if (f->selected >= visible) first = f->selected - visible + 1;
    /* The list can shrink under a scrolled view (removing the last favourite),
     * which would leave a blank row and no highlight for a frame. */
    if (first > count - visible) first = count - visible;
    if (first < 0) first = 0;

    for (i = first; i < count && i < first + visible; i++) {
        const Station *s = &list->items[i];
        /* y is at most rows_y + (visible-1)*ROW_H, and visible*ROW_H <= rows_h
         * by construction, so the last row ends on or before rows_y + rows_h. */
        int y = rows_y + (i - first) * ROW_H;
        int live = f->player && f->player->state != PLAYER_IDLE && s->url &&
                   strcmp(f->player->url, s->url) == 0;

        if (i == f->selected) {
            vita2d_draw_rectangle(LEFT_X, y, LEFT_W, ROW_H, t->sel);
            vita2d_draw_rectangle(LEFT_X, y, 4, ROW_H, t->accent);
        }
        if (live) {
            vita2d_draw_rectangle(LEFT_X, y + 6, 4, ROW_H - 12,
                                  state_color(t, f->player->state));
            /* right edge 572, clear of the fav marker at 596 */
            text_right(LEFT_X + LEFT_W - 44, y + 26,
                       state_color(t, f->player->state), 0.9f, 20, ">");
        }
        if (s->is_fav)
            text_right(LEFT_X + LEFT_W - 20, y + 26, t->accent, 0.9f, 20, "*");

        /* 34 .. 34+518 = 552, clear of both markers and the scrollbar at 610. */
        text_cached(LEFT_X + 18, y + 19, t->text, 0.92f, LEFT_W - 82,
                    s->name ? s->name : "");
        text_cached(LEFT_X + 18, y + 37, t->dim, 0.7f, LEFT_W - 82,
                    s->kind ? s->kind : "");
    }

    if (count > visible) {
        /* Scrollbar in the panel's right gutter: 610 .. 614. */
        int track_x = LEFT_X + LEFT_W - 6;
        int th = rows_h * visible / count;
        int ty;
        if (th < 24) th = 24;
        if (th > rows_h) th = rows_h;
        ty = rows_y + (rows_h - th) * first / (count - visible);
        vita2d_draw_rectangle(track_x, rows_y, 4, rows_h, t->bar);
        vita2d_draw_rectangle(track_x, ty, 4, th, t->accent);
    }
}

/* --- detail panel (right of the list) -------------------------------------- */

/* Draws s only while its baseline is still inside the panel. One guard in front
 * of every line is what lets this panel's content grow later without anyone
 * re-deriving where the last line lands. */
static void dtext(int y, int bottom, int x, unsigned int col, float scale,
                  int max_w, const char *s)
{
    if (y <= bottom) text(x, y, col, scale, max_w, s);
}

static void draw_detail(const UiFrame *f, const Theme *t, int body_y, int body_h)
{
    const Station *s = NULL;
    const PlayerStatus *p = f->player;
    int x = RIGHT_X + 14;           /* 642 */
    int w = RIGHT_W - 28;           /* 288 -> right edge 930 */
    int bottom = body_y + body_h - 8;
    int y;
    char buf[600];

    y = panel(RIGHT_X, body_y, RIGHT_W, body_h, t, "Details", NULL) + 26;

    if (f->list && f->list->items && f->selected >= 0 &&
        f->selected < f->list->count)
        s = &f->list->items[f->selected];

    if (!s) {
        dtext(y, bottom, x, t->dim, 0.75f, w, "Nothing selected.");
        return;
    }

    dtext(y, bottom, x, t->text, 0.95f, w, s->name ? s->name : "(unnamed)");
    y += 26;
    dtext(y, bottom, x, t->dim, 0.72f, w,
          s->kind && s->kind[0] ? s->kind : "Unknown format");
    y += 22;
    dtext(y, bottom, x, s->is_fav ? t->accent : t->dim, 0.72f, w,
          s->is_fav ? "* Favourite" : "Not a favourite");
    y += 30;

    dtext(y, bottom, x, t->dim, 0.62f, w, "STREAM URL");
    y += 18;
    dtext(y, bottom, x, t->dim, 0.62f, w, s->url ? s->url : "-");
    y += 30;

    if (y <= bottom) vita2d_draw_rectangle(x, y - 12, w, 1, t->bar);

    if (!p || !s->url || strcmp(p->url, s->url) != 0) {
        dtext(y, bottom, x, t->dim, 0.7f, w, "Not the current stream.");
        return;
    }

    dtext(y, bottom, x, state_color(t, p->state), 0.85f, w, state_word(p->state));
    y += 24;

    snprintf(buf, sizeof(buf), "Codec  %s", p->codec[0] ? p->codec : "-");
    dtext(y, bottom, x, t->text, 0.7f, w, buf);
    y += 20;

    if (p->in_rate > 0)
        snprintf(buf, sizeof(buf), "Audio  %d Hz  %d ch", p->in_rate, p->in_channels);
    else
        snprintf(buf, sizeof(buf), "Audio  -");
    dtext(y, bottom, x, t->text, 0.7f, w, buf);
    y += 20;

    if (p->http_status > 0) snprintf(buf, sizeof(buf), "HTTP   %ld", p->http_status);
    else                    snprintf(buf, sizeof(buf), "HTTP   -");
    dtext(y, bottom, x, t->text, 0.7f, w, buf);
    y += 20;

    snprintf(buf, sizeof(buf), "Got    %lu KB", p->bytes_received / 1024UL);
    dtext(y, bottom, x, t->text, 0.7f, w, buf);
}

/* --- now-playing card ------------------------------------------------------ */

static void draw_card(const UiFrame *f, const Theme *t)
{
    const PlayerStatus *p = f->player;
    PlayerState st = p ? p->state : PLAYER_IDLE;
    unsigned int sc = state_color(t, st);
    unsigned pct;
    char buf[600];

    /* (16,422,928,84) -> right edge 944, bottom 506. */
    vita2d_draw_rectangle(CARD_X, CARD_Y, CARD_W, CARD_H, t->panel);
    vita2d_draw_rectangle(CARD_X, CARD_Y, 4, CARD_H, sc);

    /* Left block: 36 .. 496. */
    text_cached(36, 442, t->dim, 0.62f, 200, "NOW PLAYING");
    if (!p || st == PLAYER_IDLE) {
        text_cached(36, 470, t->dim, 1.0f, 460, "Nothing playing");
        text_cached(36, 492, t->dim, 0.7f, 460, "Press X on a station to start it.");
        text_right(928, 470, t->dim, 0.85f, 200, "Idle");
        return;
    }

    text(36, 470, t->text, 1.0f, 460, p->title[0] ? p->title : "Live stream");
    text(36, 492, t->dim, 0.7f, 460, p->url[0] ? p->url : "-");

    if (st == PLAYER_ERROR) {
        /* Error takes the whole right half: 516 .. 930. */
        text_cached(516, 452, t->err, 0.9f, 414, "Playback failed");
        text(516, 476, t->err, 0.75f, 414, p->error[0] ? p->error : "(no detail)");
        return;
    }

    /* Middle block: state word and buffer meter, 516 .. 746. */
    text_cached(516, 446, sc, 1.0f, 230, state_word(st));
    pct = p->buffer_pct > 100 ? 100 : p->buffer_pct;
    vita2d_draw_rectangle(516, 458, 230, 10, t->bar);
    vita2d_draw_rectangle(516, 458, 230.0f * (float)pct / 100.0f, 10, t->accent);
    snprintf(buf, sizeof(buf), "Buffer %u%%", pct);
    text(516, 492, t->dim, 0.68f, 230, buf);

    /* Right block: 770 .. 928. */
    snprintf(buf, sizeof(buf), "%s", p->codec[0] ? p->codec : "-");
    text(770, 446, t->text, 0.7f, 158, buf);
    if (p->in_rate > 0) snprintf(buf, sizeof(buf), "%d Hz  %d ch", p->in_rate, p->in_channels);
    else                snprintf(buf, sizeof(buf), "-");
    text(770, 468, t->dim, 0.7f, 158, buf);
    snprintf(buf, sizeof(buf), "%lu KB", p->bytes_received / 1024UL);
    text(770, 490, t->dim, 0.7f, 158, buf);
}

/* --- system tab ------------------------------------------------------------ */

/* Determinate when pct is 1..100. pct == 0 means the server sent no
 * Content-Length (see UpdateStatus.progress_pct), so there is no percentage to
 * draw - a block ping-pongs across the track instead, which says "working" and
 * does not claim 0%. Both forms are bounded by the track: the block's travel is
 * span = w - blk and the folded phase stays in [0, span]. */
static void progress_bar(int x, int y, int w, int h, const Theme *t, unsigned pct)
{
    vita2d_draw_rectangle(x, y, w, h, t->bar);
    if (w <= 0) return;

    if (pct > 0) {
        if (pct > 100) pct = 100;
        vita2d_draw_rectangle(x, y, (float)w * (float)pct / 100.0f, h, t->accent);
        return;
    }

    {
        int blk = w / 4;
        int span, phase;
        if (blk < 16) blk = 16;
        if (blk > w) blk = w;
        span = w - blk;
        if (span <= 0) {
            vita2d_draw_rectangle(x, y, w, h, t->accent);
            return;
        }
        phase = (int)((s_tick / 2) % (unsigned)(2 * span));
        if (phase > span) phase = 2 * span - phase;
        vita2d_draw_rectangle(x + phase, y, blk, h, t->accent);
    }
}

static void sys_help(int row, const char **l1, const char **l2, const char **l3)
{
    switch (row) {
    case SYS_THEME:
        *l1 = "Four palettes, applied instantly.";
        *l2 = "Left and Right change the colours.";
        *l3 = "The choice is saved with your settings.";
        return;
    case SYS_ABOUT:
        *l1 = "Vita Radio streams internet radio";
        *l2 = "straight to your Vita over Wi-Fi.";
        *l3 = "Built with the VitaSDK and vita2d.";
        return;
    case SYS_UPDATE:
    default:
        *l1 = "Press X to check GitHub for a newer";
        *l2 = "release, then X again to install it.";
        *l3 = "The Vita restarts to finish the install.";
        return;
    }
}

static void draw_system(const UiFrame *f, const Theme *t, int body_y, int body_h)
{
    const char *l1, *l2, *l3;
    int rows_y, i, x = LEFT_X + 18;     /* 34 */
    int inner_w = LEFT_W - 36;          /* 564 -> right edge 598 */
    char buf[600];

    rows_y = panel(LEFT_X, body_y, LEFT_W, body_h, t, "System", NULL);

    for (i = 0; i < SYS_ROW_COUNT; i++) {
        /* Three rows of 62 from 114: the last ends at 300, well inside 412. */
        int y = rows_y + i * SYS_ROW_H;
        int on = (f->sys_row == i);

        if (on) {
            vita2d_draw_rectangle(LEFT_X, y, LEFT_W, SYS_ROW_H, t->sel);
            vita2d_draw_rectangle(LEFT_X, y, 4, SYS_ROW_H, t->accent);
        }

        if (i == SYS_UPDATE) {
            UpdateState us = f->upd ? f->upd->state : UPD_IDLE;
            const char *msg = f->upd ? f->upd->message : "";

            text_cached(x, y + 22, t->text, 0.92f, 200, "Update");
            text_right(LEFT_X + LEFT_W - 18, y + 22, update_color(t, us), 0.78f,
                       320, update_word(us));

            if (us == UPD_DOWNLOADING) {
                snprintf(buf, sizeof(buf), "Installed v%s%s%s", VR_VERSION,
                         msg[0] ? "   " : "", msg);
                text(x, y + 40, t->dim, 0.68f, inner_w, buf);
                /* track 34..494, 8px tall at y+46 -> y+54, inside the 62 row */
                progress_bar(x, y + 46, 460, 8, t, f->upd->progress_pct);
                if (f->upd->progress_pct > 0) {
                    snprintf(buf, sizeof(buf), "%u%%",
                             f->upd->progress_pct > 100 ? 100 : f->upd->progress_pct);
                    text_right(LEFT_X + LEFT_W - 18, y + 54, t->dim, 0.68f, 80, buf);
                } else {
                    text_right(LEFT_X + LEFT_W - 18, y + 54, t->dim, 0.68f, 80,
                               "size unknown");
                }
            } else {
                snprintf(buf, sizeof(buf), "Installed v%s%s%s", VR_VERSION,
                         msg[0] ? "   -   " : "", msg);
                text(x, y + 44, t->dim, 0.7f, inner_w, buf);
            }
        } else if (i == SYS_THEME) {
            text_cached(x, y + 22, t->text, 0.92f, 200, "Theme");
            text_cached(x, y + 44, t->dim, 0.7f, 240, "Colour palette");
            /* 420 .. 594, inside the row's 598 right edge. */
            text_cached(420, y + 36, t->dim, 0.9f, 16, "<");
            text_center(505, y + 36, t->accent, 0.9f, 130, theme_name(f->theme_index));
            text_cached(584, y + 36, t->dim, 0.9f, 16, ">");
        } else {
            text_cached(x, y + 22, t->text, 0.92f, 200, "About");
            text_cached(x, y + 44, t->dim, 0.7f, inner_w,
                        "Vita Radio v" VR_VERSION "  -  internet radio for PS Vita");
        }

        if (!on) vita2d_draw_rectangle(LEFT_X + 18, y + SYS_ROW_H - 1,
                                       LEFT_W - 36, 1, t->bar);
    }

    /* Contextual help, right panel. */
    {
        int hx = RIGHT_X + 14, hw = RIGHT_W - 28;
        int hy = panel(RIGHT_X, body_y, RIGHT_W, body_h, t,
                       TAB_NAMES[TAB_SYSTEM], NULL) + 28;
        sys_help(f->sys_row, &l1, &l2, &l3);
        text(hx, hy, t->text, 0.78f, hw, l1);
        text(hx, hy + 24, t->dim, 0.72f, hw, l2);
        text(hx, hy + 48, t->dim, 0.72f, hw, l3);
        text_cached(hx, hy + 96, t->dim, 0.62f, hw, "THEME");
        text(hx, hy + 118, t->accent, 0.78f, hw, theme_name(f->theme_index));
    }
}

/* --- button hints ---------------------------------------------------------- */

static const char *hints_for(const UiFrame *f)
{
    switch (f->tab) {
    case TAB_FAVOURITES:
        return "L/R Tabs    X Play    O Stop    /\\ Remove    [] Search    START Quit";
    case TAB_SEARCH:
        return "L/R Tabs    [] New search    X Play    O Stop    /\\ Favourite    START Quit";
    case TAB_SYSTEM:
        return (f->upd && f->upd->state == UPD_AVAILABLE)
            ? "L/R Tabs    Up/Down Row    Left/Right Change    X Install update    START Quit"
            : "L/R Tabs    Up/Down Row    Left/Right Change    X Select    START Quit";
    case TAB_STATIONS:
    case TAB_COUNT:
        break;
    }
    return "L/R Tabs    Left/Right Filter    X Play    O Stop    /\\ Favourite    [] Search";
}

static void draw_hints(const UiFrame *f, const Theme *t)
{
    /* (0,514,960,30) -> bottom 544, the last pixel row of the screen. */
    vita2d_draw_rectangle(0, HINT_Y, SCREEN_W, HINT_H, t->bar);
    vita2d_draw_rectangle(0, HINT_Y, SCREEN_W, 2, t->accent);

    /* 16 .. 756, clear of the right-hand note whose right edge is 944. */
    text_cached(16, HINT_Y + 21, t->text, 0.72f, 740, hints_for(f));
    text_right(944, HINT_Y + 21, t->dim, 0.72f, 170,
               f->tab == TAB_SYSTEM ? "START  Quit" : "SELECT  System");
}

/* --- frame ----------------------------------------------------------------- */

void ui_draw(const UiFrame *f)
{
    UiFrame blank;
    const Theme *t;
    int body_y, body_h;

    /* ui_draw is the boundary with main.c: a NULL frame, a NULL theme and any
     * NULL pointer inside the frame all have to draw something rather than
     * fault. The blank frame below is what "no state at all" looks like. */
    if (!f) {
        memset(&blank, 0, sizeof blank);
        blank.tab = TAB_STATIONS;
        blank.chip_active = -1;
        blank.chip_focus = -1;
        f = &blank;
    }
    t = f->theme ? f->theme : theme_at(0);

    s_tick++;

    vita2d_start_drawing();
    vita2d_clear_screen();

    if (s_font) {
        draw_header(f, t);
        draw_tabs(f, t);

        if (f->tab == TAB_STATIONS) {
            draw_chips(f, t);
            body_y = BODY_Y_CHIPS;
        } else {
            body_y = BODY_Y_PLAIN;
        }
        body_h = BODY_BOT - body_y;

        if (f->tab == TAB_SYSTEM) {
            draw_system(f, t, body_y, body_h);
        } else {
            draw_list(f, t, body_y, body_h);
            draw_detail(f, t, body_y, body_h);
        }

        draw_card(f, t);
        draw_hints(f, t);
    }

    vita2d_end_drawing();
    vita2d_swap_buffers();
}

void ui_shutdown(void)
{
    vita2d_wait_rendering_done();
    if (s_font) vita2d_free_pgf(s_font);
    s_font = NULL;
    vita2d_fini();
}
