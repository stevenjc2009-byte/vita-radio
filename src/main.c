#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <curl/curl.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>
#include <vita2d.h>

#include "favourites.h"
#include "ime.h"
#include "player.h"
#include "rbrowser.h"
#include "settings.h"
#include "station_list.h"
#include "stations.h"
#include "theme.h"
#include "ui.h"
#include "updater.h"
#include "version.h"

#define USER_AGENT      "VitaRadio/" VR_VERSION " (PS Vita)"
#define CA_FILE         "app0:assets/cacert.pem"
#define NET_POOL_SIZE   (1 * 1024 * 1024)
/* sceNetCtlInit when already initialised; not named in the SDK headers */
#define SCE_NET_CTL_ERROR_NOT_TERMINATED 0x80412102
#define REPEAT_DELAY_US (400 * 1000)
#define REPEAT_EVERY_US (80 * 1000)
/* Rows asked of radio-browser, for the search box and for every filter chip. */
#define SEARCH_LIMIT    60
#define NOTICE_US       (3 * 1000 * 1000)
#define NO_NET_NOTICE   "No network - enable Wi-Fi and restart"

static char s_net_pool[NET_POOL_SIZE];

/* ==== pure logic: host-probed ===============================================
 * Everything between this marker and END PURE LOGIC is a pure function of its
 * arguments - no globals, no pad, no network, no vita2d - and the scratchpad
 * probe extracts exactly these lines out of this file and compiles them on the
 * PC. Keep it that way, or the probe stops testing the shipped code. */

/* What a filter chip does when it is activated. */
typedef enum {
    CHIP_BUILTIN = 0,   /* the compiled-in list; no network at all */
    CHIP_TOPCLICK,      /* rb_top_click()        */
    CHIP_TAG,           /* rb_by_tag(arg)        */
    CHIP_COUNTRY        /* rb_by_country(arg)    */
} ChipKind;

typedef struct {
    const char *label;  /* what the chip says on screen */
    ChipKind    kind;
    /* Tag text, or an ISO 3166-1 alpha-2 country code. NULL when unused.
     *
     * The code is deliberately NOT the label. rb_by_country feeds
     * /json/stations/bycountrycodeexact/, which matches the alpha-2 code the
     * database stores, and the United Kingdom's is GB. "UK" is two letters so
     * rb_by_country's own validation would accept it and the request would
     * succeed - returning nothing, forever, with no error to notice. */
    const char *arg;
} Chip;

static const Chip s_chips[] = {
    { "Built-in",  CHIP_BUILTIN,  NULL        },
    { "Popular",   CHIP_TOPCLICK, NULL        },
    { "Rock",      CHIP_TAG,      "rock"      },
    { "Jazz",      CHIP_TAG,      "jazz"      },
    { "News",      CHIP_TAG,      "news"      },
    { "Classical", CHIP_TAG,      "classical" },
    { "Dance",     CHIP_TAG,      "dance"     },
    { "UK",        CHIP_COUNTRY,  "GB"        },
    { "US",        CHIP_COUNTRY,  "US"        },
};
#define CHIP_COUNT ((int)(sizeof(s_chips) / sizeof(s_chips[0])))

/* NULL for any index outside the table, so a focus index that drifted out of
 * range can never index it. */
static const Chip *chip_at(int index)
{
    if (index < 0 || index >= CHIP_COUNT)
        return NULL;
    return &s_chips[index];
}

/* Steps index by d and wraps both ways. Used for the tab strip, the chip row
 * and the theme list, which all cycle rather than stopping at the ends. */
static int wrap_index(int index, int d, int count)
{
    if (count <= 0)
        return 0;
    index = (index + d) % count;
    if (index < 0)
        index += count;
    return index;
}

/* Drops a trailing UTF-8 sequence that is missing its continuation bytes, and a
 * stray continuation byte with no lead. snprintf truncates on bytes and knows
 * nothing about encoding, so without this a cut string can end mid-character
 * and the font is handed a byte that is not a character. */
static void utf8_trim(char *s)
{
    size_t len, start, need;
    unsigned char c;

    len = strlen(s);
    if (len == 0)
        return;
    start = len - 1;
    while (start > 0 && ((unsigned char)s[start] & 0xC0) == 0x80)
        start--;

    c = (unsigned char)s[start];
    if (c < 0x80)             need = 1;
    else if ((c & 0xE0) == 0xC0) need = 2;
    else if ((c & 0xF0) == 0xE0) need = 3;
    else if ((c & 0xF8) == 0xF0) need = 4;
    else                         need = 0;   /* a continuation byte with no lead */

    if (c >= 0x80 && (need == 0 || start + need > len))
        s[start] = '\0';
}

/* The separator between the format and the country. U+00B7 is two bytes in
 * UTF-8, which is why the append below is all-or-nothing. */
#define KIND_SEP " \xc2\xb7 "

/* Builds the one-line label under a station name: "MP3 / 128 kbps", and with a
 * country "MP3 / 128 kbps \xc2\xb7 GB".
 *
 * The country is folded into this existing string on purpose. Giving Station a
 * country field would change sl_add's signature and add a column to
 * favourites.tsv, and favourites.tsv is already sitting on users' memory cards
 * written by v2 - a fourth column would break every one of them. */
static void compose_kind(char *dst, size_t dsz, const char *codec, int bitrate,
                         int is_hls, const char *country)
{
    const char *fmt;
    int named;
    size_t len, need;

    if (!dst || dsz == 0)
        return;

    /* The API reports "UNKNOWN" for many HLS feeds, which reads worse on the
     * row than saying nothing, so treat it as an absent codec. */
    named = codec && codec[0] && strcmp(codec, "UNKNOWN") != 0;
    fmt = is_hls ? "HLS" : (named ? codec : "stream");

    if (bitrate > 0)
        snprintf(dst, dsz, "%s / %d kbps", fmt, bitrate);
    else
        snprintf(dst, dsz, "%s", fmt);
    utf8_trim(dst);

    if (!country || !country[0])
        return;
    /* Appended only when the whole suffix fits. Letting snprintf truncate it
     * could leave half of KIND_SEP's two-byte separator behind; dropping the
     * country entirely is the clean cut. */
    len = strlen(dst);
    need = strlen(KIND_SEP) + strlen(country);
    if (len + need + 1 <= dsz)
        snprintf(dst + len, dsz - len, "%s%s", KIND_SEP, country);
}

/* ==== END PURE LOGIC ======================================================= */

/* UiFrame wants a flat array of labels; the labels themselves live in s_chips
 * so there is only ever one copy of them. Filled once by chips_init(). */
static const char *s_chip_labels[CHIP_COUNT];

static void chips_init(void)
{
    int i;
    for (i = 0; i < CHIP_COUNT; i++)
        s_chip_labels[i] = s_chips[i].label;
}

/* The Stations tab shows whichever filter was last loaded; Favourites and
 * Search have a list each; the System tab has none. */
static StationList s_stations;
static StationList s_favs;
static StationList s_search;
static char        s_stations_label[96] = "Built-in";
static char        s_search_label[96]   = "Search";

/* The main loop's own state, grouped so the three places that paint a frame can
 * fill a UiFrame with one call instead of a dozen assignments each. */
typedef struct {
    UiTab tab;
    int   selected;     /* row in the current tab's list */
    int   chip_active;  /* chip whose results are on screen, -1 for none */
    int   chip_focus;   /* chip the d-pad is sitting on */
    int   sys_row;      /* a SysRow */
} AppState;

static int net_start(void)
{
    if (sceSysmoduleLoadModule(SCE_SYSMODULE_NET) < 0) return -1;

    SceNetInitParam param;
    param.memory = s_net_pool;
    param.size = sizeof(s_net_pool);
    param.flags = 0;
    int ret = sceNetInit(&param);
    /* already initialised is fine */
    if (ret < 0 && ret != (int)SCE_NET_ERROR_EBUSY) return -2;

    ret = sceNetCtlInit();
    if (ret < 0 && ret != (int)SCE_NET_CTL_ERROR_NOT_TERMINATED) return -3;
    return 0;
}

/* NULL on the System tab: it draws settings rows, not stations. */
static StationList *list_for_tab(UiTab t)
{
    switch (t) {
    case TAB_FAVOURITES: return &s_favs;
    case TAB_SEARCH:     return &s_search;
    case TAB_SYSTEM:     return NULL;
    case TAB_STATIONS:
    default:             return &s_stations;
    }
}

static const char *label_for_tab(UiTab t)
{
    switch (t) {
    case TAB_FAVOURITES: return "Favourites";
    case TAB_SEARCH:     return s_search_label;
    case TAB_SYSTEM:     return "System";
    case TAB_STATIONS:
    default:             return s_stations_label;
    }
}

static void frame_fill(UiFrame *f, const AppState *a, const Settings *cfg,
                       const PlayerStatus *st, const UpdateStatus *upd,
                       const char *notice)
{
    memset(f, 0, sizeof(*f));
    f->tab         = a->tab;
    f->list        = list_for_tab(a->tab);
    f->list_label  = label_for_tab(a->tab);
    f->selected    = a->selected;
    f->chips       = s_chip_labels;
    f->chip_count  = CHIP_COUNT;
    f->chip_active = a->chip_active;
    f->chip_focus  = a->chip_focus;
    f->sys_row     = a->sys_row;
    f->theme_index = cfg->theme;
    f->player      = st;
    f->upd         = upd;
    f->notice      = notice;
    f->theme       = theme_at(cfg->theme);
}

/* Marks entries in l that are in the favourites list, so the star shows up on
 * directory and search rows too. */
static void mark_favourites(StationList *l)
{
    for (int i = 0; i < l->count; i++)
        l->items[i].is_fav = l->items[i].url && sl_find_url(&s_favs, l->items[i].url) >= 0;
}

/* Adds or removes the selected station from the favourites, then persists. */
static void toggle_favourite(StationList *l, int index, char *notice, size_t nsz)
{
    if (index < 0 || index >= l->count)
        return;
    Station *s = &l->items[index];
    if (!s->url)
        return;

    int at = sl_find_url(&s_favs, s->url);
    if (at >= 0) {
        /* Clear the flag first: when the favourites list is the one on screen,
         * s points into the slot sl_remove is about to compact away. */
        s->is_fav = 0;
        sl_remove(&s_favs, at);
        snprintf(notice, nsz, "Removed from favourites");
    } else {
        int added = sl_add(&s_favs, s->name, s->url, s->kind);
        if (added < 0) {
            snprintf(notice, nsz, "Out of memory");
            return;
        }
        s_favs.items[added].is_fav = 1;
        s->is_fav = 1;
        snprintf(notice, nsz, "Added to favourites");
    }
    /* The favourites list itself may be on screen; keep its flags true. */
    for (int i = 0; i < s_favs.count; i++)
        s_favs.items[i].is_fav = 1;
    mark_favourites(&s_stations);
    mark_favourites(&s_search);

    if (fav_save(&s_favs, VR_FAV_FILE) != 0)
        snprintf(notice, nsz, "Could not save favourites");
}

/* Replaces l with the directory result. Returns 0, or -1 if the list could not
 * be grown - in which case l holds however many fitted and the caller has to
 * say so on screen. sl_add's return used to be dropped here, so a failed
 * allocation lost stations with nothing anywhere to show it had happened. */
static int fill_from_result(StationList *l, const RbResult *res)
{
    int i;

    sl_clear(l);
    for (i = 0; i < res->count; i++) {
        const RbStation *rs = &res->items[i];
        char kind[48];

        compose_kind(kind, sizeof(kind), rs->codec, rs->bitrate, rs->is_hls, rs->country);
        if (sl_add(l, rs->name, rs->url, kind) < 0)
            return -1;
    }
    return 0;
}

/* Maps one chip onto the directory endpoint it stands for. CHIP_BUILTIN never
 * reaches here - it has no endpoint and the caller handles it without the
 * network. Returns 0 with out filled, or -1 with a reason in err. */
static int chip_fetch(const Chip *c, RbResult *out, char *err, size_t errsz)
{
    switch (c->kind) {
    case CHIP_TOPCLICK: return rb_top_click(SEARCH_LIMIT, out, err, errsz);
    case CHIP_TAG:      return rb_by_tag(c->arg, SEARCH_LIMIT, out, err, errsz);
    case CHIP_COUNTRY:  return rb_by_country(c->arg, SEARCH_LIMIT, out, err, errsz);
    case CHIP_BUILTIN:
    default:
        snprintf(err, errsz, "no endpoint for this filter");
        return -1;
    }
}

/* Switches theme and writes the choice back. The save is the only thing that
 * makes it survive a reboot, so a failed save has to be visible rather than
 * silently forgotten. */
static void set_theme(Settings *cfg, int index, char *notice, size_t nsz)
{
    cfg->theme = index;
    ui_set_theme(theme_at(cfg->theme));
    if (settings_save(cfg, VR_SETTINGS_FILE) != 0)
        snprintf(notice, nsz, "Could not save settings");
    else
        snprintf(notice, nsz, "Theme: %s", theme_name(cfg->theme));
}

/* sceCtrlPeekBufferPositive is a level read and prev_buttons is only refreshed
 * at the bottom of the main loop, so a button still held when a blocking call
 * returns would read as a fresh press. Adopt the live pad state instead, and
 * restart the auto-repeat clocks with it. */
static unsigned int pad_resync(SceUInt64 *hold_since, SceUInt64 *last_repeat)
{
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(0, &pad, 1);
    *hold_since = *last_repeat = sceKernelGetProcessTimeWide();
    return pad.buttons;
}

/* --- shutdown probe (temporary) ---------------------------------------------
 * The teardown below runs with nothing on screen, so a call that never returns
 * is indistinguishable from a clean exit. One flat frame before each call
 * leaves the last colour that was reached on the display, naming the call that
 * hung. Remove once the exit path is confirmed good on hardware. */
static void shutdown_probe(unsigned int col)
{
    vita2d_start_drawing();
    vita2d_set_clear_color(col);
    vita2d_clear_screen();
    vita2d_end_drawing();
    vita2d_swap_buffers();
}

int main(void)
{
    int net_ok = net_start() == 0;

    scePowerSetArmClockFrequency(444);

    /* Without a successful curl_global_init every curl_easy_init in the player,
     * updater and browser is undefined behaviour, so nothing that touches the
     * network starts unless both the socket layer and curl came up. */
    int curl_ok = 0, player_ok = 0;
    if (net_ok) {
        curl_ok = curl_global_init(CURL_GLOBAL_ALL) == CURLE_OK;
        if (curl_ok) {
            player_ok = player_init(USER_AGENT, CA_FILE) == 0;
            updater_init(USER_AGENT, CA_FILE);
        }
    }
    rb_set_user_agent(USER_AGENT);
    rb_set_ca_file(CA_FILE);

    /* Settings first: ui_init takes the theme, so the very first frame is
     * already in the colours the user chose rather than flashing the default.
     * A missing file is not an error - settings_load leaves the defaults and
     * returns 0, so a first run must not show a failure here. */
    Settings cfg;
    settings_defaults(&cfg);
    int cfg_failed = settings_load(&cfg, VR_SETTINGS_FILE) != 0;

    chips_init();
    ui_init(theme_at(cfg.theme));
    int ime_ok = ime_init() == 0;   /* after ui_init: the dialog composites through GXM */

    sl_init(&s_stations);
    sl_init(&s_favs);
    sl_init(&s_search);
    sl_add_builtins(&s_stations);
    fav_ensure_dir();
    fav_load(&s_favs, VR_FAV_FILE);
    mark_favourites(&s_stations);

    AppState app;
    memset(&app, 0, sizeof(app));
    app.tab = TAB_STATIONS;
    app.selected = 0;
    app.chip_active = 0;        /* the built-in list is what is on screen */
    app.chip_focus = 0;
    app.sys_row = SYS_UPDATE;

    unsigned int prev_buttons = 0;
    SceUInt64 hold_since = 0, last_repeat = 0;
    PlayerStatus st;
    UpdateStatus upd;
    UiFrame f;
    /* Big enough for the longest message built below: a full 255-byte query
     * quoted inside "Searching for ..." or "No stations found for ...". */
    char notice[320] = "";
    /* 0 means the notice has no deadline - the no-network line is re-armed
     * every frame and is meant to stay put. */
    SceUInt64 notice_until = 0;
    int running = 1, launch_updater = 0;

    memset(&st, 0, sizeof(st));
    memset(&upd, 0, sizeof(upd));

    if (cfg_failed) {
        snprintf(notice, sizeof(notice), "Could not read settings - using defaults");
        notice_until = sceKernelGetProcessTimeWide() + NOTICE_US;
    }

    if (net_ok && !curl_ok) {
        /* Nothing downstream can run without curl, and the teardown below is
         * still the right way out, so say so and fall straight through it. */
        snprintf(notice, sizeof(notice), "HTTP init failed");
        frame_fill(&f, &app, &cfg, &st, &upd, notice);
        for (int i = 0; i < 180; i++)   /* ~3 s; ui_draw swaps at vsync */
            ui_draw(&f);
        running = 0;
    }

    while (running) {
        StationList *list = list_for_tab(app.tab);
        if (list) {
            if (app.selected >= list->count) app.selected = list->count - 1;
            if (app.selected < 0) app.selected = 0;
        }

        SceCtrlData pad;
        memset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned int now_buttons = pad.buttons;
        unsigned int pressed = now_buttons & ~prev_buttons;
        SceUInt64 now = sceKernelGetProcessTimeWide();

        /* Transient notices expire, or they shadow the updater's message for
         * the rest of the session (ui_draw gives notice the higher priority). */
        if (notice_until != 0 && now >= notice_until) {
            notice[0] = '\0';
            notice_until = 0;
        }
        /* With no socket layer the app is list-only; keep saying so. */
        if (!net_ok && notice[0] == '\0')
            snprintf(notice, sizeof(notice), NO_NET_NOTICE);

        /* Up/Down: move on press, then auto-repeat while held */
        unsigned int dir_mask = SCE_CTRL_UP | SCE_CTRL_DOWN;
        int step = 0;
        if (pressed & dir_mask) {
            step = (pressed & SCE_CTRL_UP) ? -1 : 1;
            hold_since = now;
            last_repeat = now;
        } else if (now_buttons & dir_mask) {
            if (now - hold_since >= REPEAT_DELAY_US && now - last_repeat >= REPEAT_EVERY_US) {
                step = (now_buttons & SCE_CTRL_UP) ? -1 : 1;
                last_repeat = now;
            }
        }
        if (step != 0) {
            if (app.tab == TAB_SYSTEM) {
                app.sys_row += step;
                if (app.sys_row < 0) app.sys_row = 0;
                if (app.sys_row >= SYS_ROW_COUNT) app.sys_row = SYS_ROW_COUNT - 1;
            } else if (list && list->count > 0) {
                app.selected += step;
                if (app.selected < 0) app.selected = 0;
                if (app.selected >= list->count) app.selected = list->count - 1;
            }
        }

        /* Left/Right act rather than just move, on both tabs that use them, so
         * they fire on the press only - no auto-repeat. Held down, a repeat
         * here would be a burst of directory requests or a burst of writes to
         * the memory card. */
        unsigned int lr_mask = SCE_CTRL_LEFT | SCE_CTRL_RIGHT;
        int hstep = 0;
        if (pressed & lr_mask)
            hstep = (pressed & SCE_CTRL_LEFT) ? -1 : 1;

        updater_get_status(&upd);
        int installing = upd.state == UPD_DOWNLOADING || upd.state == UPD_INSTALLING ||
                         upd.state == UPD_READY;

        /* Outside the !installing gate: a download that stalls (Wi-Fi dropped
         * mid-transfer) never leaves UPD_DOWNLOADING, and START must still get
         * the user out. updater_shutdown() cancels the transfer and joins. */
        if (pressed & SCE_CTRL_START) running = 0;

        if (!installing) {
            /* L / R: previous / next tab, wrapping. */
            if ((pressed & SCE_CTRL_LTRIGGER) || (pressed & SCE_CTRL_RTRIGGER)) {
                app.tab = (UiTab)wrap_index((int)app.tab,
                                            (pressed & SCE_CTRL_LTRIGGER) ? -1 : 1,
                                            TAB_COUNT);
                app.selected = 0;
                notice[0] = '\0';
                notice_until = 0;
            }

            /* SELECT: straight to the System tab. */
            if (pressed & SCE_CTRL_SELECT) {
                app.tab = TAB_SYSTEM;
                app.selected = 0;
            }

            if (hstep != 0 && app.tab == TAB_STATIONS) {
                const Chip *c;

                app.chip_focus = wrap_index(app.chip_focus, hstep, CHIP_COUNT);
                c = chip_at(app.chip_focus);
                if (c && c->kind == CHIP_BUILTIN) {
                    sl_clear(&s_stations);
                    sl_add_builtins(&s_stations);
                    mark_favourites(&s_stations);
                    snprintf(s_stations_label, sizeof(s_stations_label), "%s", c->label);
                    app.chip_active = app.chip_focus;
                    app.selected = 0;
                    notice[0] = '\0';
                    notice_until = 0;
                } else if (c && !net_ok) {
                    snprintf(notice, sizeof(notice), NO_NET_NOTICE);
                    notice_until = 0;
                } else if (c) {
                    RbResult res;
                    char err[160];
                    int rc;

                    /* chip_fetch blocks the UI thread on the network, exactly
                     * like the SQUARE search does, so this follows that path
                     * step for step: paint one frame saying what is happening,
                     * block, then re-sync the pad - whatever is held when a
                     * multi-second call returns is not a press the user made
                     * at this screen. */
                    snprintf(notice, sizeof(notice), "Loading %s...", c->label);
                    player_get_status(&st);
                    frame_fill(&f, &app, &cfg, &st, &upd, notice);
                    ui_draw(&f);

                    memset(&res, 0, sizeof(res));
                    rc = chip_fetch(c, &res, err, sizeof(err));
                    now_buttons = pad_resync(&hold_since, &last_repeat);
                    if (rc == 0) {
                        /* chip_active only moves on success: a failed fetch
                         * leaves the previous results on screen, and the chip
                         * the user moved to shown as focused but not active. */
                        int full = fill_from_result(&s_stations, &res);
                        mark_favourites(&s_stations);
                        snprintf(s_stations_label, sizeof(s_stations_label), "%s", c->label);
                        app.chip_active = app.chip_focus;
                        app.selected = 0;
                        if (full != 0) {
                            snprintf(notice, sizeof(notice),
                                     "Out of memory - showing %d of %d",
                                     s_stations.count, res.count);
                            notice_until = sceKernelGetProcessTimeWide() + NOTICE_US;
                        } else if (s_stations.count == 0) {
                            snprintf(notice, sizeof(notice), "No stations for %s", c->label);
                            notice_until = sceKernelGetProcessTimeWide() + NOTICE_US;
                        } else {
                            notice[0] = '\0';
                            notice_until = 0;
                        }
                    } else {
                        snprintf(notice, sizeof(notice), "%s failed: %s", c->label, err);
                        notice_until = sceKernelGetProcessTimeWide() + NOTICE_US;
                    }
                    rb_result_free(&res);
                }
            } else if (hstep != 0 && app.tab == TAB_SYSTEM && app.sys_row == SYS_THEME) {
                set_theme(&cfg, wrap_index(cfg.theme, hstep, theme_count()),
                          notice, sizeof(notice));
                notice_until = sceKernelGetProcessTimeWide() + NOTICE_US;
            }

            /* CROSS: play the selection, or act on the System row. */
            if (pressed & SCE_CTRL_CROSS) {
                if (app.tab == TAB_SYSTEM) {
                    if (app.sys_row == SYS_UPDATE) {
                        if (!net_ok) {
                            snprintf(notice, sizeof(notice), NO_NET_NOTICE);
                            notice_until = 0;
                        } else if (upd.state == UPD_AVAILABLE) {
                            if (player_ok) player_stop();
                            updater_install();
                        } else {
                            updater_check();
                        }
                    } else if (app.sys_row == SYS_THEME) {
                        set_theme(&cfg, wrap_index(cfg.theme, 1, theme_count()),
                                  notice, sizeof(notice));
                        notice_until = sceKernelGetProcessTimeWide() + NOTICE_US;
                    }
                    /* SYS_ABOUT is not activatable. */
                } else {
                    StationList *l = list_for_tab(app.tab);
                    if (player_ok && l && app.selected >= 0 && app.selected < l->count &&
                        l->items[app.selected].url)
                        player_play(l->items[app.selected].url);
                }
            }

            /* CIRCLE: stop playback. */
            if ((pressed & SCE_CTRL_CIRCLE) && player_ok) player_stop();

            /* TRIANGLE: toggle the favourite. Nothing to toggle on System. */
            if (pressed & SCE_CTRL_TRIANGLE) {
                StationList *l = list_for_tab(app.tab);
                if (l) {
                    toggle_favourite(l, app.selected, notice, sizeof(notice));
                    notice_until = sceKernelGetProcessTimeWide() + NOTICE_US;
                }
            }

            /* SQUARE: the search keyboard. */
            if (pressed & SCE_CTRL_SQUARE) {
                /* 64 UTF-16 units of accented text can reach 256 UTF-8 bytes. */
                char query[256] = "";
                int got = -1;

                if (!net_ok) {
                    snprintf(notice, sizeof(notice), NO_NET_NOTICE);
                    notice_until = 0;
                } else if (!ime_ok) {
                    /* ime_init failed, so the keyboard would never appear -
                     * say that rather than looking like a dead button. */
                    snprintf(notice, sizeof(notice), "Keyboard unavailable");
                    notice_until = sceKernelGetProcessTimeWide() + NOTICE_US;
                } else {
                    got = ime_prompt("Search stations", "", query, sizeof(query));
                    now_buttons = pad_resync(&hold_since, &last_repeat);
                    if (got < 0) {
                        snprintf(notice, sizeof(notice), "Keyboard unavailable");
                        notice_until = sceKernelGetProcessTimeWide() + NOTICE_US;
                    }
                }

                if (got == 1 && query[0]) {
                    RbResult res;
                    char err[160];
                    int found;

                    /* rb_search_name blocks on the network, so paint one frame
                     * saying so before the UI thread stalls on it. */
                    snprintf(notice, sizeof(notice), "Searching for \"%s\"...", query);
                    player_get_status(&st);
                    frame_fill(&f, &app, &cfg, &st, &upd, notice);
                    ui_draw(&f);

                    memset(&res, 0, sizeof(res));
                    found = rb_search_name(query, SEARCH_LIMIT, &res, err, sizeof(err));
                    /* The search blocked for seconds; whatever is held now is
                     * not a press the user made at this screen. */
                    now_buttons = pad_resync(&hold_since, &last_repeat);
                    if (found == 0) {
                        int full = fill_from_result(&s_search, &res);
                        mark_favourites(&s_search);
                        snprintf(s_search_label, sizeof(s_search_label), "Search: %s", query);
                        app.tab = TAB_SEARCH;
                        app.selected = 0;
                        if (full != 0) {
                            snprintf(notice, sizeof(notice),
                                     "Out of memory - showing %d of %d",
                                     s_search.count, res.count);
                            notice_until = sceKernelGetProcessTimeWide() + NOTICE_US;
                        } else if (s_search.count == 0) {
                            snprintf(notice, sizeof(notice),
                                     "No stations found for \"%s\"", query);
                            notice_until = sceKernelGetProcessTimeWide() + NOTICE_US;
                        } else {
                            notice[0] = '\0';
                            notice_until = 0;
                        }
                    } else {
                        snprintf(notice, sizeof(notice), "Search failed: %s", err);
                        notice_until = sceKernelGetProcessTimeWide() + NOTICE_US;
                    }
                    /* Freed on both paths. rb_result_free is documented as the
                     * only way to release a result and costs nothing on an
                     * empty one; the failure path used to skip it entirely. */
                    rb_result_free(&res);
                }
            }
        }
        if (upd.state == UPD_READY) {
            launch_updater = 1;
            running = 0;
        }

        prev_buttons = now_buttons;

        player_get_status(&st);
        if (!player_ok) {
            st.state = PLAYER_ERROR;
            strcpy(st.error, "Player failed to start");
        }
        frame_fill(&f, &app, &cfg, &st, &upd, notice);
        ui_draw(&f);     /* swaps at vsync */
    }

    shutdown_probe(RGBA8(200, 30, 30, 255));    /* red */
    updater_shutdown();
    shutdown_probe(RGBA8(30, 180, 60, 255));    /* green */
    if (player_ok) player_shutdown();
    /* Before ui_shutdown: SceAppUtil's service thread and its SceShell channel
     * outlive sceKernelExitProcess, and the dialog draws through GXM. */
    ime_shutdown();
    shutdown_probe(RGBA8(40, 70, 220, 255));    /* blue */
    ui_shutdown();
    sl_free(&s_stations);
    sl_free(&s_favs);
    sl_free(&s_search);
    if (curl_ok) curl_global_cleanup();
    if (net_ok) {
        sceNetCtlTerm();
        sceNetTerm();
        sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
    }
    if (launch_updater)
        updater_launch();   /* does not return */
    sceKernelExitProcess(0);
    return 0;
}
