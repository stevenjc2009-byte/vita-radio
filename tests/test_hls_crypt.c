#include "hls_crypt.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0, g_pass = 0;

#define CHECK(name, cond) do { \
    if (cond) { printf("PASS %s\n", name); g_pass++; } \
    else { printf("FAIL %s (%s:%d)\n", name, __FILE__, __LINE__); g_fail++; } \
} while (0)

static int nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

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

/* NIST SP 800-38A F.2 key/IV, reused as the segment key. */
static const char SEG_KEY[] = "2b7e151628aed2a6abf7158809cf4f3c";
static const char SEG_IV[]  = "000102030405060708090a0b0c0d0e0f";

/* Ciphertexts derived from that key/IV with a scratch AES-128-CBC encryptor
 * which was itself validated by reproducing the published F.2.2 ciphertext.
 * SEG_CT_A holds 31 bytes of payload (one 0x01 pad byte); SEG_CT_B holds 32
 * bytes of payload followed by a whole 0x10 padding block. */
static const char SEG_CT_A[] = "485523cf1a5de2889b5506bb6db6260d"
                               "bd17360e46389b167266620368cf3be4";
static const char SEG_PLAIN_A[] = "vita-radio HLS segment payload!";

static const char SEG_CT_B[] = "64768548007aef9f3d258e5c34cdc21b"
                               "fde8bb0c7e4ea6e4b0a4d56da413c4a8"
                               "08342244766cd4bb9706f0d257363200";
static const char SEG_PLAIN_B[] = "0123456789abcdef0123456789abcdef";

/* The published F.2.2 ciphertext itself: it decrypts correctly but its
 * plaintext is not PKCS#7 padded, so a segment made of it must be refused. */
static const char NIST_CT[] = "7649abac8119b246cee98e9b12e9197d"
                              "5086cb9b507219ee95db113a917678b2"
                              "73bed6b8e3c1743b7116e69e22229516"
                              "3ff1caa1681fac09120eca307586e1a7";

static int iv_is(long long seq, const char *expect_hex)
{
    unsigned char iv[16], want[16];
    hls_iv_from_sequence(seq, iv);
    if (unhex(expect_hex, want) != 16)
        return 0;
    return memcmp(iv, want, 16) == 0;
}

int main(void)
{
    unsigned char key[16], iv[16], buf[64];
    char err[128];
    size_t len;

    unhex(SEG_KEY, key);
    unhex(SEG_IV, iv);

    /* ---- RFC 8216 s5.2 implied IV: sequence number, big-endian, in the
     * low 8 bytes, with bytes 0-7 zero ------------------------------------ */
    CHECK("iv_seq_0",  iv_is(0, "00000000000000000000000000000000"));
    CHECK("iv_seq_1",  iv_is(1, "00000000000000000000000000000001"));
    CHECK("iv_seq_2",  iv_is(2, "00000000000000000000000000000002"));
    CHECK("iv_seq_255", iv_is(255, "000000000000000000000000000000ff"));
    CHECK("iv_seq_256", iv_is(256, "00000000000000000000000000000100"));
    /* 279612681 = 0x10AA8D09 */
    CHECK("iv_seq_279612681", iv_is(279612681, "0000000000000000000000001" "0aa8d09"));
    /* 4294967295 = 0xFFFFFFFF: still inside the low four bytes */
    CHECK("iv_seq_2p32_minus_1", iv_is(4294967295LL, "000000000000000000000000ffffffff"));
    /* 5000000000 = 0x1_2A05F200: above 2^32, so byte 11 must be set */
    CHECK("iv_seq_5000000000", iv_is(5000000000LL, "0000000000000000000000012a05f200"));
    /* full eight-byte layout, every byte distinct */
    CHECK("iv_seq_full_8_bytes", iv_is(0x0102030405060708LL, "00000000000000000102030405060708"));

    /* byte-by-byte on one of them, spelled out rather than compared in bulk */
    {
        unsigned char x[16];
        int zeros = 1, i;
        hls_iv_from_sequence(5000000000LL, x);
        for (i = 0; i < 8; i++)
            if (x[i] != 0)
                zeros = 0;
        CHECK("iv_high_8_bytes_zero", zeros);
        CHECK("iv_byte8_is_zero", x[8] == 0x00);
        CHECK("iv_byte11_carries_bit32", x[11] == 0x01);
        CHECK("iv_byte12", x[12] == 0x2a);
        CHECK("iv_byte13", x[13] == 0x05);
        CHECK("iv_byte14", x[14] == 0xf2);
        CHECK("iv_byte15", x[15] == 0x00);
    }

    /* ---- round trip: 31-byte payload, one 0x01 pad byte ---------------- */
    len = unhex(SEG_CT_A, buf);
    CHECK("seg_a_fixture_len", len == 32);
    memset(err, 0, sizeof err);
    CHECK("seg_a_rc", hls_decrypt_segment(key, iv, buf, &len, err, sizeof err) == 0);
    CHECK("seg_a_len", len == 31);
    CHECK("seg_a_payload", len == 31 && memcmp(buf, SEG_PLAIN_A, 31) == 0);
    CHECK("seg_a_no_error_text", err[0] == '\0');

    /* ---- round trip: 32-byte payload, whole 0x10 padding block --------- */
    len = unhex(SEG_CT_B, buf);
    CHECK("seg_b_fixture_len", len == 48);
    memset(err, 0, sizeof err);
    CHECK("seg_b_rc", hls_decrypt_segment(key, iv, buf, &len, err, sizeof err) == 0);
    CHECK("seg_b_len", len == 32);
    CHECK("seg_b_payload", len == 32 && memcmp(buf, SEG_PLAIN_B, 32) == 0);

    /* ---- wrong key: must be reported, not silently accepted ------------ */
    {
        unsigned char bad[16];
        int all_rejected = 1, k;
        for (k = 0; k < 8; k++) {
            memcpy(bad, key, 16);
            bad[k] ^= 0x01;
            len = unhex(SEG_CT_A, buf);
            memset(err, 0, sizeof err);
            if (hls_decrypt_segment(bad, iv, buf, &len, err, sizeof err) != -1 ||
                err[0] == '\0' || len != 32)
                all_rejected = 0;
        }
        CHECK("wrong_key_rejected_with_reason", all_rejected);
    }

    /* ---- wrong IV: block 1 is corrupted but the padding still checks out,
     * so this one is caught only by the payload being wrong -------------- */
    {
        unsigned char biv[16];
        memcpy(biv, iv, 16);
        biv[0] ^= 0x01;
        len = unhex(SEG_CT_A, buf);
        memset(err, 0, sizeof err);
        CHECK("wrong_iv_rc", hls_decrypt_segment(key, biv, buf, &len, err, sizeof err) == 0);
        CHECK("wrong_iv_payload_differs", memcmp(buf, SEG_PLAIN_A, 16) != 0);
        CHECK("wrong_iv_tail_intact", memcmp(buf + 16, SEG_PLAIN_A + 16, 15) == 0);
    }

    /* ---- unpadded plaintext (the raw NIST vector) must be refused ------ */
    len = unhex(NIST_CT, buf);
    memset(err, 0, sizeof err);
    CHECK("unpadded_segment_rejected", hls_decrypt_segment(key, iv, buf, &len, err, sizeof err) == -1);
    CHECK("unpadded_segment_reason", strstr(err, "padding") != NULL);
    CHECK("unpadded_segment_len_untouched", len == 64);

    /* ---- length validation --------------------------------------------- */
    len = 0;
    memset(err, 0, sizeof err);
    CHECK("zero_len_rejected", hls_decrypt_segment(key, iv, buf, &len, err, sizeof err) == -1);
    CHECK("zero_len_reason", err[0] != '\0');

    len = 17;
    memset(err, 0, sizeof err);
    CHECK("len_17_rejected", hls_decrypt_segment(key, iv, buf, &len, err, sizeof err) == -1);
    CHECK("len_17_reason", err[0] != '\0');

    len = 31;
    memset(err, 0, sizeof err);
    CHECK("len_31_rejected", hls_decrypt_segment(key, iv, buf, &len, err, sizeof err) == -1);
    CHECK("len_31_reason", err[0] != '\0');

    len = 1;
    memset(err, 0, sizeof err);
    CHECK("len_1_rejected", hls_decrypt_segment(key, iv, buf, &len, err, sizeof err) == -1);

    /* ---- err handling: truncation and NULL must both be safe ----------- */
    {
        char small[8];
        memset(small, 0x7F, sizeof small);
        len = 17;
        CHECK("small_err_rejected", hls_decrypt_segment(key, iv, buf, &len, small, sizeof small) == -1);
        CHECK("small_err_terminated", memchr(small, '\0', sizeof small) != NULL);
        len = 17;
        CHECK("null_err_safe", hls_decrypt_segment(key, iv, buf, &len, NULL, 0) == -1);
    }

    printf("test_hls_crypt: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
