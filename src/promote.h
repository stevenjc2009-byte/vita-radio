#ifndef VR_PROMOTE_H
#define VR_PROMOTE_H

/* Installs the extracted package at `dir` (must contain eboot.bin,
 * sce_sys/param.sfo and sce_sys/package/head.bin) with ScePromoterUtil.
 * Same load/promote/unload sequence VitaShell uses. Blocks until done.
 * Returns 0 or a negative SCE error code. Needs "Unsafe Homebrew". */
int promote_pkg(const char *dir);

#endif
