#include "icy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0, g_pass = 0;

#define CHECK(name, cond) do { \
    if (cond) { printf("PASS %s\n", name); g_pass++; } \
    else { printf("FAIL %s (%s:%d)\n", name, __FILE__, __LINE__); g_fail++; } \
} while (0)

/* ---- synthetic stream ---------------------------------------------- */

#define METAINT 16
#define MAXSTREAM 16384

static unsigned char stream[MAXSTREAM];
static size_t        stream_len;
static unsigned char audio_expect[MAXSTREAM];
static size_t        audio_expect_len;

static void put_audio(size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char b = (unsigned char)(audio_expect_len * 7 + 3);
        audio_expect[audio_expect_len++] = b;
        stream[stream_len++] = b;
    }
}

/* Writes a length byte + a NUL padded metadata block. blocks = 0 -> empty. */
static void put_meta(const char *text, size_t blocks)
{
    size_t sz = blocks * 16, tl = text ? strlen(text) : 0;
    stream[stream_len++] = (unsigned char)blocks;
    memset(stream + stream_len, 0, sz);
    if (text)
        memcpy(stream + stream_len, text, tl < sz ? tl : sz);
    stream_len += sz;
}

static size_t blocks_for(const char *t)
{
    return (strlen(t) + 15) / 16;
}

static const char *expected_titles[] = {
    "Song A", "Guns N' Roses - Don't Cry", "Big", "Song B"
};
#define N_EXPECTED_TITLES 4

static void build_stream(void)
{
    static char big[4081];
    const char *a = "StreamTitle='Song A';StreamUrl='';";
    const char *a2 = "StreamTitle='Song A';";
    const char *g = "StreamTitle='Guns N' Roses - Don't Cry';StreamUrl='';";
    const char *e = "StreamTitle='';";
    const char *b = "StreamTitle='Song B';";
    const char *head = "StreamTitle='Big';StreamUrl='";
    size_t hl = strlen(head);

    memcpy(big, head, hl);
    memset(big + hl, 'x', 4080 - hl - 2);
    memcpy(big + 4078, "';", 2);
    big[4080] = '\0';

    stream_len = audio_expect_len = 0;
    put_audio(METAINT); put_meta(a, blocks_for(a));
    put_audio(METAINT); put_meta(NULL, 0);
    put_audio(METAINT); put_meta(a2, blocks_for(a2));   /* same title: no callback */
    put_audio(METAINT); put_meta(g, blocks_for(g));
    put_audio(METAINT); put_meta(NULL, 0);
    put_audio(METAINT); put_meta(big, 255);              /* max-size block */
    put_audio(METAINT); put_meta(e, blocks_for(e));      /* empty title: no callback */
    put_audio(METAINT); put_meta(b, blocks_for(b));
    put_audio(METAINT); put_meta(NULL, 0);
    put_audio(10);                                       /* partial trailing audio */
}

/* ---- collector ----------------------------------------------------- */

typedef struct {
    unsigned char audio[MAXSTREAM];
    size_t        audio_len;
    char          titles[16][256];
    int           n_titles;
    int           audio_calls;
    int           stop_after;    /* >0: return -1 on this call number */
} Sink;

static int sink_audio(void *user, const unsigned char *d, size_t n)
{
    Sink *s = user;
    s->audio_calls++;
    if (s->audio_len + n <= sizeof(s->audio))
        memcpy(s->audio + s->audio_len, d, n);
    s->audio_len += n;
    if (s->stop_after > 0 && s->audio_calls >= s->stop_after)
        return -1;
    return 0;
}

static void sink_title(void *user, const char *t)
{
    Sink *s = user;
    if (s->n_titles < 16)
        snprintf(s->titles[s->n_titles], 256, "%s", t);
    s->n_titles++;
}

static Sink *sink_new(void)
{
    return calloc(1, sizeof(Sink));
}

static int audio_matches(const Sink *s, const unsigned char *exp, size_t exp_len)
{
    return s->audio_len == exp_len && memcmp(s->audio, exp, exp_len) == 0;
}

static int titles_match(const Sink *s)
{
    int i;
    if (s->n_titles != N_EXPECTED_TITLES)
        return 0;
    for (i = 0; i < N_EXPECTED_TITLES; i++)
        if (strcmp(s->titles[i], expected_titles[i]) != 0)
            return 0;
    return 1;
}

static int run_chunks(Sink *s, size_t metaint, const size_t *sizes, size_t nsizes, unsigned seed)
{
    static IcyParser p;
    size_t off = 0, k = 0;
    unsigned r = seed;
    icy_init(&p, metaint, sink_audio, sink_title, s);
    while (off < stream_len) {
        size_t n;
        if (sizes) {
            n = sizes[k++ % nsizes];
        } else {
            r = r * 1103515245u + 12345u;
            n = 1 + (r >> 16) % 700;
        }
        if (n > stream_len - off)
            n = stream_len - off;
        if (icy_feed(&p, stream + off, n) != 0)
            return -1;
        off += n;
    }
    return 0;
}

int main(void)
{
    static IcyParser p;
    Sink *s;
    size_t one = 1, whole;
    size_t split;
    int ok;
    char out[256];

    build_stream();
    whole = stream_len;

    s = sink_new();
    ok = run_chunks(s, METAINT, &whole, 1, 0) == 0;
    CHECK("whole_feed_audio", ok && audio_matches(s, audio_expect, audio_expect_len));
    CHECK("whole_feed_titles", titles_match(s));
    free(s);

    s = sink_new();
    ok = run_chunks(s, METAINT, &one, 1, 0) == 0;
    CHECK("byte_feed_audio", ok && audio_matches(s, audio_expect, audio_expect_len));
    CHECK("byte_feed_titles", titles_match(s));
    free(s);

    {
        int all = 1;
        unsigned seed;
        for (seed = 1; seed <= 20; seed++) {
            s = sink_new();
            if (run_chunks(s, METAINT, NULL, 0, seed) != 0 ||
                !audio_matches(s, audio_expect, audio_expect_len) || !titles_match(s))
                all = 0;
            free(s);
        }
        CHECK("random_chunks_20_seeds", all);
    }

    /* Every two-piece split point: covers a boundary on each length byte,
     * inside every metadata block, and on each audio/meta transition. */
    {
        int all = 1;
        s = sink_new();
        for (split = 0; split <= stream_len && all; split++) {
            memset(s, 0, sizeof(*s));
            icy_init(&p, METAINT, sink_audio, sink_title, s);
            if (icy_feed(&p, stream, split) != 0 ||
                icy_feed(&p, stream + split, stream_len - split) != 0 ||
                !audio_matches(s, audio_expect, audio_expect_len) || !titles_match(s))
                all = 0;
        }
        free(s);
        CHECK("every_split_point", all);
    }

    /* metaint = 0: every byte is audio. */
    s = sink_new();
    ok = run_chunks(s, 0, NULL, 0, 7) == 0;
    CHECK("metaint0_passthrough", ok && audio_matches(s, stream, stream_len) && s->n_titles == 0);
    free(s);

    /* on_audio returning -1 stops the feed immediately. */
    s = sink_new();
    s->stop_after = 1;
    icy_init(&p, METAINT, sink_audio, sink_title, s);
    CHECK("stop_returns_minus1", icy_feed(&p, stream, stream_len) == -1);
    CHECK("stop_no_further_calls", s->audio_calls == 1 && s->audio_len == METAINT && s->n_titles == 0);
    free(s);

    s = sink_new();
    s->stop_after = 1;
    icy_init(&p, 0, sink_audio, sink_title, s);
    CHECK("stop_metaint0", icy_feed(&p, stream, stream_len) == -1 && s->audio_calls == 1);
    free(s);

    /* RED checks: the comparators must detect a wrong result. */
    s = sink_new();
    run_chunks(s, METAINT + 1, &whole, 1, 0);
    CHECK("red_wrong_metaint_differs", !audio_matches(s, audio_expect, audio_expect_len));
    free(s);

    s = sink_new();
    run_chunks(s, METAINT, &whole, 1, 0);
    s->audio[audio_expect_len / 2] ^= 0x01;
    CHECK("red_flipped_byte_detected", !audio_matches(s, audio_expect, audio_expect_len));
    snprintf(s->titles[1], 256, "Guns N");
    CHECK("red_wrong_title_detected", !titles_match(s));
    free(s);

    /* ---- icy_extract_title edge cases ------------------------------ */
    {
        const char *m = "StreamTitle='Artist - Song';StreamUrl='';";
        CHECK("extract_basic", icy_extract_title(m, strlen(m), out, sizeof(out)) == 1 &&
                               strcmp(out, "Artist - Song") == 0);
    }
    {
        const char *m = "StreamTitle='Guns N' Roses - Don't Cry';StreamUrl='';";
        CHECK("extract_apostrophes", icy_extract_title(m, strlen(m), out, sizeof(out)) == 1 &&
                                     strcmp(out, "Guns N' Roses - Don't Cry") == 0);
    }
    {
        const char *m = "StreamTitle='No end";
        CHECK("extract_missing_terminator", icy_extract_title(m, strlen(m), out, sizeof(out)) == 0);
    }
    {
        const char *m = "StreamUrl='http://x';";
        CHECK("extract_no_key", icy_extract_title(m, strlen(m), out, sizeof(out)) == 0);
    }
    {
        const char *m = "StreamTitle='abcdef';";
        CHECK("extract_truncates", icy_extract_title(m, strlen(m), out, 4) == 1 &&
                                   strcmp(out, "abc") == 0);
    }
    {
        char m[48];
        memset(m, 0, sizeof(m));
        memcpy(m, "StreamTitle='x';", 16);
        CHECK("extract_nul_padding", icy_extract_title(m, sizeof(m), out, sizeof(out)) == 1 &&
                                     strcmp(out, "x") == 0);
    }
    {
        const char *m = "StreamTitle='abc';";
        CHECK("extract_terminator_past_len",
              icy_extract_title(m, strlen(m) - 1, out, sizeof(out)) == 0);
    }
    {
        const char *m = "StreamTitle='';";
        CHECK("extract_empty_title", icy_extract_title(m, strlen(m), out, sizeof(out)) == 1 &&
                                     out[0] == '\0');
    }
    {
        char m[32];
        memset(m, 0, sizeof(m));
        memcpy(m, "Str\0eamTitle='x';", 17);
        CHECK("extract_ignores_after_nul", icy_extract_title(m, sizeof(m), out, sizeof(out)) == 0);
    }

    /* Truncation must land on a character boundary: half a UTF-8 sequence is
     * not text the renderer can draw. "abcd" + U+20AC (E2 82 AC) in 7 bytes. */
    {
        const char *m = "StreamTitle='abcd\xE2\x82\xAC';";
        char o[8];
        CHECK("extract_utf8_boundary",
              icy_extract_title(m, strlen(m), o, 7) == 1 && strcmp(o, "abcd") == 0);
        CHECK("extract_utf8_exact_fit",
              icy_extract_title(m, strlen(m), o, 8) == 1 &&
              strcmp(o, "abcd\xE2\x82\xAC") == 0);
    }
    /* A two-byte sequence, and a cut that already lands on a boundary. */
    {
        const char *m = "StreamTitle='ab\xC3\xA9xy';";
        char o[8];
        CHECK("extract_utf8_two_byte",
              icy_extract_title(m, strlen(m), o, 4) == 1 && strcmp(o, "ab") == 0);
        CHECK("extract_utf8_clean_cut",
              icy_extract_title(m, strlen(m), o, 5) == 1 &&
              strcmp(o, "ab\xC3\xA9") == 0);
    }

    printf("test_icy: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
