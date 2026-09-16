#include "favourites.h"
#include "station_list.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#define VR_MKDIR(p) _mkdir(p)
#define VR_FSYNC(fd) _commit(fd)
#else
#include <unistd.h>
#define VR_MKDIR(p) mkdir((p), 0777)
#define VR_FSYNC(fd) fsync(fd)
#endif

/* Deliberately no psp2/ headers here: the host unit tests link this file, and
 * newlib's mkdir/fopen/rename already reach ux0: on the Vita. */

/* ---- saving ------------------------------------------------------------ */

/* Tabs and newlines are the record structure, so a field containing one would
 * silently split a row. They are replaced with spaces rather than escaped: an
 * escape scheme needs an unescaper on load, and an unescaper plus a half-written
 * final line is exactly the case the format exists to survive. A lossy space is
 * a station name that looks slightly odd; an escape left dangling by a power cut
 * is a parser that has to guess. */
static void fput_field(const char *s, FILE *f)
{
    if (!s)
        return;
    for (; *s; s++) {
        char c = *s;
        if (c == '\t' || c == '\n' || c == '\r')
            c = ' ';
        fputc((unsigned char)c, f);
    }
}

int fav_save(const StationList *l, const char *path)
{
    char *tmp;
    size_t plen;
    FILE *f;
    int i, bad;

    if (!l || !path)
        return -1;

    plen = strlen(path);
    tmp = (char *)malloc(plen + 5);          /* ".tmp" + NUL */
    if (!tmp)
        return -1;
    memcpy(tmp, path, plen);
    memcpy(tmp + plen, ".tmp", 5);

    f = fopen(tmp, "wb");
    if (!f) {
        free(tmp);
        return -1;
    }
    for (i = 0; i < l->count; i++) {
        if (!l->items[i].is_fav)
            continue;
        fput_field(l->items[i].name, f);
        fputc('\t', f);
        fput_field(l->items[i].url, f);
        fputc('\t', f);
        fput_field(l->items[i].kind, f);
        fputc('\n', f);
    }
    bad = ferror(f);
    /* Push the bytes to the card before the rename. FAT can commit the
     * directory entry ahead of the data, so a power cut between the two would
     * otherwise leave a zero-length favourites file where a complete one was. */
    if (!bad && (fflush(f) != 0 || VR_FSYNC(fileno(f)) != 0))
        bad = 1;
    if (fclose(f) != 0)
        bad = 1;
    if (bad) {
        remove(tmp);
        free(tmp);
        return -1;
    }

    /* Rename over the target so an interrupted save cannot destroy the file
     * that is already there. POSIX replaces atomically; newlib's rename refuses
     * an existing target - so does sceIoRename under it - which makes this
     * fallback the normal path on the Vita, not a rare one. Removing the old
     * file there would open a window on every single save in which there are no
     * favourites at all, so it steps aside as ".bak" instead and is only
     * deleted once the new file is in place. */
    if (rename(tmp, path) != 0) {
        char *bak = (char *)malloc(plen + 5);    /* ".bak" + NUL */
        int moved;

        if (!bak) {
            free(tmp);
            return -1;
        }
        memcpy(bak, path, plen);
        memcpy(bak + plen, ".bak", 5);

        remove(bak);                             /* left by an earlier failure */
        moved = (rename(path, bak) == 0);        /* fails if there was no file */
        if (rename(tmp, path) != 0) {
            if (moved)
                rename(bak, path);               /* put the old one back */
            /* tmp is a complete, good file. It is deliberately left where it
             * is: deleting it here would throw away the only copy of the new
             * favourites as well as whatever the failed rename cost us. */
            free(bak);
            free(tmp);
            return -1;
        }
        if (moved)
            remove(bak);
        free(bak);
    }
    free(tmp);
    return 0;
}

/* ---- loading ----------------------------------------------------------- */

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} LineBuf;

static int lb_push(LineBuf *lb, char c)
{
    if (lb->len + 1 >= lb->cap) {
        size_t cap = lb->cap ? lb->cap * 2 : 256;
        char *p = (char *)realloc(lb->buf, cap);
        if (!p)
            return -1;
        lb->buf = p;
        lb->cap = cap;
    }
    lb->buf[lb->len++] = c;
    return 0;
}

/* Reads one line. Returns 1 if bytes were read, 0 at clean EOF, -1 on error.
 * *terminated says whether the line ended with '\n' - an unterminated final
 * line is a half-written record and gets dropped by the caller. */
static int lb_read_line(FILE *f, LineBuf *lb, int *terminated)
{
    int c;

    lb->len = 0;
    *terminated = 0;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') {
            *terminated = 1;
            break;
        }
        if (lb_push(lb, (char)c) != 0)
            return -1;
    }
    if (!*terminated && lb->len == 0)
        return ferror(f) ? -1 : 0;
    if (lb->len > 0 && lb->buf[lb->len - 1] == '\r')
        lb->len--;                            /* tolerate CRLF files */
    if (!lb->buf) {
        /* An empty line pushed nothing, so on the very first line of the file
         * there is no buffer yet to terminate. One push allocates the block;
         * the byte it wrote is then discarded. */
        if (lb_push(lb, '\0') != 0)
            return -1;
        lb->len = 0;
    }
    lb->buf[lb->len] = '\0';
    return 1;
}

int fav_load(StationList *l, const char *path)
{
    FILE *f;
    LineBuf lb;
    int rc = 0, terminated, r;

    if (!l || !path)
        return -1;

    f = fopen(path, "rb");
    if (!f)
        return (errno == ENOENT) ? 0 : -1;    /* no favourites yet is normal */

    lb.buf = NULL;
    lb.len = 0;
    lb.cap = 0;

    while ((r = lb_read_line(f, &lb, &terminated)) == 1) {
        char *t1, *t2;
        int idx;

        if (!terminated)
            break;                            /* truncated final record */
        if (lb.len == 0)
            continue;
        t1 = strchr(lb.buf, '\t');
        if (!t1)
            continue;                         /* malformed: skip the row */
        t2 = strchr(t1 + 1, '\t');
        if (!t2)
            continue;
        *t1 = '\0';
        *t2 = '\0';
        idx = sl_add(l, lb.buf, t1 + 1, t2 + 1);
        if (idx < 0) {
            rc = -1;
            break;
        }
        l->items[idx].is_fav = 1;
    }
    if (r < 0 || ferror(f))
        rc = -1;

    free(lb.buf);
    fclose(f);
    return rc;
}

/* ---- directory --------------------------------------------------------- */

int fav_ensure_dir(void)
{
    char path[256];
    size_t i, n;

    n = strlen(VR_FAV_DIR);
    if (n + 1 > sizeof path)
        return -1;
    memcpy(path, VR_FAV_DIR, n + 1);

    /* Create each component in turn. The "ux0:" device prefix is not a
     * directory, so skip past the colon before the first separator. */
    i = 0;
    {
        const char *colon = strchr(path, ':');
        if (colon)
            i = (size_t)(colon - path) + 1;
    }
    for (; i <= n; i++) {
        if (path[i] != '/' && path[i] != '\0')
            continue;
        if (i == 0)
            continue;
        {
            char saved = path[i];
            path[i] = '\0';
            if (VR_MKDIR(path) != 0 && errno != EEXIST) {
                path[i] = saved;
                return -1;
            }
            path[i] = saved;
        }
    }
    return 0;
}
