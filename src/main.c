#include <stdint.h>
#include <string.h>

#include <curl/curl.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>

#include "player.h"
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

static char s_net_pool[NET_POOL_SIZE];

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

int main(void)
{
    int net_ok = net_start() == 0;

    scePowerSetArmClockFrequency(444);

    int curl_ok = curl_global_init(CURL_GLOBAL_ALL) == CURLE_OK;
    int player_ok = player_init(USER_AGENT, CA_FILE) == 0;
    updater_init(USER_AGENT, CA_FILE);
    ui_init();

    int selected = 0;
    unsigned int prev_buttons = 0;
    SceUInt64 hold_since = 0, last_repeat = 0;
    PlayerStatus st;
    UpdateStatus upd;
    int running = 1, launch_updater = 0;

    while (running) {
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
        if (step != 0 && g_builtin_station_count > 0) {
            selected += step;
            if (selected < 0) selected = 0;
            if (selected >= g_builtin_station_count) selected = g_builtin_station_count - 1;
        }

        updater_get_status(&upd);
        int installing = upd.state == UPD_DOWNLOADING || upd.state == UPD_INSTALLING ||
                         upd.state == UPD_READY;

        if (!installing) {
            if (pressed & SCE_CTRL_CROSS) player_play(g_builtin_stations[selected].url);
            if (pressed & SCE_CTRL_CIRCLE) player_stop();
            if (pressed & SCE_CTRL_START) running = 0;
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
        ui_draw(g_builtin_stations, g_builtin_station_count, selected, &st, &upd);  /* swaps at vsync */
    }

    updater_shutdown();
    player_shutdown();
    ui_shutdown();
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
