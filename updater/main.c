/* Vita Radio Updater (VRADUPDTR).
 * Launched by Vita Radio after it has unpacked a new release into PKG_DIR.
 * Closes Vita Radio, installs the package over it, writes the result code for
 * Vita Radio to show, then relaunches Vita Radio. No UI.
 *
 * This bubble is installed permanently and stays on the LiveArea between
 * updates, so it must not promote whatever happens to be sitting in PKG_DIR:
 * it installs only a package Vita Radio staged, vouched for by the one-shot
 * token beside it, and deletes that token before promoting. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <psp2/appmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "pkg_meta.h"
#include "promote.h"

#define DATA_DIR      "ux0:data/VitaRadio"
#define PKG_DIR       DATA_DIR "/pkg"
#define TOKEN_PATH    DATA_DIR "/update_token.bin"
#define RESULT_PATH   DATA_DIR "/update_result.txt"
#define MAIN_TITLE_ID "VRAD00001"
#define ERR_NOT_OURS   ((int)0x80010002)   /* package isn't Vita Radio */
#define ERR_NO_TOKEN   ((int)0x80010003)   /* nobody asked us to install this */
#define ERR_INCOMPLETE ((int)0x80010004)   /* package has no usable eboot.bin */

/* Reads a whole small file. Returns bytes read, or <0. */
static int read_file(const char *path, uint8_t *buf, unsigned int cap)
{
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0)
        return fd;
    int n = sceIoRead(fd, buf, cap);
    sceIoClose(fd);
    return n;
}

/* 0 if PKG_DIR holds a Vita Radio package this copy of Vita Radio staged. */
static int package_is_ours(void)
{
    uint8_t sfo[16 * 1024];
    UpdateToken token;
    SceIoStat st;
    char id[16];

    /* An eboot.bin that is missing or empty installs cleanly and then won't
     * launch, leaving no way back to the updater. Refuse it here too. */
    if (sceIoGetstat(PKG_DIR "/eboot.bin", &st) < 0 || st.st_size <= 0)
        return ERR_INCOMPLETE;

    int n = read_file(PKG_DIR "/sce_sys/param.sfo", sfo, sizeof(sfo));
    if (n <= 0)
        return ERR_INCOMPLETE;
    if (sfo_get_string(sfo, (size_t)n, "TITLE_ID", id, sizeof(id)) != 0 ||
        strcmp(id, MAIN_TITLE_ID) != 0)
        return ERR_NOT_OURS;

    /* The token ties this bubble to the staging step that created it. Without
     * it we would promote anything the user left in PKG_DIR, months later. */
    int t = read_file(TOKEN_PATH, (uint8_t *)&token, sizeof(token));
    if (t < 0 || update_token_check(&token, (size_t)t, sfo, (size_t)n) != 0)
        return ERR_NO_TOKEN;
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

    int res = package_is_ours();
    if (res == 0) {
        /* One shot: spend the token before installing, so a crash or a power
         * cut mid-promote can't leave it usable for a second, unasked run. */
        sceIoRemove(TOKEN_PATH);
        res = promote_pkg(PKG_DIR);
    }
    write_result(res);

    const char *uri = "psgm:play?titleid=" MAIN_TITLE_ID;
    sceAppMgrLaunchAppByUri(0xFFFFF, uri);
    sceKernelDelayThread(10 * 1000);
    sceAppMgrLaunchAppByUri(0xFFFFF, uri);
    sceKernelExitProcess(0);
    return 0;
}
