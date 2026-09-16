#include "url_util.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0, g_pass = 0;

#define CHECK(name, cond) do { \
    if (cond) { printf("PASS %s\n", name); g_pass++; } \
    else { printf("FAIL %s (%s:%d)\n", name, __FILE__, __LINE__); g_fail++; } \
} while (0)

/* Resolve and compare against the expected absolute URL. */
static void chk(const char *name, const char *base, const char *ref, const char *want)
{
    char out[1024];
    memset(out, 0x7F, sizeof(out));
    int rc = url_resolve(base, ref, out, sizeof(out));
    if (rc == 0 && strcmp(out, want) == 0) {
        printf("PASS %s\n", name);
        g_pass++;
    } else {
        printf("FAIL %s: rc=%d got \"%s\" want \"%s\"\n",
               name, rc, rc == 0 ? out : "", want);
        g_fail++;
    }
}

/* Resolution that must be rejected. */
static void chk_err(const char *name, const char *base, const char *ref)
{
    char out[1024];
    int rc = url_resolve(base, ref, out, sizeof(out));
    if (rc == -1) {
        printf("PASS %s\n", name);
        g_pass++;
    } else {
        printf("FAIL %s: rc=%d got \"%s\", expected -1\n", name, rc, out);
        g_fail++;
    }
}

#define BASE "http://h.example/a/b/c.m3u8"

/* A real BBC HLS media playlist URL: the path segments contain '=' and '%3d',
 * which must not confuse the directory split. */
#define BBC_BASE "http://as-hls-uk.live.cf.md.bbci.co.uk/pool_01505109/live/uk/" \
                 "bbc_radio_one/bbc_radio_one.isml/bbc_radio_one-audio%3d96000.norewind.m3u8"
#define BBC_DIR  "http://as-hls-uk.live.cf.md.bbci.co.uk/pool_01505109/live/uk/" \
                 "bbc_radio_one/bbc_radio_one.isml/"

int main(void)
{
    /* ---- url_is_absolute ------------------------------------------ */
    CHECK("abs_http", url_is_absolute("http://h/x") == 1);
    CHECK("abs_https", url_is_absolute("https://h/x") == 1);
    CHECK("abs_scheme_only", url_is_absolute("data:,x") == 1);
    CHECK("abs_mixed_case", url_is_absolute("HTTP://h/x") == 1);
    CHECK("abs_plus_dash_dot", url_is_absolute("a+b-c.d:x") == 1);
    CHECK("notabs_relative", url_is_absolute("seg.ts") == 0);
    CHECK("notabs_root", url_is_absolute("/seg.ts") == 0);
    CHECK("notabs_scheme_rel", url_is_absolute("//h/x") == 0);
    CHECK("notabs_leading_digit", url_is_absolute("1http:x") == 0);
    CHECK("notabs_underscore", url_is_absolute("bbc_radio_one-audio=96000.ts") == 0);
    CHECK("notabs_empty", url_is_absolute("") == 0);
    CHECK("notabs_null", url_is_absolute(NULL) == 0);

    /* ---- the four resolution forms from the header ----------------- */
    chk("res_absolute", BASE, "https://other.example/y.ts", "https://other.example/y.ts");
    chk("res_scheme_relative", BASE, "//cdn.example/z/s.ts", "http://cdn.example/z/s.ts");
    chk("res_root_relative", BASE, "/x/s.ts", "http://h.example/x/s.ts");
    chk("res_relative", BASE, "seg.ts", "http://h.example/a/b/seg.ts");
    chk("res_relative_subdir", BASE, "sub/seg.ts", "http://h.example/a/b/sub/seg.ts");

    /* ---- dot segments --------------------------------------------- */
    chk("res_dotdot", BASE, "../a/b", "http://h.example/a/a/b");
    chk("res_dotdot_once", BASE, "../seg.ts", "http://h.example/a/seg.ts");
    chk("res_dot", BASE, "./x", "http://h.example/a/b/x");
    chk("res_dot_dotdot_mix", BASE, "./../x.ts", "http://h.example/a/x.ts");
    chk("res_dotdot_above_root", BASE, "../../../../z", "http://h.example/z");
    chk("res_dotdot_in_root_relative", BASE, "/p/q/../r.ts", "http://h.example/p/r.ts");
    chk("res_trailing_dot_keeps_slash", BASE, "sub/.", "http://h.example/a/b/sub/");

    /* ---- query and fragment preservation --------------------------- */
    chk("res_query_relative", BASE, "seg.ts?foo=1&bar=2",
        "http://h.example/a/b/seg.ts?foo=1&bar=2");
    chk("res_query_root", BASE, "/s.ts?t=9", "http://h.example/s.ts?t=9");
    chk("res_query_not_normalised", BASE, "seg.ts?p=../../x",
        "http://h.example/a/b/seg.ts?p=../../x");
    chk("res_fragment", BASE, "seg.ts#frag", "http://h.example/a/b/seg.ts#frag");
    chk("res_query_scheme_relative", BASE, "//cdn.example/z.ts?k=1",
        "http://cdn.example/z.ts?k=1");
    chk("res_absolute_query_verbatim", BASE, "https://o.example/y.ts?a=b#c",
        "https://o.example/y.ts?a=b#c");
    /* The base's own query must not leak into the result. */
    chk("res_base_query_dropped", "http://h.example/a/b.m3u8?token=1", "s.ts",
        "http://h.example/a/s.ts");

    /* ---- RFC 3986 5.4.1 references that keep the base's path -------- */
    /* A query-only reference replaces the query and keeps the whole path,
       including the last segment. Token-refresh playlists use this form. */
    chk("res_query_only_keeps_last_segment", "http://a/b/c/d;p?q", "?y",
        "http://a/b/c/d;p?y");
    chk("res_query_only_hls", "https://cdn.example/live/master.m3u8?token=abc",
        "?token=xyz", "https://cdn.example/live/master.m3u8?token=xyz");
    /* A fragment-only reference keeps the base's path AND its query. */
    chk("res_fragment_only_keeps_path_and_query", "http://a/b/c/d;p?q", "#s",
        "http://a/b/c/d;p?q#s");

    /* ---- empty path segments survive normalisation ------------------ */
    /* RFC 3986 5.2.4 removes only "." and ".."; "//" inside a path is a real
       empty segment and the server will 404 without it. */
    chk("res_empty_segment_kept", "https://cdn.example/a//b/p.m3u8", "s.ts",
        "https://cdn.example/a//b/s.ts");
    chk("res_empty_segment_in_ref", "https://cdn.example/a/p.m3u8", "x//y.ts",
        "https://cdn.example/a/x//y.ts");

    /* ---- too many segments is an error, never a truncated URL ------- */
    {
        char deep[1400];
        size_t pos = 0;
        for (int i = 0; i < 200 && pos + 2 < sizeof(deep); i++) {
            deep[pos++] = 's';
            deep[pos++] = '/';
        }
        pos += (size_t)snprintf(deep + pos, sizeof(deep) - pos, "f.ts");
        deep[pos] = '\0';

        char out[2048];
        int rc = url_resolve("http://h.example/", deep, out, sizeof(out));
        /* Dropping segments silently would return 0 with the filename gone. */
        CHECK("err_segment_cap_rejected",
              rc == -1 || strstr(out, "f.ts") != NULL);
    }

    /* ---- bases without a path -------------------------------------- */
    chk("res_base_no_path", "http://h.example", "seg.ts", "http://h.example/seg.ts");
    chk("res_base_root", "http://h.example/", "seg.ts", "http://h.example/seg.ts");
    chk("res_base_port", "http://h.example:8000/live/p.m3u8", "s.ts",
        "http://h.example:8000/live/s.ts");
    chk("res_https_kept", "https://h.example/a/p.m3u8", "s.ts", "https://h.example/a/s.ts");

    /* ---- real BBC playlist URLs ------------------------------------ */
    chk("res_bbc_segment", BBC_BASE, "bbc_radio_one-audio=96000-279612681.ts",
        BBC_DIR "bbc_radio_one-audio=96000-279612681.ts");
    chk("res_bbc_variant_absolute", BBC_BASE,
        BBC_DIR "bbc_radio_one-audio%3d320000.norewind.m3u8",
        BBC_DIR "bbc_radio_one-audio%3d320000.norewind.m3u8");

    /* ---- rejections ------------------------------------------------ */
    chk_err("err_empty_ref", BASE, "");
    chk_err("err_null_ref", BASE, NULL);
    chk_err("err_null_base_relative", NULL, "seg.ts");
    chk_err("err_base_no_scheme", "h.example/a/b.m3u8", "seg.ts");
    chk_err("err_base_no_authority", "http:/a/b.m3u8", "seg.ts");
    /* A null base is still fine for an absolute ref. */
    chk("res_null_base_absolute", NULL, "http://h.example/x", "http://h.example/x");

    /* ---- output buffer sizing -------------------------------------- */
    {
        const char *want = "http://h.example/a/b/seg.ts";   /* 27 chars */
        size_t need = strlen(want);
        char small[8];
        char exact[28];
        char tight[27];
        int rc;

        rc = url_resolve(BASE, "seg.ts", small, sizeof(small));
        CHECK("buf_too_small_rejected", rc == -1);

        rc = url_resolve(BASE, "seg.ts", exact, sizeof(exact));
        CHECK("buf_exact_fit_ok", rc == 0 && strcmp(exact, want) == 0 && need == 27);

        rc = url_resolve(BASE, "seg.ts", tight, sizeof(tight));
        CHECK("buf_one_short_rejected", rc == -1);

        /* Same boundary on the absolute fast path. */
        rc = url_resolve(BASE, want, exact, sizeof(exact));
        CHECK("buf_abs_exact_fit_ok", rc == 0 && strcmp(exact, want) == 0);
        rc = url_resolve(BASE, want, tight, sizeof(tight));
        CHECK("buf_abs_one_short_rejected", rc == -1);

        rc = url_resolve(BASE, "seg.ts", exact, 0);
        CHECK("buf_zero_rejected", rc == -1);
        rc = url_resolve(BASE, "seg.ts", NULL, 64);
        CHECK("buf_null_rejected", rc == -1);
    }

    printf("test_url_util: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
