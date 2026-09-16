#include "fs_util.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void fs_mkdir_p(const char *path)
{
    char tmp[FS_PATH_LEN];
    /* snprintf("%s", NULL) is undefined - newlib does not print glibc's
     * "(null)" - so a NULL path has to stop here, not inside vsnprintf. */
    if (!path)
        return;
    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp))
        return;

    char *colon = strchr(tmp, ':');
    char *p = colon ? colon + 1 : tmp;
    if (*p == '/')
        p++;
    for (; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    mkdir(tmp, 0777);
}

void fs_rm_tree(const char *path)
{
    char **names = NULL;
    size_t count = 0, cap = 0, i;
    DIR *d;
    struct dirent *e;

    if (!path)
        return;

    d = opendir(path);
    if (!d) {
        unlink(path);
        return;
    }
    /* Read every name out first. Deleting entries while readdir is still
     * walking the same open DIR is unspecified, and sceIoDread skips entries
     * when it happens - the directory is then left part full, the rmdir below
     * fails, and nothing says so because this function returns void. */
    while ((e = readdir(d)) != NULL) {
        char *n;
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (count == cap) {
            size_t ncap = cap ? cap * 2 : 16;
            char **p = (char **)realloc(names, ncap * sizeof(*names));
            if (!p)
                break;              /* out of memory: delete what we collected */
            names = p;
            cap = ncap;
        }
        n = strdup(e->d_name);
        if (!n)
            break;
        names[count++] = n;
    }
    /* Closed before the recursion, so a deep tree does not hold one directory
     * handle open at every level on the way down. */
    closedir(d);

    for (i = 0; i < count; i++) {
        char child[FS_PATH_LEN];
        struct stat sb;
        if (snprintf(child, sizeof(child), "%s/%s", path, names[i]) < (int)sizeof(child)) {
            /* lstat, not stat: a symlink pointing at a directory is a file to
             * unlink here, not a tree to walk into. */
            if (lstat(child, &sb) == 0 && S_ISDIR(sb.st_mode))
                fs_rm_tree(child);
            else
                unlink(child);
        }
        free(names[i]);
    }
    free(names);
    rmdir(path);
}
