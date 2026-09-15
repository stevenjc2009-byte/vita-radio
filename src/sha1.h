#ifndef VR_SHA1_H
#define VR_SHA1_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[5];
    uint64_t total;      /* bytes hashed so far */
    uint8_t  block[64];
    size_t   used;       /* bytes in block */
} Sha1;

void sha1_init(Sha1 *s);
void sha1_update(Sha1 *s, const uint8_t *data, size_t len);
void sha1_final(Sha1 *s, uint8_t out[20]);

#endif
