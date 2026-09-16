#include "playlist.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0, g_pass = 0;

#define CHECK(name, cond) do { \
    if (cond) { printf("PASS %s\n", name); g_pass++; } \
    else { printf("FAIL %s (%s:%d)\n", name, __FILE__, __LINE__); g_fail++; } \
} while (0)

static char out[512];

/* Runs the parser over a NUL-terminated literal and compares the result. */
static int first_is(const char *text, const char *base, const char *want)
{
    memset(out, 0, sizeof(out));
    if (playlist_first_url(text, strlen(text), base, out, sizeof(out)) != 0)
        return 0;
    return strcmp(out, want) == 0;
}

static int first_fails(const char *text, const char *base)
{
    memset(out, 0, sizeof(out));
    return playlist_first_url(text, strlen(text), base, out, sizeof(out)) == -1;
}

int main(void)
{
    const char *base = "http://example.com/dir/list.pls";

    /* ---- .pls ------------------------------------------------------- */
    {
        const char *pls =
            "[playlist]\n"
            "NumberOfEntries=1\n"
            "File1=http://example.com:8000/stream\n"
            "Title1=Some Radio\n"
            "Length1=-1\n"
            "Version=2\n";
        CHECK("pls_basic", first_is(pls, base, "http://example.com:8000/stream"));
    }

    {
        /* A file may start at File2 - the lowest present entry wins. */
        const char *pls =
            "[playlist]\n"
            "NumberOfEntries=2\n"
            "File2=http://example.com/two\n"
            "Title2=Two\n"
            "File3=http://example.com/three\n";
        CHECK("pls_starts_at_file2", first_is(pls, base, "http://example.com/two"));
    }

    {
        /* Out of order in the text: still the lowest number, not the first
         * line. This is the case that goes red if the FileN comparison
         * breaks. */
        const char *pls =
            "[playlist]\n"
            "File3=http://example.com/three\n"
            "File10=http://example.com/ten\n"
            "File1=http://example.com/one\n"
            "File2=http://example.com/two\n";
        CHECK("pls_lowest_not_first", first_is(pls, base, "http://example.com/one"));
    }

    {
        /* Numeric, not lexicographic: File10 must not beat File9. */
        const char *pls =
            "[playlist]\n"
            "File10=http://example.com/ten\n"
            "File9=http://example.com/nine\n";
        CHECK("pls_numeric_order", first_is(pls, base, "http://example.com/nine"));
    }

    {
        const char *pls =
            "[PLAYLIST]\n"
            "FILE1=http://example.com/upper\n";
        CHECK("pls_upper_keys", first_is(pls, base, "http://example.com/upper"));
    }

    {
        const char *pls =
            "[Playlist]\n"
            "file1=http://example.com/lower\n";
        CHECK("pls_lower_keys", first_is(pls, base, "http://example.com/lower"));
    }

    {
        const char *pls =
            "[playlist]\n"
            "   File1   =   http://example.com/spaced   \n";
        CHECK("pls_whitespace_around_eq", first_is(pls, base, "http://example.com/spaced"));
    }

    {
        const char *pls =
            "[playlist]\n"
            "\tFile1\t=\thttp://example.com/tabbed\t\n";
        CHECK("pls_tabs_around_eq", first_is(pls, base, "http://example.com/tabbed"));
    }

    {
        const char *pls =
            "[playlist]\r\n"
            "NumberOfEntries=1\r\n"
            "File1=http://example.com/crlf\r\n"
            "Title1=CRLF Radio\r\n";
        CHECK("pls_crlf", first_is(pls, base, "http://example.com/crlf"));
    }

    {
        const char *pls =
            "\xEF\xBB\xBF[playlist]\n"
            "File1=http://example.com/bom\n";
        CHECK("pls_bom", first_is(pls, base, "http://example.com/bom"));
    }

    {
        /* Metadata only: nothing playable. */
        const char *pls =
            "[playlist]\n"
            "NumberOfEntries=0\n"
            "Version=2\n";
        CHECK("pls_no_entries", first_fails(pls, base));
    }

    {
        /* Title/Length must not be mistaken for entries. */
        const char *pls =
            "[playlist]\n"
            "Title1=Some Radio\n"
            "Length1=-1\n"
            "NumberOfEntries=1\n";
        CHECK("pls_title_length_ignored", first_fails(pls, base));
    }

    {
        const char *pls =
            "[playlist]\n"
            "File1=\n"
            "File2=http://example.com/two\n";
        CHECK("pls_empty_value_skipped", first_is(pls, base, "http://example.com/two"));
    }

    {
        /* "File=" with no number is not an entry. */
        const char *pls =
            "[playlist]\n"
            "File=http://example.com/nonum\n"
            "File2=http://example.com/two\n";
        CHECK("pls_unnumbered_key_ignored", first_is(pls, base, "http://example.com/two"));
    }

    {
        const char *pls =
            "[playlist]\n"
            "File1=stream.mp3\n";
        CHECK("pls_relative_resolved", first_is(pls, base, "http://example.com/dir/stream.mp3"));
    }

    {
        const char *pls =
            "[playlist]\n"
            "File1=/live/stream.mp3\n";
        CHECK("pls_root_relative_resolved", first_is(pls, base, "http://example.com/live/stream.mp3"));
    }

    {
        /* Absolute entries are left exactly as they are. */
        const char *pls =
            "[playlist]\n"
            "File1=https://other.example.net:9000/hi?x=1\n";
        CHECK("pls_absolute_untouched", first_is(pls, base, "https://other.example.net:9000/hi?x=1"));
    }

    {
        /* A nested playlist URL comes back as-is; no hops are followed. */
        const char *pls =
            "[playlist]\n"
            "File1=http://example.com/inner.m3u\n";
        CHECK("pls_nested_returned_asis", first_is(pls, base, "http://example.com/inner.m3u"));
    }

    /* ---- .m3u ------------------------------------------------------- */
    {
        const char *m3u =
            "#EXTM3U\n"
            "#EXTINF:-1,Some Radio\n"
            "http://example.com/live.mp3\n"
            "http://example.com/second.mp3\n";
        CHECK("m3u_comments_skipped", first_is(m3u, base, "http://example.com/live.mp3"));
    }

    {
        const char *m3u = "\n\n   \n\nhttp://example.com/afterblanks\n";
        CHECK("m3u_leading_blank_lines", first_is(m3u, base, "http://example.com/afterblanks"));
    }

    {
        const char *m3u = "\r\n#EXTINF:-1,R\r\nhttp://example.com/crlf.mp3\r\n";
        CHECK("m3u_crlf", first_is(m3u, base, "http://example.com/crlf.mp3"));
    }

    {
        const char *m3u = "\xEF\xBB\xBF#EXTM3U\nhttp://example.com/bom.mp3\n";
        CHECK("m3u_bom", first_is(m3u, base, "http://example.com/bom.mp3"));
    }

    {
        const char *m3u = "#EXTM3U\n#EXTINF:-1,R\nstream.aac\n";
        CHECK("m3u_relative_resolved", first_is(m3u, base, "http://example.com/dir/stream.aac"));
    }

    {
        const char *m3u = "  http://example.com/padded.mp3  \n";
        CHECK("m3u_trimmed", first_is(m3u, base, "http://example.com/padded.mp3"));
    }

    {
        /* No trailing newline on the only line. */
        CHECK("m3u_no_final_newline",
              first_is("http://example.com/nonl.mp3", base, "http://example.com/nonl.mp3"));
    }

    {
        const char *m3u = "# just a comment\n#EXTINF:-1,R\n#another\n";
        CHECK("m3u_only_comments", first_fails(m3u, base));
    }

    {
        CHECK("m3u_empty", first_fails("", base));
        CHECK("m3u_blank_only", first_fails("\n\r\n   \n", base));
        CHECK("m3u_bom_only", first_fails("\xEF\xBB\xBF", base));
    }

    {
        /* Relative with no base to resolve against cannot produce a URL. */
        CHECK("relative_without_base_fails", first_fails("stream.mp3", NULL));
    }

    /* ---- buffer discipline ------------------------------------------ */
    {
        /* Too small to hold the URL: must fail, and must not scribble past
         * the length it was given. */
        char small[64];
        int rc;
        size_t i;
        int clean = 1;
        const char *pls =
            "[playlist]\n"
            "File1=http://example.com/a-considerably-longer-stream-url.mp3\n";

        memset(small, 'X', sizeof(small));
        rc = playlist_first_url(pls, strlen(pls), base, small, 8);
        for (i = 8; i < sizeof(small); i++)
            if (small[i] != 'X')
                clean = 0;
        CHECK("small_buffer_fails", rc == -1);
        CHECK("small_buffer_no_overrun", clean);

        memset(small, 'X', sizeof(small));
        rc = playlist_first_url("http://example.com/a-long-one.mp3", 33, base, small, 8);
        clean = 1;
        for (i = 8; i < sizeof(small); i++)
            if (small[i] != 'X')
                clean = 0;
        CHECK("small_buffer_m3u_fails", rc == -1);
        CHECK("small_buffer_m3u_no_overrun", clean);

        CHECK("zero_outsz_fails",
              playlist_first_url(pls, strlen(pls), base, small, 0) == -1);
    }

    {
        /* Exactly-fitting buffer is fine: 26 chars + NUL. */
        char exact[27];
        const char *m3u = "http://example.com/fit.mp3\n";
        CHECK("exact_fit_ok",
              playlist_first_url(m3u, strlen(m3u), base, exact, sizeof(exact)) == 0 &&
              strcmp(exact, "http://example.com/fit.mp3") == 0);
    }

    {
        /* Not NUL-terminated: only len bytes may be read (ASan checks this). */
        static const char raw[] = "http://example.com/x.mp3\nhttp://example.com/y.mp3\n";
        memset(out, 0, sizeof(out));
        CHECK("respects_len",
              playlist_first_url(raw, 24, base, out, sizeof(out)) == 0 &&
              strcmp(out, "http://example.com/x.mp3") == 0);
    }

    {
        CHECK("null_text_fails", playlist_first_url(NULL, 0, base, out, sizeof(out)) == -1);
    }

    printf("test_playlist: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
