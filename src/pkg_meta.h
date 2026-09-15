#ifndef VR_PKG_META_H
#define VR_PKG_META_H

#include <stddef.h>
#include <stdint.h>

/* Reads string value `name` from a param.sfo image. 0 on success, -1 if the
 * image is malformed, the key is missing, or the value doesn't fit. */
int sfo_get_string(const uint8_t *sfo, size_t size, const char *name, char *out, size_t outsz);

/* 1 if id is exactly 9 characters of A-Z / 0-9 (the Vita TITLE_ID format). */
int title_id_valid(const char *id);

/* Builds sce_sys/package/head.bin for promoting an extracted homebrew package.
 * `tmpl` is the fake-PKG header template (assets/head.bin, as used by VitaShell).
 * Writes the content ID (or "EP9000-<title_id>_00-0000000000000000" when
 * content_id is empty/NULL) at 0x30, then the three fake-PKG HMACs.
 * `out` must hold `size` bytes. Returns 0, or -1 on a bad title ID or a
 * template whose offsets don't fit. */
int headbin_make(const uint8_t *tmpl, size_t size, const char *title_id,
                 const char *content_id, uint8_t *out);

#endif
