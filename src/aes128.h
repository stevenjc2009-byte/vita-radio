#ifndef VR_AES128_H
#define VR_AES128_H

#include <stddef.h>

/* Self-contained AES-128 decryption.
 *
 * Deliberately not mbedTLS: mbedTLS is vendored for the Vita build only, so a
 * host unit test could not link it, and this must be provable against the
 * FIPS-197 / NIST SP 800-38A known-answer vectors on the host. It is ~200
 * lines and used once per segment, so the size and speed are irrelevant. */

typedef struct {
    unsigned char rk[176];   /* 11 round keys, expanded for decryption */
} Aes128;

void aes128_init(Aes128 *a, const unsigned char key[16]);

/* Single block, ECB. The primitive the CBC mode below is built from. */
void aes128_decrypt_block(const Aes128 *a, const unsigned char in[16],
                          unsigned char out[16]);

/* CBC decrypt in place or out of place; len must be a non-zero multiple of 16.
 * Does NOT remove padding - see aes128_pkcs7_len. Returns 0, -1 on bad length. */
int  aes128_cbc_decrypt(const Aes128 *a, const unsigned char iv[16],
                        const unsigned char *in, size_t len, unsigned char *out);

/* Validated PKCS#7 length of a decrypted buffer: returns the payload length,
 * or -1 if the padding is malformed (which means the key or IV was wrong -
 * report it, never silently keep the garbage). */
long aes128_pkcs7_len(const unsigned char *buf, size_t len);

#endif
