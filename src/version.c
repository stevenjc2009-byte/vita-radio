#include "version.h"

#include <ctype.h>

static const char *skip_v(const char *s)
{
    if (s && (*s == 'v' || *s == 'V'))
        s++;
    return s;
}

int version_valid(const char *s)
{
    s = skip_v(s);
    return s && isdigit((unsigned char)*s);
}

/* Reads one numeric part and moves *s past it and its trailing '.'. */
static unsigned long next_part(const char **s)
{
    unsigned long v = 0;
    while (isdigit((unsigned char)**s)) {
        v = v * 10 + (unsigned long)(**s - '0');
        (*s)++;
    }
    if (**s == '.')
        (*s)++;
    else
        while (**s)   /* anything else (e.g. "-beta") ends the version */
            (*s)++;
    return v;
}

int version_compare(const char *a, const char *b)
{
    a = skip_v(a ? a : "");
    b = skip_v(b ? b : "");
    while (*a || *b) {
        unsigned long x = next_part(&a), y = next_part(&b);
        if (x != y)
            return x < y ? -1 : 1;
    }
    return 0;
}
