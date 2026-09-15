#include "fs_util.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void fs_mkdir_p(const char *path)
{
    char tmp[FS_PATH_LEN];
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
    DIR *d = opendir(path);
    if (!d) {
        unlink(path);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char child[FS_PATH_LEN];
        struct stat sb;
        if (snprintf(child, sizeof(child), "%s/%s", path, e->d_name) >= (int)sizeof(child))
            continue;
        if (stat(child, &sb) == 0 && S_ISDIR(sb.st_mode))
            fs_rm_tree(child);
        else
            unlink(child);
    }
    closedir(d);
    rmdir(path);
}
