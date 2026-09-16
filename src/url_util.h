#ifndef VR_URL_UTIL_H
#define VR_URL_UTIL_H

#include <stddef.h>

/* Relative URL resolution, RFC 3986 section 5.
 *
 * Its own module rather than part of http_get.c on purpose: m3u8.c and
 * playlist.c both need it and both have host unit tests, and http_get.c pulls
 * in libcurl, which the host suite cannot link. Pure string work, no I/O. */

/* Resolves ref against base into out.
 *   absolute        "http://h/x"  -> used as is
 *   scheme-relative "//h/x"       -> base's scheme + ref
 *   root-relative   "/x"          -> base's scheme://authority + ref
 *   relative        "seg.ts"      -> base's directory + ref
 * "." and ".." are removed from the resulting path; any ?query or #fragment on
 * ref is preserved and never normalised.
 * Returns 0 on success, -1 if base has no scheme://authority, ref is empty, or
 * the result would not fit in outsz. */
int url_resolve(const char *base, const char *ref, char *out, size_t outsz);

/* 1 if s begins with a scheme ("http:", "https:", ...). */
int url_is_absolute(const char *s);

#endif
