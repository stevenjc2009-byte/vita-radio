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

/* ---- update handover token ------------------------------------------------
 * The updater title is a permanently installed LiveArea bubble that promotes
 * whatever package it is pointed at, so on its own it will happily install
 * something the user staged there themselves weeks later. Vita Radio writes one
 * of these next to the staged package and the updater title refuses to promote
 * without it, which ties the bubble to the one package this app staged. The
 * token is deleted before promoting, so it works exactly once.
 *
 * Both titles compile this struct, so its layout must not drift: the fields are
 * fixed-width, in size order, and pkg_meta.c asserts the total. */

#define UPDATE_TOKEN_MAGIC   0x56525455u   /* "VRTU" */
#define UPDATE_TOKEN_VERSION 1u
#define UPDATE_TOKEN_TAG_LEN 32

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint8_t  nonce[16];                  /* fresh per staging, so a stale token can't be reused */
    char     tag[UPDATE_TOKEN_TAG_LEN];  /* the release this package claims to be */
    uint8_t  sfo_sha1[20];               /* SHA-1 of the staged sce_sys/param.sfo */
} UpdateToken;

/* Fills t for the package whose param.sfo image is sfo[0..sfo_len). */
void update_token_build(UpdateToken *t, const char *tag, const uint8_t nonce[16],
                        const uint8_t *sfo, size_t sfo_len);

/* 0 if t is a whole, current token for exactly this param.sfo, else -1.
 * `len` is how many bytes were actually read, so a short file is refused. */
int update_token_check(const UpdateToken *t, size_t len, const uint8_t *sfo, size_t sfo_len);

#endif
