/* Vita Radio Updater (VRADUPDTR).
 * Launched by Vita Radio after it has unpacked a new release into
 * ux0:data/pkg. Closes Vita Radio, installs the package over it, writes the
 * result code for Vita Radio to show, then relaunches Vita Radio. No UI. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <psp2/appmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "pkg_meta.h"
#include "promote.h"

#define PKG_DIR       "ux0:data/pkg"
#define RESULT_PATH   "ux0:data/VitaRadio/update_result.txt"
#define MAIN_TITLE_ID "VRAD00001"
#define ERR_NOT_OURS  ((int)0x80010002)   /* package isn't Vita Radio */

static int package_is_vita_radio(void)
{
    uint8_t sfo[16 * 1024];
    char id[16];
    SceUID fd = sceIoOpen(PKG_DIR "/sce_sys/param.sfo", SCE_O_RDONLY, 0);
    if (fd < 0)
        return fd;
    int n = sceIoRead(fd, sfo, sizeof(sfo));
    sceIoClose(fd);
    if (n <= 0)
        return ERR_NOT_OURS;
    if (sfo_get_string(sfo, (size_t)n, "TITLE_ID", id, sizeof(id)) != 0 || strcmp(id, MAIN_TITLE_ID) != 0)
        return ERR_NOT_OURS;
    return 0;
}

static void write_result(int res)
{
    char line[16];
    int len = snprintf(line, sizeof(line), "0x%08X\n", (unsigned int)res);
    SceUID fd = sceIoOpen(RESULT_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0)
        return;
    sceIoWrite(fd, line, len);
    sceIoClose(fd);
}

int main(void)
{
    sceAppMgrDestroyOtherApp();
    sceKernelDelayThread(500 * 1000);

    int res = package_is_vita_radio();
    if (res == 0)
        res = promote_pkg(PKG_DIR);
    write_result(res);

    const char *uri = "psgm:play?titleid=" MAIN_TITLE_ID;
    sceAppMgrLaunchAppByUri(0xFFFFF, uri);
    sceKernelDelayThread(10 * 1000);
    sceAppMgrLaunchAppByUri(0xFFFFF, uri);
    sceKernelExitProcess(0);
    return 0;
}
