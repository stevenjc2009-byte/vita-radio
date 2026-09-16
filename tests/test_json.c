#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0, g_pass = 0;

#define CHECK(name, cond) do { \
    if (cond) { printf("PASS %s\n", name); g_pass++; } \
    else { printf("FAIL %s (%s:%d)\n", name, __FILE__, __LINE__); g_fail++; } \
} while (0)

/* Parse from a heap block of exactly n bytes and free it before the tree is
 * used: ASan then catches both an overread past the document and any pointer
 * the DOM wrongly kept into the input. */
static JsonValue *parse_exact(const char *s, size_t n)
{
    char      *buf;
    JsonValue *v;

    if (n == 0)
        return json_parse(s, 0);

    buf = (char *)malloc(n);
    if (!buf)
        return NULL;
    memcpy(buf, s, n);
    v = json_parse(buf, n);
    free(buf);
    return v;
}

static JsonValue *pj(const char *s)
{
    return parse_exact(s, strlen(s));
}

/* Byte-exact string compare, so an escape decoder that emits a short or long
 * sequence cannot slip past strcmp on a common prefix. */
static int str_is(const JsonValue *v, const char *want, size_t wantlen)
{
    const char *got = json_string(v, NULL);
    if (!got)
        return 0;
    return strlen(got) == wantlen && memcmp(got, want, wantlen) == 0;
}

#define STR_IS(v, lit) str_is((v), (lit), sizeof(lit) - 1)

/* Does this text fail to parse at all? */
static int rejects(const char *s)
{
    JsonValue *v = parse_exact(s, strlen(s));
    if (!v)
        return 1;
    json_free(v);
    return 0;
}

static void test_scalars(void)
{
    JsonValue *v;

    v = pj("null");
    CHECK("scalar_null", v && json_type(v) == JSON_NULL);
    json_free(v);

    v = pj("true");
    CHECK("scalar_true", v && json_type(v) == JSON_BOOL && json_bool(v, 0) == 1);
    json_free(v);

    v = pj("false");
    CHECK("scalar_false", v && json_type(v) == JSON_BOOL && json_bool(v, 1) == 0);
    json_free(v);

    v = pj("0");
    CHECK("scalar_zero", v && json_type(v) == JSON_NUMBER && json_number(v, -1) == 0.0);
    json_free(v);

    v = pj("  \"hi\"  ");
    CHECK("scalar_string", v && json_type(v) == JSON_STRING && STR_IS(v, "hi"));
    json_free(v);

    v = pj("\"\"");
    CHECK("scalar_empty_string", v && json_type(v) == JSON_STRING && STR_IS(v, ""));
    json_free(v);
}

static void test_numbers(void)
{
    JsonValue *v;

    v = pj("-1");
    CHECK("num_negative", v && json_number(v, 0) == -1.0);
    json_free(v);

    v = pj("1.5");
    CHECK("num_fraction", v && json_number(v, 0) == 1.5);
    json_free(v);

    v = pj("1e3");
    CHECK("num_exponent", v && json_number(v, 0) == 1000.0);
    json_free(v);

    v = pj("1E+3");
    CHECK("num_exponent_plus_upper", v && json_number(v, 0) == 1000.0);
    json_free(v);

    v = pj("-2.5e-3");
    CHECK("num_neg_frac_negexp", v && json_number(v, 0) == -0.0025);
    json_free(v);

    v = pj("0.0");
    CHECK("num_zero_frac", v && json_number(v, 9) == 0.0);
    json_free(v);

    v = pj("128000");
    CHECK("num_bitrate", v && json_number(v, 0) == 128000.0);
    json_free(v);

    /* Long but legal: must not be truncated into a different value. */
    v = pj("1234567890123456789012345678901234567890"
           "1234567890123456789012345678901234567890e-40");
    CHECK("num_very_long_token", v && json_type(v) == JSON_NUMBER &&
                                 json_number(v, 0) > 1.2e39 && json_number(v, 0) < 1.3e39);
    json_free(v);

    CHECK("num_reject_leading_zero", rejects("01"));
    CHECK("num_reject_leading_plus", rejects("+1"));
    CHECK("num_reject_bare_dot", rejects(".5"));
    CHECK("num_reject_trailing_dot", rejects("1."));
    CHECK("num_reject_empty_exponent", rejects("1e"));
    CHECK("num_reject_exponent_sign_only", rejects("1e+"));
    CHECK("num_reject_lone_minus", rejects("-"));
    CHECK("num_reject_hex", rejects("0x10"));
    CHECK("num_reject_inf", rejects("Infinity"));
    CHECK("num_reject_nan", rejects("NaN"));
}

static void test_escapes(void)
{
    JsonValue *v;

    /* "\"\\\/\b\f\n\r\t" */
    v = pj("\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"");
    CHECK("esc_all_simple", v && STR_IS(v, "\"\\/\b\f\n\r\t"));
    json_free(v);

    v = pj("\"\\u0041\"");
    CHECK("esc_u_1byte", v && STR_IS(v, "A"));
    json_free(v);

    v = pj("\"\\u00e9\"");
    CHECK("esc_u_2byte", v && STR_IS(v, "\xC3\xA9"));
    json_free(v);

    v = pj("\"\\u00C9\"");
    CHECK("esc_u_uppercase_hex", v && STR_IS(v, "\xC3\x89"));
    json_free(v);

    v = pj("\"\\u20ac\"");
    CHECK("esc_u_3byte", v && STR_IS(v, "\xE2\x82\xAC"));
    json_free(v);

    v = pj("\"\\u0080\"");
    CHECK("esc_u_2byte_boundary", v && STR_IS(v, "\xC2\x80"));
    json_free(v);

    v = pj("\"\\u07FF\"");
    CHECK("esc_u_2byte_top", v && STR_IS(v, "\xDF\xBF"));
    json_free(v);

    v = pj("\"\\u0800\"");
    CHECK("esc_u_3byte_boundary", v && STR_IS(v, "\xE0\xA0\x80"));
    json_free(v);

    v = pj("\"\\uFFFF\"");
    CHECK("esc_u_3byte_top", v && STR_IS(v, "\xEF\xBF\xBF"));
    json_free(v);

    /* Surrogate pair -> one 4-byte sequence (U+1F600). */
    v = pj("\"\\uD83D\\uDE00\"");
    CHECK("esc_surrogate_pair", v && STR_IS(v, "\xF0\x9F\x98\x80"));
    json_free(v);

    v = pj("\"a\\uD83D\\uDE00b\"");
    CHECK("esc_surrogate_pair_inline", v && STR_IS(v, "a" "\xF0\x9F\x98\x80" "b"));
    json_free(v);

    /* Lone surrogates decode to U+FFFD, never to raw garbage or a short write. */
    v = pj("\"\\uD83D\"");
    CHECK("esc_lone_high_surrogate", v && STR_IS(v, "\xEF\xBF\xBD"));
    json_free(v);

    v = pj("\"\\uDE00\"");
    CHECK("esc_lone_low_surrogate", v && STR_IS(v, "\xEF\xBF\xBD"));
    json_free(v);

    v = pj("\"\\uD83Dx\"");
    CHECK("esc_lone_high_then_char", v && STR_IS(v, "\xEF\xBF\xBD" "x"));
    json_free(v);

    v = pj("\"\\uD83D\\u0041\"");
    CHECK("esc_lone_high_then_escape", v && STR_IS(v, "\xEF\xBF\xBD" "A"));
    json_free(v);

    v = pj("\"\\uD83D\\uD83D\\uDE00\"");
    CHECK("esc_high_high_low", v && STR_IS(v, "\xEF\xBF\xBD" "\xF0\x9F\x98\x80"));
    json_free(v);

    /* The lone surrogate is the very last thing in the buffer: a decoder that
     * peeks for a trailing "\u" must not read past the end (ASan). */
    {
        static const char doc[] = "\"\\uD83D\"";
        v = parse_exact(doc, sizeof(doc) - 1);
        CHECK("esc_lone_surrogate_at_buffer_end", v && STR_IS(v, "\xEF\xBF\xBD"));
        json_free(v);
    }

    /* Raw (unescaped) UTF-8 passes through untouched. */
    v = pj("\"caf\xC3\xA9\"");
    CHECK("esc_raw_utf8_passthrough", v && STR_IS(v, "caf\xC3\xA9"));
    json_free(v);

    /* A\u0000 escape becomes U+FFFD, never a real NUL byte:
     * json_string() hands back a bare char* with no length, so a NUL in the
     * middle silently truncates every consumer - a station URL included. Same
     * treatment as an unpaired surrogate, and the same 6-in/3-out size. */
    v = pj("\"a\\u0000b\"");
    CHECK("esc_u_nul", v && STR_IS(v, "a\xEF\xBF\xBD" "b"));
    json_free(v);

    /* At the very start of a span, and as the whole span. */
    v = pj("\"\\u0000\"");
    CHECK("esc_u_nul_alone", v && STR_IS(v, "\xEF\xBF\xBD"));
    json_free(v);

    v = pj("\"x\\u0000\"");
    CHECK("esc_u_nul_at_end", v && STR_IS(v, "x\xEF\xBF\xBD"));
    json_free(v);

    CHECK("esc_reject_unknown", rejects("\"\\q\""));
    CHECK("esc_reject_short_u", rejects("\"\\u12\""));
    CHECK("esc_reject_bad_hex", rejects("\"\\u12g4\""));
    CHECK("esc_reject_trailing_backslash", rejects("\"abc\\"));
    CHECK("esc_reject_raw_newline", rejects("\"a\nb\""));
    CHECK("esc_reject_raw_control", rejects("\"a\x01" "b\""));
    CHECK("esc_reject_raw_tab", rejects("\"a\tb\""));
}

static void test_containers(void)
{
    JsonValue *v;
    const JsonValue *a, *b;

    v = pj("[]");
    CHECK("empty_array", v && json_type(v) == JSON_ARRAY && json_array_count(v) == 0 &&
                         json_array_at(v, 0) == NULL);
    json_free(v);

    v = pj("{}");
    CHECK("empty_object", v && json_type(v) == JSON_OBJECT &&
                          json_object_get(v, "a") == NULL);
    json_free(v);

    v = pj("[1,2,3]");
    CHECK("array_count", v && json_array_count(v) == 3);
    CHECK("array_items", v && json_number(json_array_at(v, 0), 0) == 1 &&
                         json_number(json_array_at(v, 1), 0) == 2 &&
                         json_number(json_array_at(v, 2), 0) == 3);
    CHECK("array_out_of_range", v && json_array_at(v, 3) == NULL &&
                                json_array_at(v, -1) == NULL);
    json_free(v);

    v = pj("[null,true,false,\"s\",1,[],{}]");
    CHECK("array_mixed_types", v && json_array_count(v) == 7 &&
          json_type(json_array_at(v, 0)) == JSON_NULL &&
          json_type(json_array_at(v, 1)) == JSON_BOOL &&
          json_type(json_array_at(v, 2)) == JSON_BOOL &&
          json_type(json_array_at(v, 3)) == JSON_STRING &&
          json_type(json_array_at(v, 4)) == JSON_NUMBER &&
          json_type(json_array_at(v, 5)) == JSON_ARRAY &&
          json_type(json_array_at(v, 6)) == JSON_OBJECT);
    json_free(v);

    v = pj("{\"a\":{\"b\":[1,{\"c\":\"deep\"}]}}");
    a = json_object_get(v, "a");
    b = a ? json_object_get(a, "b") : NULL;
    b = b ? json_array_at(b, 1) : NULL;
    b = b ? json_object_get(b, "c") : NULL;
    CHECK("nested_lookup", b && STR_IS(b, "deep"));
    json_free(v);

    /* An escaped key must be matched by its decoded bytes. */
    v = pj("{\"caf\\u00e9\":7}");
    CHECK("object_escaped_key", v && json_number(json_object_get(v, "caf\xC3\xA9"), 0) == 7);
    CHECK("object_escaped_key_not_raw", v && json_object_get(v, "caf\\u00e9") == NULL);
    json_free(v);

    /* Duplicate keys: the first one wins, and both are freed. */
    v = pj("{\"a\":1,\"a\":2}");
    CHECK("duplicate_keys_first_wins", v && json_number(json_object_get(v, "a"), 0) == 1);
    json_free(v);

    v = pj("{\"a\":1,\"b\":2,\"c\":3}");
    CHECK("object_missing_key", v && json_object_get(v, "z") == NULL);
    CHECK("object_last_key", v && json_number(json_object_get(v, "c"), 0) == 3);
    json_free(v);

    /* An array wide enough to exercise whatever growth strategy is used. */
    {
        size_t i;
        char  *doc = (char *)malloc(4 * 300 + 3);
        size_t n = 0;
        int    ok = 1;
        doc[n++] = '[';
        for (i = 0; i < 300; i++)
            n += (size_t)sprintf(doc + n, "%s%u", i ? "," : "", (unsigned)(i % 10));
        doc[n++] = ']';
        v = parse_exact(doc, n);
        free(doc);
        if (!v || json_array_count(v) != 300)
            ok = 0;
        else
            for (i = 0; i < 300; i++)
                if (json_number(json_array_at(v, (int)i), -1) != (double)(i % 10))
                    ok = 0;
        CHECK("array_300_items", ok);
        json_free(v);
    }
}

static void test_whitespace(void)
{
    JsonValue *v;

    v = pj(" \t\r\n { \t\r\n \"a\" \t\r\n : \t\r\n [ \t\r\n 1 \t\r\n , \t\r\n 2 \t\r\n ]"
           " \t\r\n , \t\r\n \"b\" \t\r\n : \t\r\n true \t\r\n } \t\r\n ");
    CHECK("whitespace_everywhere", v && json_type(v) == JSON_OBJECT &&
          json_array_count(json_object_get(v, "a")) == 2 &&
          json_number(json_array_at(json_object_get(v, "a"), 1), 0) == 2 &&
          json_bool(json_object_get(v, "b"), 0) == 1);
    json_free(v);

    v = pj("[ ]");
    CHECK("whitespace_empty_array", v && json_array_count(v) == 0);
    json_free(v);

    v = pj("{ }");
    CHECK("whitespace_empty_object", v && json_type(v) == JSON_OBJECT);
    json_free(v);
}

static void test_malformed(void)
{
    CHECK("bad_empty_input", rejects(""));
    CHECK("bad_whitespace_only", rejects("   \n\t "));
    CHECK("bad_null_text", json_parse(NULL, 4) == NULL);
    CHECK("bad_zero_len", json_parse("null", 0) == NULL);

    CHECK("bad_unterminated_string", rejects("\"abc"));
    CHECK("bad_unterminated_string_in_object", rejects("{\"a\":\"abc}"));
    CHECK("bad_unterminated_array", rejects("[1,2"));
    CHECK("bad_unterminated_array_empty", rejects("["));
    CHECK("bad_unterminated_object", rejects("{\"a\":1"));
    CHECK("bad_unterminated_object_empty", rejects("{"));
    CHECK("bad_unclosed_nested", rejects("[[1,2],[3"));

    CHECK("bad_trailing_garbage", rejects("{} x"));
    CHECK("bad_trailing_value", rejects("1 2"));
    CHECK("bad_trailing_brace", rejects("[1]]"));
    CHECK("bad_trailing_comma_top", rejects("null,"));

    CHECK("bad_array_trailing_comma", rejects("[1,]"));
    CHECK("bad_array_leading_comma", rejects("[,1]"));
    CHECK("bad_array_double_comma", rejects("[1,,2]"));
    CHECK("bad_array_missing_comma", rejects("[1 2]"));
    CHECK("bad_object_trailing_comma", rejects("{\"a\":1,}"));
    CHECK("bad_object_missing_value", rejects("{\"a\":}"));
    CHECK("bad_object_missing_colon", rejects("{\"a\" 1}"));
    CHECK("bad_object_unquoted_key", rejects("{a:1}"));
    CHECK("bad_object_bare_comma", rejects("{,}"));
    CHECK("bad_object_missing_comma", rejects("{\"a\":1 \"b\":2}"));
    CHECK("bad_mismatched_brackets", rejects("[1}"));

    CHECK("bad_truncated_true", rejects("tru"));
    CHECK("bad_truncated_null", rejects("nul"));
    CHECK("bad_truncated_false", rejects("fals"));
    CHECK("bad_capital_true", rejects("True"));
    CHECK("bad_bare_word", rejects("undefined"));
    CHECK("bad_single_quotes", rejects("'a'"));

    /* "true" as a prefix of the buffer only - must not read the 5th byte. */
    {
        JsonValue *t = parse_exact("trueX", 4);
        CHECK("bad_true_prefix_exact_len", t != NULL && json_bool(t, 0) == 1);
        json_free(t);
    }
    CHECK("bad_tru_exact_len", parse_exact("trueX", 3) == NULL);
}

static void test_not_nul_terminated(void)
{
    /* Exact-length buffers with no terminator, on the heap so ASan traps any
     * read past the end. */
    JsonValue *v;

    v = parse_exact("{\"a\":123}", 9);
    CHECK("no_nul_object", v && json_number(json_object_get(v, "a"), 0) == 123);
    json_free(v);

    v = parse_exact("123", 3);
    CHECK("no_nul_number", v && json_number(v, 0) == 123);
    json_free(v);

    v = parse_exact("1.5e2", 5);
    CHECK("no_nul_number_exp", v && json_number(v, 0) == 150.0);
    json_free(v);

    /* Number followed immediately by the end of the buffer, where a strtod on
     * an unterminated buffer would keep scanning. */
    v = parse_exact("[1e2]", 5);
    CHECK("no_nul_number_in_array", v && json_number(json_array_at(v, 0), 0) == 100.0);
    json_free(v);

    v = parse_exact("\"abc\"", 5);
    CHECK("no_nul_string", v && STR_IS(v, "abc"));
    json_free(v);

    /* The document is a valid prefix of a longer buffer: the extra bytes are
     * not ours to read, and the short length must still parse cleanly. */
    v = parse_exact("nullnull", 4);
    CHECK("no_nul_prefix_only", v && json_type(v) == JSON_NULL);
    json_free(v);
}

static void test_depth(void)
{
    /* 64 is the documented limit: exactly 64 parses, 65 does not. */
    char *doc;
    JsonValue *v;
    int i;
    const int limit = 64;

    /* Big enough for the widest case below: 4096 levels of {"a": = 5 bytes each. */
    doc = (char *)malloc(5 * 4096 + 16);

    for (i = 0; i < limit; i++) { doc[i] = '['; doc[2 * limit - 1 - i] = ']'; }
    v = parse_exact(doc, (size_t)(2 * limit));
    CHECK("depth_at_limit_ok", v != NULL && json_type(v) == JSON_ARRAY);
    json_free(v);

    for (i = 0; i < limit + 1; i++) { doc[i] = '['; doc[2 * (limit + 1) - 1 - i] = ']'; }
    CHECK("depth_over_limit_null", parse_exact(doc, (size_t)(2 * (limit + 1))) == NULL);

    /* Wildly deep: must return NULL, not recurse 4096 frames into a fault. */
    for (i = 0; i < 4096; i++) { doc[i] = '['; doc[2 * 4096 - 1 - i] = ']'; }
    CHECK("depth_4096_arrays_null", parse_exact(doc, 2 * 4096) == NULL);

    for (i = 0; i < 4096; i++) { doc[i] = '['; }
    CHECK("depth_4096_unclosed_null", parse_exact(doc, 4096) == NULL);

    /* Objects nest through the same counter. */
    {
        size_t n = 0;
        for (i = 0; i < 4096; i++) { doc[n++] = '{'; doc[n++] = '"'; doc[n++] = 'a';
                                     doc[n++] = '"'; doc[n++] = ':'; }
        doc[n++] = '1';
        CHECK("depth_4096_objects_null", parse_exact(doc, n) == NULL);
    }

    free(doc);
}

static void test_accessors(void)
{
    JsonValue *num = pj("42");
    JsonValue *str = pj("\"s\"");
    JsonValue *bol = pj("true");
    JsonValue *nul = pj("null");
    JsonValue *arr = pj("[1]");
    JsonValue *obj = pj("{\"k\":1}");

    /* Wrong type -> documented fallback. */
    CHECK("wrong_string_of_number", strcmp(json_string(num, "fb"), "fb") == 0);
    CHECK("wrong_string_of_array", strcmp(json_string(arr, "fb"), "fb") == 0);
    CHECK("wrong_string_of_null", strcmp(json_string(nul, "fb"), "fb") == 0);
    CHECK("wrong_number_of_string", json_number(str, -7.5) == -7.5);
    CHECK("wrong_number_of_bool", json_number(bol, -7.5) == -7.5);
    CHECK("wrong_number_of_object", json_number(obj, -7.5) == -7.5);
    CHECK("wrong_bool_of_number", json_bool(num, 3) == 3);
    CHECK("wrong_bool_of_null", json_bool(nul, 3) == 3);
    CHECK("wrong_bool_of_string", json_bool(str, 3) == 3);
    CHECK("wrong_count_of_object", json_array_count(obj) == 0);
    CHECK("wrong_count_of_string", json_array_count(str) == 0);
    CHECK("wrong_at_of_object", json_array_at(obj, 0) == NULL);
    CHECK("wrong_at_of_number", json_array_at(num, 0) == NULL);
    CHECK("wrong_get_of_array", json_object_get(arr, "k") == NULL);
    CHECK("wrong_get_of_number", json_object_get(num, "k") == NULL);
    CHECK("wrong_get_null_key", json_object_get(obj, NULL) == NULL);

    /* A NULL fallback is legal and must come straight back. */
    CHECK("fallback_null_string", json_string(num, NULL) == NULL);

    json_free(num); json_free(str); json_free(bol);
    json_free(nul); json_free(arr); json_free(obj);
}

static void test_null_arguments(void)
{
    CHECK("null_type", json_type(NULL) == JSON_NULL);
    CHECK("null_array_count", json_array_count(NULL) == 0);
    CHECK("null_array_at", json_array_at(NULL, 0) == NULL);
    CHECK("null_object_get", json_object_get(NULL, "a") == NULL);
    CHECK("null_object_get_null_key", json_object_get(NULL, NULL) == NULL);
    CHECK("null_string", strcmp(json_string(NULL, "fb"), "fb") == 0);
    CHECK("null_string_null_fallback", json_string(NULL, NULL) == NULL);
    CHECK("null_number", json_number(NULL, 1.25) == 1.25);
    CHECK("null_bool", json_bool(NULL, 1) == 1);
    json_free(NULL);
    CHECK("null_free_survives", 1);
}

/* A cut-down radio-browser.info /json/stations response. */
static const char RB[] =
"[\n"
"  {\n"
"    \"stationuuid\": \"9617a958-0601-11e8-ae97-52543be04c81\",\n"
"    \"name\": \"Radio Caf\\u00e9 \\u2013 Paris\",\n"
"    \"url\": \"http:\\/\\/example.fr\\/live.mp3\",\n"
"    \"url_resolved\": \"http:\\/\\/cdn.example.fr\\/live.mp3\",\n"
"    \"codec\": \"MP3\",\n"
"    \"bitrate\": 128,\n"
"    \"hls\": 0,\n"
"    \"countrycode\": \"FR\"\n"
"  },\n"
"  {\n"
"    \"stationuuid\": \"961893b2-0601-11e8-ae97-52543be04c81\",\n"
"    \"name\": \"Tokyo HLS\",\n"
"    \"url\": \"https:\\/\\/example.jp\\/master.m3u8\",\n"
"    \"url_resolved\": \"https:\\/\\/cdn.example.jp\\/master.m3u8\",\n"
"    \"codec\": \"AAC\",\n"
"    \"bitrate\": 0,\n"
"    \"hls\": 1,\n"
"    \"countrycode\": \"JP\"\n"
"  }\n"
"]";

static void test_radio_browser_fixture(void)
{
    JsonValue *v = parse_exact(RB, sizeof(RB) - 1);
    const JsonValue *s0, *s1;

    CHECK("rb_is_array_of_2", v && json_type(v) == JSON_ARRAY && json_array_count(v) == 2);
    if (!v) { json_free(v); return; }

    s0 = json_array_at(v, 0);
    s1 = json_array_at(v, 1);

    CHECK("rb0_uuid", STR_IS(json_object_get(s0, "stationuuid"),
                             "9617a958-0601-11e8-ae97-52543be04c81"));
    /* "Radio Caf<e9> <en dash> Paris": a 2-byte and a 3-byte escape in one name. */
    CHECK("rb0_accented_name", STR_IS(json_object_get(s0, "name"),
                                      "Radio Caf\xC3\xA9 \xE2\x80\x93 Paris"));
    CHECK("rb0_url", STR_IS(json_object_get(s0, "url"), "http://example.fr/live.mp3"));
    CHECK("rb0_url_resolved", STR_IS(json_object_get(s0, "url_resolved"),
                                     "http://cdn.example.fr/live.mp3"));
    CHECK("rb0_codec", STR_IS(json_object_get(s0, "codec"), "MP3"));
    CHECK("rb0_bitrate", json_number(json_object_get(s0, "bitrate"), -1) == 128);
    CHECK("rb0_hls", json_number(json_object_get(s0, "hls"), -1) == 0);
    CHECK("rb0_countrycode", STR_IS(json_object_get(s0, "countrycode"), "FR"));

    CHECK("rb1_uuid", STR_IS(json_object_get(s1, "stationuuid"),
                             "961893b2-0601-11e8-ae97-52543be04c81"));
    CHECK("rb1_name", STR_IS(json_object_get(s1, "name"), "Tokyo HLS"));
    CHECK("rb1_url", STR_IS(json_object_get(s1, "url"), "https://example.jp/master.m3u8"));
    CHECK("rb1_url_resolved", STR_IS(json_object_get(s1, "url_resolved"),
                                     "https://cdn.example.jp/master.m3u8"));
    CHECK("rb1_codec", STR_IS(json_object_get(s1, "codec"), "AAC"));
    CHECK("rb1_bitrate", json_number(json_object_get(s1, "bitrate"), -1) == 0);
    CHECK("rb1_hls", json_number(json_object_get(s1, "hls"), -1) == 1);
    CHECK("rb1_countrycode", STR_IS(json_object_get(s1, "countrycode"), "JP"));

    CHECK("rb_absent_field", json_object_get(s1, "favicon") == NULL);
    CHECK("rb_out_of_range", json_array_at(v, 2) == NULL);

    json_free(v);
}

int main(void)
{
    test_scalars();
    test_numbers();
    test_escapes();
    test_containers();
    test_whitespace();
    test_malformed();
    test_not_nul_terminated();
    test_depth();
    test_accessors();
    test_null_arguments();
    test_radio_browser_fixture();

    printf("test_json: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
