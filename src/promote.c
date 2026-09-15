#include "promote.h"

#include <stdint.h>

#include <psp2/promoterutil.h>
#include <psp2/sysmodule.h>

/* ScePromoterUtil needs ScePaf loaded first, with these load arguments. */
static int load_paf(void)
{
    static uint32_t argp[] = {0x180000, 0xFFFFFFFF, 0xFFFFFFFF, 1, 0xFFFFFFFF, 0xFFFFFFFF};
    int result = -1;
    SceSysmoduleOpt opt = {sizeof(opt), &result, {-1, -1}};
    int res = sceSysmoduleLoadModuleInternalWithArg(SCE_SYSMODULE_INTERNAL_PAF,
                                                    sizeof(argp), argp, &opt);
    return res < 0 ? res : 0;
}

static void unload_paf(void)
{
    SceSysmoduleOpt opt = {0, 0, {0, 0}};
    sceSysmoduleUnloadModuleInternalWithArg(SCE_SYSMODULE_INTERNAL_PAF, 0, 0, &opt);
}

int promote_pkg(const char *dir)
{
    int res = load_paf();
    if (res < 0)
        return res;

    res = sceSysmoduleLoadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
    if (res >= 0) {
        res = scePromoterUtilityInit();
        if (res >= 0) {
            res = scePromoterUtilityPromotePkgWithRif(dir, 1);
            scePromoterUtilityExit();
        }
        sceSysmoduleUnloadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
    }
    unload_paf();
    return res < 0 ? res : 0;
}
