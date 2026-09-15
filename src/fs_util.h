#ifndef VR_FS_UTIL_H
#define VR_FS_UTIL_H

#define FS_PATH_LEN 512

/* Creates every missing directory in path. Handles a "ux0:" device prefix.
 * Errors are not reported; the caller's next open/write reports them. */
void fs_mkdir_p(const char *path);

/* Deletes a file, or a directory and everything under it. */
void fs_rm_tree(const char *path);

#endif
