#ifndef VR_PLAYLIST_H
#define VR_PLAYLIST_H

#include <stddef.h>

/* .pls and plain .m3u resolution - the indirection a great many station
 * directory links use. Text in, one URL out; no I/O so it is host-testable. */

/* Reads the first playable entry:
 *   .pls   -> File1= (or the lowest-numbered FileN present)
 *   .m3u   -> the first non-comment, non-blank line
 * Relative entries are resolved against base_url. Skips entries that are
 * obviously not streams (a nested .pls/.m3u is returned as-is; the caller
 * decides how many hops to follow).
 * Returns 0 with out filled, -1 if nothing usable was found. */
int playlist_first_url(const char *text, size_t len, const char *base_url,
                       char *out, size_t outsz);

#endif
