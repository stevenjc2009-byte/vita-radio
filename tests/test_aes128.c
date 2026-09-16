#include "aes128.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0, g_pass = 0;

#define CHECK(name, cond) do { \
    if (cond) { printf("PASS %s\n", name); g_pass++; } \
    else { printf("FAIL %s (%s:%d)\n", name, __FILE__, __LINE__); g_fail++; } \
} while (0)

/* Known-answer vectors, typed from the standards themselves.
 *
 *   FIPS-197 Appendix B / C.1      - AES-128 single block
 *   NIST SP 800-38A F.2.1 / F.2.2  - CBC-AES128 over four blocks
 */
static const char FIPS_KEY[] = "000102030405060708090a0b0c0d0e0f";
static const char FIPS_PT[]  = "00112233445566778899aabbccddeeff";
static const char FIPS_CT[]  = "69c4e0d86a7b0430d8cdb78070b4c55a";

static const char NIST_KEY[] = "2b7e151628aed2a6abf7158809cf4f3c";
static const char NIST_IV[]  = "000102030405060708090a0b0c0d0e0f";
static const char NIST_CT[]  = "7649abac8119b246cee98e9b12e9197d"
                               "5086cb9b507219ee95db113a917678b2"
                               "73bed6b8e3c1743b7116e69e22229516"
                               "3ff1caa1681fac09120eca307586e1a7";
static const char NIST_PT[]  = "6bc1bee22e409f96e93d7e117393172a"
                               "ae2d8a571e03ac9c9eb76fac45af8e51"
                               "30c81c46a35ce411e5fbc1191a0a52ef"
                               "f69f2445df4f9b17ad2b417be66c3710";

static int nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode a hex literal into out; returns the byte count (0 if malformed). */
static size_t unhex(const char *hex, unsigned char *out)
{
    size_t n = strlen(hex) / 2, i;
    if (strlen(hex) % 2)
        return 0;
    for (i = 0; i < n; i++) {
        int hi = nibble(hex[2 * i]), lo = nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return 0;
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    return n;
}

int main(void)
{
    Aes128 a;
    unsigned char key[16], iv[16], ct[64], pt[64], got[64], buf[64];
    size_t i;

    /* ---- the vectors decoded cleanly (guards the rest of the file) ---- */
    CHECK("vector_lengths", unhex(FIPS_KEY, key) == 16 && unhex(NIST_CT, ct) == 64 &&
                            unhex(NIST_PT, pt) == 64);

    /* ---- FIPS-197 C.1: single block, ECB ------------------------------ */
    unhex(FIPS_KEY, key);
    unhex(FIPS_CT, ct);
    unhex(FIPS_PT, pt);
    aes128_init(&a, key);
    aes128_decrypt_block(&a, ct, got);
    CHECK("fips197_c1_decrypt_block", memcmp(got, pt, 16) == 0);

    /* Decrypting a different block must not give that plaintext: proves the
     * check above could actually go red. */
    {
        unsigned char other[16];
        memcpy(buf, ct, 16);
        buf[0] ^= 0x01;
        aes128_decrypt_block(&a, buf, other);
        CHECK("fips197_red_control_one_bit_changes_output", memcmp(other, pt, 16) != 0);
    }

    /* ---- NIST SP 800-38A F.2.2: CBC-AES128.Decrypt, four blocks ------- */
    unhex(NIST_KEY, key);
    unhex(NIST_IV, iv);
    unhex(NIST_CT, ct);
    unhex(NIST_PT, pt);
    aes128_init(&a, key);
    memset(got, 0, sizeof got);
    CHECK("nist_f22_cbc_returns_zero", aes128_cbc_decrypt(&a, iv, ct, 64, got) == 0);
    CHECK("nist_f22_cbc_block1", memcmp(got + 0,  pt + 0,  16) == 0);
    CHECK("nist_f22_cbc_block2", memcmp(got + 16, pt + 16, 16) == 0);
    CHECK("nist_f22_cbc_block3", memcmp(got + 32, pt + 32, 16) == 0);
    CHECK("nist_f22_cbc_block4", memcmp(got + 48, pt + 48, 16) == 0);
    CHECK("nist_f22_cbc_all_four", memcmp(got, pt, 64) == 0);

    /* Chaining: block 2 is only right because the previous *ciphertext*
     * block is XORed in. An ECB implementation passes block 1 and fails
     * blocks 2-4, so these two checks are what catch a chaining bug. */
    {
        unsigned char ecb[16];
        int differs, chained = 1;
        aes128_decrypt_block(&a, ct + 16, ecb);
        differs = memcmp(ecb, pt + 16, 16) != 0;
        for (i = 0; i < 16; i++)
            if ((unsigned char)(ecb[i] ^ ct[i]) != pt[16 + i])
                chained = 0;
        CHECK("cbc_red_control_ecb_block2_is_wrong", differs);
        CHECK("cbc_block2_xors_previous_ciphertext", chained);
    }

    /* A single-byte change in ciphertext block 1 must corrupt plaintext
     * block 2 as well - again, only true if the blocks are chained. */
    {
        memcpy(buf, ct, 64);
        buf[3] ^= 0x80;
        CHECK("cbc_tamper_rc", aes128_cbc_decrypt(&a, iv, buf, 64, got) == 0);
        CHECK("cbc_tamper_block1_corrupt", memcmp(got, pt, 16) != 0);
        CHECK("cbc_tamper_block2_corrupt", memcmp(got + 16, pt + 16, 16) != 0);
        CHECK("cbc_tamper_block3_intact", memcmp(got + 32, pt + 32, 16) == 0);
    }

    /* In place (buf used as both in and out), same answer. */
    memcpy(buf, ct, 64);
    CHECK("cbc_in_place_rc", aes128_cbc_decrypt(&a, iv, buf, 64, buf) == 0);
    CHECK("cbc_in_place_matches", memcmp(buf, pt, 64) == 0);

    /* Decrypting one block at a time, feeding the previous ciphertext as the
     * IV, must equal the whole-buffer answer. */
    {
        unsigned char chunk[16];
        int same = 1;
        for (i = 0; i < 4; i++) {
            const unsigned char *prev = (i == 0) ? iv : ct + (i - 1) * 16;
            aes128_cbc_decrypt(&a, prev, ct + i * 16, 16, chunk);
            if (memcmp(chunk, pt + i * 16, 16) != 0)
                same = 0;
        }
        CHECK("cbc_blockwise_matches_whole_buffer", same);
    }

    /* ---- CBC length validation ---------------------------------------- */
    CHECK("cbc_zero_len_rejected", aes128_cbc_decrypt(&a, iv, ct, 0, got) == -1);
    CHECK("cbc_len_1_rejected",    aes128_cbc_decrypt(&a, iv, ct, 1, got) == -1);
    CHECK("cbc_len_15_rejected",   aes128_cbc_decrypt(&a, iv, ct, 15, got) == -1);
    CHECK("cbc_len_17_rejected",   aes128_cbc_decrypt(&a, iv, ct, 17, got) == -1);
    CHECK("cbc_len_63_rejected",   aes128_cbc_decrypt(&a, iv, ct, 63, got) == -1);
    CHECK("cbc_len_16_accepted",   aes128_cbc_decrypt(&a, iv, ct, 16, got) == 0);

    /* ---- PKCS#7: every valid pad length ------------------------------- */
    {
        int all = 1, n;
        for (n = 1; n <= 16; n++) {
            memset(buf, 0xA5, 32);
            memset(buf + 32 - n, (unsigned char)n, (size_t)n);
            if (aes128_pkcs7_len(buf, 32) != (long)(32 - n))
                all = 0;
        }
        CHECK("pkcs7_all_pad_lengths_1_to_16", all);
    }
    memset(buf, 0x10, 16);
    CHECK("pkcs7_full_block_pad_empty_payload", aes128_pkcs7_len(buf, 16) == 0);

    /* ---- PKCS#7: malformed padding must be REJECTED -------------------- */
    memset(buf, 0xA5, 32); buf[31] = 0x00;
    CHECK("pkcs7_pad_byte_zero_rejected", aes128_pkcs7_len(buf, 32) == -1);
    memset(buf, 0xA5, 32); buf[31] = 17;
    CHECK("pkcs7_pad_byte_17_rejected", aes128_pkcs7_len(buf, 32) == -1);
    memset(buf, 0xA5, 32); buf[31] = 0xFF;
    CHECK("pkcs7_pad_byte_255_rejected", aes128_pkcs7_len(buf, 32) == -1);
    memset(buf, 0x10, 8);
    CHECK("pkcs7_pad_longer_than_buffer_rejected", aes128_pkcs7_len(buf, 8) == -1);
    memset(buf, 0x04, 4);
    CHECK("pkcs7_pad_equal_to_buffer_ok", aes128_pkcs7_len(buf, 4) == 0);
    memset(buf, 0xA5, 32); memset(buf + 28, 4, 4); buf[29] = 3;
    CHECK("pkcs7_pad_bytes_differ_rejected", aes128_pkcs7_len(buf, 32) == -1);
    memset(buf, 0xA5, 32); buf[30] = 2; buf[31] = 2;
    CHECK("pkcs7_two_byte_pad_ok", aes128_pkcs7_len(buf, 32) == 30);
    buf[30] = 0xA5;
    CHECK("pkcs7_first_pad_byte_wrong_rejected", aes128_pkcs7_len(buf, 32) == -1);
    CHECK("pkcs7_zero_len_rejected", aes128_pkcs7_len(buf, 0) == -1);

    /* ---- a wrong key must be reported, never silently accepted --------
     * FIPS-197 gives D(FIPS_CT) = FIPS_PT, so CBC-decrypting that one block
     * under IV = FIPS_PT ^ want yields exactly `want`. That lets a properly
     * padded buffer be built from the published vector alone. */
    {
        unsigned char want[16], fkey[16], fct[16], fpt[16], wrong[16];
        int k, all_rejected = 1;

        memset(want, 0x0b, 16);
        memcpy(want, "hello", 5);          /* "hello" + 11 x 0x0b */
        unhex(FIPS_KEY, fkey);
        unhex(FIPS_CT, fct);
        unhex(FIPS_PT, fpt);
        for (i = 0; i < 16; i++)
            iv[i] = (unsigned char)(fpt[i] ^ want[i]);

        aes128_init(&a, fkey);
        CHECK("derived_rc", aes128_cbc_decrypt(&a, iv, fct, 16, got) == 0);
        CHECK("derived_iv_gives_padded_plaintext", memcmp(got, want, 16) == 0);
        CHECK("pkcs7_of_derived_payload", aes128_pkcs7_len(got, 16) == 5);

        for (k = 0; k < 8; k++) {
            Aes128 w;
            memcpy(wrong, fkey, 16);
            wrong[k] ^= 0x01;
            aes128_init(&w, wrong);
            aes128_cbc_decrypt(&w, iv, fct, 16, buf);
            if (memcmp(buf, want, 16) == 0 || aes128_pkcs7_len(buf, 16) != -1)
                all_rejected = 0;
        }
        CHECK("wrong_key_rejected_by_padding", all_rejected);
    }

    printf("test_aes128: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
