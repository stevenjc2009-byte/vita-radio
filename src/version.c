#include "version.h"

#include <ctype.h>
#include <string.h>

/* unsigned long is 32 bits on the Vita, so a part longer than this can't be
 * accumulated without wrapping. version_valid rejects such tags outright. */
#define MAX_PART_DIGITS 6
#define PART_TOO_BIG    1000000UL

static const char *skip_v(const char *s)
{
    if (s && (*s == 'v' || *s == 'V'))
        s++;
    return s;
}

int version_valid(const char *s)
{
    s = skip_v(s);
    if (!s || !isdigit((unsigned char)*s))
        return 0;
    for (int digits = 0; *s; s++) {
        if (isdigit((unsigned char)*s)) {
            if (++digits > MAX_PART_DIGITS)
                return 0;
        } else if (*s == '.') {
            digits = 0;
        } else {
            break;        /* a pre-release suffix; no numbers left to overflow */
        }
    }
    return 1;
}

/* Reads one numeric part and moves *s past it and its trailing '.'. Any other
 * separator ends the numeric parts and leaves *s on the suffix. */
static unsigned long next_part(const char **s)
{
    unsigned long v = 0;
    int digits = 0;
    while (isdigit((unsigned char)**s)) {
        v = ++digits > MAX_PART_DIGITS ? PART_TOO_BIG : v * 10 + (unsigned long)(**s - '0');
        (*s)++;
    }
    if (**s == '.')
        (*s)++;
    return v;
}

int version_compare(const char *a, const char *b)
{
    a = skip_v(a ? a : "");
    b = skip_v(b ? b : "");
    while (isdigit((unsigned char)*a) || isdigit((unsigned char)*b)) {
        unsigned long x = next_part(&a), y = next_part(&b);
        if (x != y)
            return x < y ? -1 : 1;
    }
    /* Whatever is left is a pre-release suffix. Semver sorts a release above
     * any pre-release of it, and orders two pre-releases by their text. */
    if (!*a && !*b)
        return 0;
    if (!*a)
        return 1;
    if (!*b)
        return -1;
    int d = strcmp(a, b);
    return d < 0 ? -1 : d > 0 ? 1 : 0;
}
