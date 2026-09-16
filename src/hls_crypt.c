#include "hls_crypt.h"

#include "aes128.h"

#include <stdio.h>
#include <string.h>

static void set_err(char *err, size_t errsz, const char *msg)
{
    if (err == NULL || errsz == 0)
        return;
    snprintf(err, errsz, "%s", msg);
}

int hls_decrypt_segment(const unsigned char key[16], const unsigned char iv[16],
                        unsigned char *buf, size_t *len, char *err, size_t errsz)
{
    Aes128 a;
    long payload;

    if (key == NULL || iv == NULL || buf == NULL || len == NULL) {
        set_err(err, errsz, "segment decrypt: missing key, IV or buffer");
        return -1;
    }
    if (*len == 0) {
        set_err(err, errsz, "segment is empty");
        return -1;
    }
    if (*len % 16 != 0) {
        if (err != NULL && errsz != 0)
            snprintf(err, errsz, "segment length %lu is not a multiple of 16",
                     (unsigned long)*len);
        return -1;
    }

    aes128_init(&a, key);
    if (aes128_cbc_decrypt(&a, iv, buf, *len, buf) != 0) {
        set_err(err, errsz, "segment decrypt: CBC rejected the buffer");
        return -1;
    }

    /* Bad padding means the key or IV was wrong. Report it: keeping the
     * garbage would only surface later as a burst of decoder noise. */
    payload = aes128_pkcs7_len(buf, *len);
    if (payload < 0) {
        set_err(err, errsz, "bad PKCS#7 padding (wrong key or IV?)");
        return -1;
    }

    *len = (size_t)payload;
    return 0;
}

void hls_iv_from_sequence(long long seq, unsigned char iv[16])
{
    unsigned long long s = (unsigned long long)seq;
    int i;

    memset(iv, 0, 16);
    for (i = 15; i >= 8; i--) {
        iv[i] = (unsigned char)(s & 0xFF);
        s >>= 8;
    }
}
