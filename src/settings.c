#include "settings.h"
#include "theme.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#define VR_FSYNC(fd) _commit(fd)
#else
#include <unistd.h>
#define VR_FSYNC(fd) fsync(fd)
#endif

/* Same stance as favourites.c: no psp2/ headers, so the host unit tests link
 * this file directly and newlib's fopen/rename already reach ux0: on the Vita.
 *
 * The valid theme range comes from theme.h's VR_THEME_COUNT macro rather than
 * from theme_count(). Both say 4; the macro is used because it costs no link
 * dependency, which keeps test_settings a single-module suite and keeps a
 * settings save off the path of anything that touches the UI's theme table. */

/* ---- saving ------------------------------------------------------------ */

int settings_save(const Settings *s, const char *path)
{
    char *tmp;
    size_t plen;
    FILE *f;
    int bad;

    if (!s || !path)
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
    /* No field sanitising here, unlike favourites.c: every value written is an
     * int printed by us, so none of them can contain a tab or a newline and
     * split its own record. */
    fprintf(f, "theme\t%d\n", s->theme);

    bad = ferror(f);
    /* Push the bytes to the card before the rename, for the same reason as in
     * favourites.c: FAT can commit the directory entry ahead of the data, so a
     * power cut between the two would leave a zero-length settings file where
     * a complete one was. */
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
     * already there. POSIX replaces atomically; newlib's rename refuses an
     * existing target - so does sceIoRename under it - which makes the
     * fallback below the normal path on the Vita, not a rare one. The old file
     * steps aside as ".bak" rather than being deleted, so there is no window
     * in which the settings are simply absent. */
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
            /* tmp is a complete, good file and is deliberately left in place -
             * it is the only copy of the new settings. */
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
        size_t cap = lb->cap ? lb->cap * 2 : 128;
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
 * line is a half-written record and gets dropped by the caller. The buffer
 * grows, so there is no length a line can reach that overruns it. */
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

/* Parses a whole value as a theme index. Returns 0 and fills *out only if the
 * text is an integer and nothing else, and lands inside the valid range.
 *
 * strtol on its own is too permissive for this: it skips leading whitespace
 * and stops happily at the first non-digit, so " 2abc" would read as 2. The
 * value here is machine-written, so anything that is not exactly an integer is
 * a corrupt line and must leave the field at its default rather than be
 * guessed at. */
static int parse_index(const char *v, int *out)
{
    long n;
    char *end;

    /* Rejects an empty value, a leading space and a leading '+' up front, so
     * only '-' and a digit can begin a value strtol is allowed to see. */
    if (*v != '-' && (*v < '0' || *v > '9'))
        return -1;

    errno = 0;
    n = strtol(v, &end, 10);
    if (end == v || *end != '\0')          /* nothing parsed, or trailing text */
        return -1;
    if (errno == ERANGE || n < 0 || n >= VR_THEME_COUNT)
        return -1;

    *out = (int)n;
    return 0;
}

void settings_defaults(Settings *s)
{
    if (!s)
        return;
    s->theme = 0;              /* theme.c: index 0 is the fresh-install theme */
}

int settings_load(Settings *s, const char *path)
{
    FILE *f;
    LineBuf lb;
    int rc = 0, terminated, r;

    if (!s || !path)
        return -1;

    f = fopen(path, "rb");
    if (!f)
        return (errno == ENOENT) ? 0 : -1;    /* no settings yet is normal */

    lb.buf = NULL;
    lb.len = 0;
    lb.cap = 0;

    while ((r = lb_read_line(f, &lb, &terminated)) == 1) {
        char *tab;
        int v;

        if (!terminated)
            break;                            /* truncated final record */
        if (lb.len == 0)
            continue;
        tab = strchr(lb.buf, '\t');
        if (!tab)
            continue;                         /* malformed: skip the line */
        *tab = '\0';

        /* An unknown key is skipped rather than rejected, so a file written by
         * a newer build still loads the keys this one understands. A value
         * that does not parse leaves the field alone for the same reason: the
         * UI must never be handed a theme index it cannot draw. */
        if (strcmp(lb.buf, "theme") == 0 && parse_index(tab + 1, &v) == 0)
            s->theme = v;
    }
    if (r < 0 || ferror(f))
        rc = -1;

    free(lb.buf);
    fclose(f);
    return rc;
}
