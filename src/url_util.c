#include "url_util.h"

#include <string.h>
#include <stdio.h>

#define MAX_PATH_LEN 1536
#define MAX_SEGS     128

static int is_scheme_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
}

int url_is_absolute(const char *s)
{
    if (!s || !*s)
        return 0;
    if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z')))
        return 0;   /* a scheme must start with a letter */
    const char *p = s + 1;
    while (*p && is_scheme_char(*p))
        p++;
    return *p == ':';
}

/* Collapse "." and ".." in place. Keeps a leading and trailing slash. */
static void normalize_path(char *path, size_t sz)
{
    const char *segs[MAX_SEGS];
    size_t      lens[MAX_SEGS];
    int         n = 0;
    char        tmp[MAX_PATH_LEN];

    const char *s = path;
    int lead = (*s == '/');
    if (lead)
        s++;

    int trailing = 0;
    while (*s) {
        const char *e = strchr(s, '/');
        size_t      l = e ? (size_t)(e - s) : strlen(s);

        if (l == 1 && s[0] == '.') {
            trailing = 1;
        } else if (l == 2 && s[0] == '.' && s[1] == '.') {
            if (n > 0)
                n--;
            trailing = 1;
        } else if (l > 0) {
            if (n < MAX_SEGS) {
                segs[n] = s;
                lens[n] = l;
                n++;
            }
            trailing = 0;
        }
        if (!e)
            break;
        s = e + 1;
        if (*s == '\0')
            trailing = 1;
    }

    size_t pos = 0;
    if (lead && pos + 1 < sizeof(tmp))
        tmp[pos++] = '/';
    for (int i = 0; i < n; i++) {
        if (i && pos + 1 < sizeof(tmp))
            tmp[pos++] = '/';
        if (pos + lens[i] + 2 >= sizeof(tmp))
            break;
        memcpy(tmp + pos, segs[i], lens[i]);
        pos += lens[i];
    }
    if (trailing && n > 0 && pos + 1 < sizeof(tmp))
        tmp[pos++] = '/';
    tmp[pos] = '\0';

    snprintf(path, sz, "%s", tmp);
}

int url_resolve(const char *base, const char *ref, char *out, size_t outsz)
{
    if (!ref || !*ref || !out || outsz == 0)
        return -1;

    if (url_is_absolute(ref)) {
        if (strlen(ref) >= outsz)
            return -1;
        snprintf(out, outsz, "%s", ref);
        return 0;
    }
    if (!base)
        return -1;

    /* Split the base into scheme, authority and path. */
    const char *sep = strstr(base, "://");
    if (!sep)
        return -1;
    size_t scheme_len = (size_t)(sep - base);

    const char *auth = sep + 3;
    const char *auth_end = auth;
    while (*auth_end && *auth_end != '/' && *auth_end != '?' && *auth_end != '#')
        auth_end++;
    size_t prefix_len = (size_t)(auth_end - base);   /* "scheme://authority" */

    /* A ?query or #fragment on ref rides along untouched. */
    const char *tail = strpbrk(ref, "?#");
    size_t ref_path_len = tail ? (size_t)(tail - ref) : strlen(ref);

    char path[MAX_PATH_LEN];

    if (ref[0] == '/' && ref[1] == '/') {
        /* scheme-relative: everything after "//" is a fresh authority+path. */
        char rest[MAX_PATH_LEN];
        if (ref_path_len >= sizeof(rest))
            return -1;
        memcpy(rest, ref, ref_path_len);
        rest[ref_path_len] = '\0';

        char *slash = strchr(rest + 2, '/');
        if (slash)
            normalize_path(slash, sizeof(rest) - (size_t)(slash - rest));

        int w = snprintf(out, outsz, "%.*s:%s%s",
                         (int)scheme_len, base, rest, tail ? tail : "");
        return (w < 0 || (size_t)w >= outsz) ? -1 : 0;
    }

    if (ref[0] == '/') {
        if (ref_path_len >= sizeof(path))
            return -1;
        memcpy(path, ref, ref_path_len);
        path[ref_path_len] = '\0';
    } else {
        /* Merge with the base's directory. */
        const char *bpath = auth_end;
        const char *bend = bpath;
        while (*bend && *bend != '?' && *bend != '#')
            bend++;

        const char *last = NULL;
        for (const char *p = bpath; p < bend; p++)
            if (*p == '/')
                last = p;

        size_t dir_len = last ? (size_t)(last - bpath) + 1 : 0;
        if (dir_len + ref_path_len + 2 >= sizeof(path))
            return -1;

        size_t pos = 0;
        if (dir_len == 0)
            path[pos++] = '/';
        else {
            memcpy(path, bpath, dir_len);
            pos = dir_len;
        }
        memcpy(path + pos, ref, ref_path_len);
        pos += ref_path_len;
        path[pos] = '\0';
    }

    normalize_path(path, sizeof(path));

    int w = snprintf(out, outsz, "%.*s%s%s",
                     (int)prefix_len, base, path, tail ? tail : "");
    return (w < 0 || (size_t)w >= outsz) ? -1 : 0;
}
