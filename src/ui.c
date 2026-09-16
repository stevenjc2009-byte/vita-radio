#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <vita2d.h>

#include "version.h"

#define SCREEN_W 960
#define SCREEN_H 544

#define COL_BG       RGBA8(18, 20, 30, 255)
#define COL_BAR      RGBA8(40, 44, 64, 255)
#define COL_PANEL    RGBA8(28, 31, 46, 255)
#define COL_SEL      RGBA8(60, 90, 150, 255)
#define COL_TEXT     RGBA8(230, 232, 240, 255)
#define COL_DIM      RGBA8(150, 155, 175, 255)
#define COL_ACCENT   RGBA8(255, 170, 40, 255)
#define COL_OK       RGBA8(110, 220, 120, 255)
#define COL_ERR      RGBA8(255, 80, 80, 255)

#define LIST_X   16
#define LIST_W   440
#define PANEL_X  (LIST_X + LIST_W + 16)
#define PANEL_W  (SCREEN_W - PANEL_X - 16)
#define TOP_Y    56
#define LIST_Y   (TOP_Y + 24)     /* room for the list label above the rows */
#define FOOT_Y   (SCREEN_H - 36)
#define ROW_H    52

static vita2d_pgf *s_font;

/* Copies src into dst, shortened with "..." so it renders no wider than max_w. */
static const char *fit(char *dst, size_t dst_sz, const char *src, float scale, int max_w)
{
    if (!src) src = "";
    snprintf(dst, dst_sz, "%s", src);
    if (!s_font || vita2d_pgf_text_width(s_font, scale, dst) <= max_w) return dst;

    size_t len = strlen(dst);
    while (len > 0) {
        len--;
        /* don't cut in the middle of a UTF-8 sequence */
        while (len > 0 && ((unsigned char)dst[len] & 0xC0) == 0x80) len--;
        if (len + 4 > dst_sz) continue;
        memcpy(dst + len, "...", 4);
        if (vita2d_pgf_text_width(s_font, scale, dst) <= max_w) return dst;
    }
    snprintf(dst, dst_sz, "...");
    return dst;
}

static void text(int x, int y, unsigned int col, float scale, int max_w, const char *s)
{
    char buf[600];
    vita2d_pgf_draw_text(s_font, x, y, col, scale, fit(buf, sizeof(buf), s, scale, max_w));
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

static unsigned int state_color(PlayerState st)
{
    switch (st) {
    case PLAYER_PLAYING: return COL_OK;
    case PLAYER_ERROR:   return COL_ERR;
    case PLAYER_IDLE:    return COL_DIM;
    default:             return COL_ACCENT;
    }
}

void ui_init(void)
{
    vita2d_init();
    vita2d_set_clear_color(COL_BG);
    s_font = vita2d_load_default_pgf();
}

static void draw_list(const StationList *list, const char *label, int selected,
                      const PlayerStatus *st)
{
    int count = list ? list->count : 0;
    int list_h = FOOT_Y - LIST_Y - 8;
    int visible = list_h / ROW_H;
    int first = 0;
    if (visible < 1) visible = 1;
    if (selected >= visible) first = selected - visible + 1;

    char head[160];
    snprintf(head, sizeof(head), "%s (%d)", label ? label : "Stations", count);
    text(LIST_X, TOP_Y + 16, COL_ACCENT, 0.9f, LIST_W, head);

    vita2d_draw_rectangle(LIST_X, LIST_Y, LIST_W, list_h, COL_PANEL);

    if (count == 0) {
        text(LIST_X + 16, LIST_Y + 34, COL_DIM, 0.9f, LIST_W - 32, "No stations in this list.");
        return;
    }

    for (int i = first; i < count && i < first + visible; i++) {
        const Station *s = &list->items[i];
        int y = LIST_Y + (i - first) * ROW_H;
        int active = st && st->state != PLAYER_IDLE && s->url &&
                     strcmp(st->url, s->url) == 0;

        if (i == selected) vita2d_draw_rectangle(LIST_X, y, LIST_W, ROW_H, COL_SEL);
        if (active) {
            vita2d_draw_rectangle(LIST_X, y, 6, ROW_H, state_color(st->state));
            text(LIST_X + LIST_W - 30, y + 24, state_color(st->state), 1.0f, 24, ">");
        }
        if (s->is_fav)
            text(LIST_X + LIST_W - 52, y + 24, COL_ACCENT, 1.0f, 24, "*");

        text(LIST_X + 16, y + 22, COL_TEXT, 1.0f, LIST_W - 76, s->name ? s->name : "");
        text(LIST_X + 16, y + 44, COL_DIM, 0.8f, LIST_W - 76, s->kind ? s->kind : "");
    }
}

static void draw_status(const PlayerStatus *st)
{
    int x = PANEL_X + 16, w = PANEL_W - 32, y = TOP_Y + 30;
    char line[600];

    vita2d_draw_rectangle(PANEL_X, TOP_Y, PANEL_W, FOOT_Y - TOP_Y - 8, COL_PANEL);
    if (!st) return;

    text(x, y, state_color(st->state), 1.4f, w, state_word(st->state));
    y += 36;

    text(x, y, COL_DIM, 0.9f, w, "Now playing:");
    y += 26;
    text(x, y, COL_TEXT, 1.0f, w, st->title[0] ? st->title : "-");
    y += 36;

    if (st->codec[0] || st->in_rate > 0)
        snprintf(line, sizeof(line), "Codec: %s  %d Hz  %d ch",
                 st->codec[0] ? st->codec : "?", st->in_rate, st->in_channels);
    else
        snprintf(line, sizeof(line), "Codec: -");
    text(x, y, COL_TEXT, 0.9f, w, line);
    y += 26;

    snprintf(line, sizeof(line), "Content-Type: %s", st->content_type[0] ? st->content_type : "-");
    text(x, y, COL_TEXT, 0.9f, w, line);
    y += 26;

    if (st->http_status > 0) snprintf(line, sizeof(line), "HTTP status: %ld", st->http_status);
    else                     snprintf(line, sizeof(line), "HTTP status: -");
    text(x, y, COL_TEXT, 0.9f, w, line);
    y += 26;

    unsigned pct = st->buffer_pct > 100 ? 100 : st->buffer_pct;
    snprintf(line, sizeof(line), "Buffer: %u%%", pct);
    text(x, y, COL_TEXT, 0.9f, w, line);
    vita2d_draw_rectangle(x + 150, y - 14, w - 150, 14, COL_BAR);
    vita2d_draw_rectangle(x + 150, y - 14, (float)(w - 150) * pct / 100.0f, 14, COL_ACCENT);
    y += 26;

    snprintf(line, sizeof(line), "Received: %lu KB", st->bytes_received / 1024UL);
    text(x, y, COL_TEXT, 0.9f, w, line);
    y += 36;

    if (st->state == PLAYER_ERROR) {
        text(x, y, COL_ERR, 0.9f, w, "Error:");
        y += 26;
        text(x, y, COL_ERR, 0.9f, w, st->error[0] ? st->error : "(no detail)");
    }
    y += 26;

    if (st->url[0] && y < FOOT_Y - 20) text(x, FOOT_Y - 20, COL_DIM, 0.7f, w, st->url);
}

static unsigned int update_color(UpdateState st)
{
    switch (st) {
    case UPD_AVAILABLE: return COL_OK;
    case UPD_ERROR:     return COL_ERR;
    case UPD_UP_TO_DATE:
    case UPD_IDLE:      return COL_DIM;
    default:            return COL_ACCENT;
    }
}

void ui_draw(const StationList *list, const char *list_label, int selected,
             const PlayerStatus *st, const UpdateStatus *upd, const char *notice)
{
    vita2d_start_drawing();
    vita2d_clear_screen();

    if (s_font) {
        vita2d_draw_rectangle(0, 0, SCREEN_W, 44, COL_BAR);
        text(16, 31, COL_ACCENT, 1.3f, 200, "Vita Radio");
        text(160, 31, COL_DIM, 0.8f, 80, "v" VR_VERSION);

        /* A transient notice (searching, search failed) outranks the updater
         * message in the title bar - it is the thing the user just asked for. */
        if (notice && notice[0])
            text(250, 30, COL_ACCENT, 0.85f, SCREEN_W - 266, notice);
        else if (upd && upd->message[0])
            text(250, 30, update_color(upd->state), 0.85f, SCREEN_W - 266, upd->message);

        draw_list(list, list_label, selected, st);
        draw_status(st);

        vita2d_draw_rectangle(0, FOOT_Y, SCREEN_W, SCREEN_H - FOOT_Y, COL_BAR);
        text(16, FOOT_Y + 25, COL_TEXT, 0.85f, SCREEN_W - 32,
             upd && upd->state == UPD_AVAILABLE
                 ? "X play  O stop  [] search  L/R list  SELECT fav  /\\ install update  START quit"
                 : "X play  O stop  [] search  L/R list  SELECT fav  /\\ updates  START quit");
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
