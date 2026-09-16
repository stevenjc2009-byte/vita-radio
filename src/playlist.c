#include "playlist.h"

#include "url_util.h"

#include <string.h>

/* Longest entry we will carry out of a playlist. Entries longer than this are
 * treated as unusable rather than truncated - a truncated URL is worse than
 * no URL. */
#define PL_MAX_URL 2048

static int is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Narrows [*s, *e) past leading and trailing whitespace. */
static void trim(const char **s, const char **e)
{
    while (*s < *e && is_ws(**s))
        (*s)++;
    while (*e > *s && is_ws((*e)[-1]))
        (*e)--;
}

/* Next line in [*p, end): its span lands in [*ls, *le) with the terminator
 * stripped, *p moves past it. Returns 0 at end of input. */
static int next_line(const char **p, const char *end, const char **ls, const char **le)
{
    if (*p >= end)
        return 0;
    *ls = *p;
    while (*p < end && **p != '\n')
        (*p)++;
    *le = *p;
    if (*p < end)
        (*p)++;
    return 1;
}

static int span_equals_ci(const char *s, const char *e, const char *lit)
{
    size_t n = strlen(lit);
    size_t i;
    if ((size_t)(e - s) != n)
        return 0;
    for (i = 0; i < n; i++)
        if (lower(s[i]) != lit[i])
            return 0;
    return 1;
}

/* Parses one "FileN = value" line. Returns N (>= 0) and sets the value span,
 * or -1 if the line is not a FileN entry or has an empty value. */
static long pls_file_entry(const char *s, const char *e, const char **vs, const char **ve)
{
    static const char kw[] = "file";
    const char *p;
    long num = 0;
    int digits = 0;
    size_t i;

    for (i = 0; i < 4; i++)
        if (s + i >= e || lower(s[i]) != kw[i])
            return -1;

    p = s + 4;
    while (p < e && *p >= '0' && *p <= '9') {
        if (digits < 9)
            num = num * 10 + (*p - '0');
        digits++;
        p++;
    }
    if (digits == 0 || digits > 9)
        return -1;      /* "File=" or an absurd index: not an entry we handle */

    while (p < e && is_ws(*p))
        p++;
    if (p >= e || *p != '=')
        return -1;
    p++;

    *vs = p;
    *ve = e;
    trim(vs, ve);
    if (*vs >= *ve)
        return -1;      /* "File1=" with nothing after it */
    return num;
}

int playlist_first_url(const char *text, size_t len, const char *base_url,
                       char *out, size_t outsz)
{
    const char *p, *end;
    const char *ls, *le;
    const char *best_s = NULL, *best_e = NULL;
    long best_num = 0;
    int is_pls = 0;
    char ref[PL_MAX_URL];
    size_t n;

    if (!text || !out || outsz == 0)
        return -1;

    p = text;
    end = text + len;

    /* UTF-8 BOM. */
    if ((size_t)(end - p) >= 3 && (unsigned char)p[0] == 0xEF &&
        (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
        p += 3;

    /* A .pls announces itself with [playlist] on its first real line. */
    {
        const char *q = p;
        while (next_line(&q, end, &ls, &le)) {
            trim(&ls, &le);
            if (ls == le)
                continue;
            is_pls = span_equals_ci(ls, le, "[playlist]");
            break;
        }
    }

    if (is_pls) {
        while (next_line(&p, end, &ls, &le)) {
            const char *vs, *ve;
            long num;
            trim(&ls, &le);
            num = pls_file_entry(ls, le, &vs, &ve);
            if (num < 0)
                continue;
            if (!best_s || num < best_num) {
                best_num = num;
                best_s = vs;
                best_e = ve;
            }
        }
    } else {
        while (next_line(&p, end, &ls, &le)) {
            trim(&ls, &le);
            if (ls == le || *ls == '#')
                continue;
            best_s = ls;
            best_e = le;
            break;
        }
    }

    if (!best_s)
        return -1;

    n = (size_t)(best_e - best_s);
    if (n == 0 || n >= sizeof(ref))
        return -1;
    memcpy(ref, best_s, n);
    ref[n] = '\0';

    /* url_resolve passes absolutes through untouched and refuses to overflow
     * out, so a too-small buffer fails here rather than truncating. */
    return url_resolve(base_url, ref, out, outsz);
}
