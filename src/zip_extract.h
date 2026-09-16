#ifndef VR_ZIP_EXTRACT_H
#define VR_ZIP_EXTRACT_H

/* Minimal zip reader for VPKs: stored and deflated entries, no zip64, no
 * encryption. Uses zlib for inflate and CRC-32. */

enum {
    ZIP_OK              = 0,
    ZIP_ERR_OPEN        = -1,
    ZIP_ERR_READ        = -2,
    ZIP_ERR_FORMAT      = -3,
    ZIP_ERR_UNSUPPORTED = -4,
    ZIP_ERR_UNSAFE_NAME = -5,
    ZIP_ERR_WRITE       = -6,
    ZIP_ERR_DATA        = -7,
    ZIP_ERR_CRC         = -8,
    ZIP_ERR_ABORTED     = -9,
    ZIP_ERR_NOMEM       = -10,
    ZIP_ERR_EMPTY       = -11,
    ZIP_ERR_TOO_BIG     = -12   /* declared output exceeds the extraction cap */
};

/* Called after every chunk; return nonzero to abort. May be NULL. */
typedef int (*ZipTick)(void *user);

/* Extracts every entry of zip_path under dir (created as needed).
 * Rejects absolute names, device names and ".." components before writing
 * anything for that entry. Returns ZIP_OK or a ZIP_ERR_* code. */
int zip_extract(const char *zip_path, const char *dir, ZipTick tick, void *user);

/* 1 if an entry name is a safe relative path. */
int zip_name_safe(const char *name);

const char *zip_strerror(int err);

#endif
