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

#include "favourites.h"
#include "ime.h"
#include "player.h"
#include "rbrowser.h"
#include "station_list.h"
#include "stations.h"
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
#define SEARCH_LIMIT    60

static char s_net_pool[NET_POOL_SIZE];

/* The three lists the user can page through with L/R. */
typedef enum { LIST_BUILTIN = 0, LIST_FAVOURITES, LIST_SEARCH, LIST_COUNT } ListMode;

static StationList s_builtin;
static StationList s_favs;
static StationList s_search;
static char        s_search_label[96] = "Search";

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

static StationList *list_for(ListMode m)
{
    switch (m) {
    case LIST_FAVOURITES: return &s_favs;
    case LIST_SEARCH:     return &s_search;
    case LIST_BUILTIN:
    default:              return &s_builtin;
    }
}

static const char *label_for(ListMode m)
{
    switch (m) {
    case LIST_FAVOURITES: return "Favourites";
    case LIST_SEARCH:     return s_search_label;
    case LIST_BUILTIN:
    default:              return "Built-in";
    }
}

/* Marks entries in l that are in the favourites list, so the star shows up on
 * built-in and search rows too. */
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
        sl_remove(&s_favs, at);
        s->is_fav = 0;
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
    mark_favourites(&s_builtin);
    mark_favourites(&s_search);

    if (fav_save(&s_favs, VR_FAV_FILE) != 0)
        snprintf(notice, nsz, "Could not save favourites");
}

int main(void)
{
    int net_ok = net_start() == 0;

    scePowerSetArmClockFrequency(444);

    int curl_ok = curl_global_init(CURL_GLOBAL_ALL) == CURLE_OK;
    int player_ok = player_init(USER_AGENT, CA_FILE) == 0;
    updater_init(USER_AGENT, CA_FILE);
    rb_set_user_agent(USER_AGENT);
    rb_set_ca_file(CA_FILE);
    ui_init();
    ime_init();   /* after ui_init: the dialog composites through GXM */

    sl_init(&s_builtin);
    sl_init(&s_favs);
    sl_init(&s_search);
    sl_add_builtins(&s_builtin);
    fav_ensure_dir();
    fav_load(&s_favs, VR_FAV_FILE);
    mark_favourites(&s_builtin);

    ListMode mode = LIST_BUILTIN;
    int selected = 0;
    unsigned int prev_buttons = 0;
    SceUInt64 hold_since = 0, last_repeat = 0;
    PlayerStatus st;
    UpdateStatus upd;
    /* Big enough for the longest message built below: a full 255-byte query
     * quoted inside "Searching for ..." or "No stations found for ...". */
    char notice[320] = "";
    int running = 1, launch_updater = 0;

    while (running) {
        StationList *list = list_for(mode);
        if (selected >= list->count) selected = list->count - 1;
        if (selected < 0) selected = 0;

        SceCtrlData pad;
        memset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned int now_buttons = pad.buttons;
        unsigned int pressed = now_buttons & ~prev_buttons;
        SceUInt64 now = sceKernelGetProcessTimeWide();

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
        if (step != 0 && list->count > 0) {
            selected += step;
            if (selected < 0) selected = 0;
            if (selected >= list->count) selected = list->count - 1;
        }

        updater_get_status(&upd);
        int installing = upd.state == UPD_DOWNLOADING || upd.state == UPD_INSTALLING ||
                         upd.state == UPD_READY;

        if (!installing) {
            if ((pressed & SCE_CTRL_LTRIGGER) || (pressed & SCE_CTRL_RTRIGGER)) {
                int d = (pressed & SCE_CTRL_LTRIGGER) ? LIST_COUNT - 1 : 1;
                mode = (ListMode)(((int)mode + d) % LIST_COUNT);
                selected = 0;
                notice[0] = '\0';
                list = list_for(mode);
            }
            if (pressed & SCE_CTRL_CROSS) {
                if (list->count > 0 && list->items[selected].url)
                    player_play(list->items[selected].url);
            }
            if (pressed & SCE_CTRL_CIRCLE) player_stop();
            if (pressed & SCE_CTRL_SELECT)
                toggle_favourite(list, selected, notice, sizeof(notice));
            if (pressed & SCE_CTRL_START) running = 0;

            if (pressed & SCE_CTRL_SQUARE) {
                /* 64 UTF-16 units of accented text can reach 256 UTF-8 bytes. */
                char query[256] = "";
                if (ime_prompt("Search stations", "", query, sizeof(query)) == 1 && query[0]) {
                    /* rb_search_name blocks on the network, so paint one frame
                     * saying so before the UI thread stalls on it. */
                    snprintf(notice, sizeof(notice), "Searching for \"%s\"...", query);
                    player_get_status(&st);
                    ui_draw(list, label_for(mode), selected, &st, &upd, notice);

                    RbResult res;
                    char err[160];
                    memset(&res, 0, sizeof(res));
                    if (rb_search_name(query, SEARCH_LIMIT, &res, err, sizeof(err)) == 0) {
                        sl_clear(&s_search);
                        for (int i = 0; i < res.count; i++) {
                            const RbStation *rs = &res.items[i];
                            char kind[48];
                            /* The API reports "UNKNOWN" for many HLS feeds,
                             * which reads worse on the row than saying
                             * nothing, so treat it as an absent codec. */
                            int named = rs->codec[0] && strcmp(rs->codec, "UNKNOWN") != 0;
                            const char *fmt = rs->is_hls ? "HLS"
                                                         : (named ? rs->codec : "stream");
                            if (rs->bitrate > 0)
                                snprintf(kind, sizeof(kind), "%s / %d kbps", fmt, rs->bitrate);
                            else
                                snprintf(kind, sizeof(kind), "%s", fmt);
                            sl_add(&s_search, rs->name, rs->url, kind);
                        }
                        rb_result_free(&res);
                        mark_favourites(&s_search);
                        snprintf(s_search_label, sizeof(s_search_label), "Search: %s", query);
                        mode = LIST_SEARCH;
                        selected = 0;
                        list = list_for(mode);
                        if (s_search.count == 0)
                            snprintf(notice, sizeof(notice), "No stations found for \"%s\"", query);
                        else
                            notice[0] = '\0';
                    } else {
                        snprintf(notice, sizeof(notice), "Search failed: %s", err);
                    }
                }
            }

            if (pressed & SCE_CTRL_TRIANGLE) {
                if (upd.state == UPD_AVAILABLE) {
                    player_stop();
                    updater_install();
                } else {
                    updater_check();
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
        ui_draw(list, label_for(mode), selected, &st, &upd, notice);  /* swaps at vsync */
    }

    updater_shutdown();
    player_shutdown();
    ui_shutdown();
    sl_free(&s_builtin);
    sl_free(&s_favs);
    sl_free(&s_search);
    if (curl_ok) curl_global_cleanup();
    if (net_ok) {
        sceNetCtlTerm();
        sceNetTerm();
    }
    if (launch_updater)
        updater_launch();   /* does not return */
    sceKernelExitProcess(0);
    return 0;
}
