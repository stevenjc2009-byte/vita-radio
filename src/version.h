#ifndef VR_VERSION_H
#define VR_VERSION_H

#define VR_VERSION "3.0.0"

/* Compares dotted versions ("1.0.10", "v1.2"); a leading v/V is skipped and
 * missing parts count as 0. Anything after the numbers is a pre-release suffix:
 * "2.0.0-rc1" sorts below "2.0.0" and below "2.0.0-rc2".
 * Returns <0, 0 or >0 like strcmp. */
int version_compare(const char *a, const char *b);

/* 1 if s starts (after an optional v/V) with a digit and no numeric part is
 * long enough to overflow a 32-bit unsigned long, else 0. */
int version_valid(const char *s);

#endif
