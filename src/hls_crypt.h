#ifndef VR_HLS_CRYPT_H
#define VR_HLS_CRYPT_H

#include <stddef.h>

/* EXT-X-KEY segment decryption.
 *
 * Deliberately contains no networking: fetching and caching the key bytes is
 * hls.c's job, so everything here is pure and the host suite can prove it
 * against known-answer vectors. */

/* Decrypt one whole segment in place: AES-128-CBC, PKCS#7 trimmed.
 * *len must be a non-zero multiple of 16 and is updated to the payload length.
 * Returns 0, or -1 with a reason in err - bad padding means the wrong key, and
 * is reported rather than silently keeping the garbage. */
int hls_decrypt_segment(const unsigned char key[16], const unsigned char iv[16],
                        unsigned char *buf, size_t *len, char *err, size_t errsz);

/* The IV implied when EXT-X-KEY carries no IV attribute: the segment's media
 * sequence number, big-endian, zero-padded into 16 bytes (RFC 8216 s5.2). */
void hls_iv_from_sequence(long long seq, unsigned char iv[16]);

#endif
