#include "rbrowser.h"

#include "http_get.h"
#include "json.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* radio-browser.info has no stable single hostname - the project's own docs say
 * any one mirror can and does go dark - so every request walks this list and
 * the one that answered last is tried first next time. */
static const char *const s_mirrors[] = {
    "de1.api.radio-browser.info",
    "de2.api.radio-browser.info",
    "nl1.api.radio-browser.info",
    "at1.api.radio-browser.info",
    "fi1.api.radio-browser.info",
};
#define RB_MIRROR_COUNT ((int)(sizeof(s_mirrors) / sizeof(s_mirrors[0])))

#define RB_DEFAULT_UA   "VitaRadio/2.0.0"
#define RB_ENC_LEN      800     /* 256 caller bytes can encode to 768 */
#define RB_PATH_LEN     1024
#define RB_URL_LEN      (RB_PATH_LEN + 64)
#define RB_ERR_LEN      160
#define RB_CLICK_MAX    8192    /* the click endpoint answers with one tiny object */
#define RB_BYTES_PER_ROW 4096   /* measured ~1.2 KB; this is headroom, not a guess */

/* Fixed buffers rather than strdup: rbrowser.h has no shutdown call to free
 * them in, and both are set once from main. */
static char s_user_agent[128] = RB_DEFAULT_UA;
static char s_ca_file[256];
static int  s_mirror;           /* index of the mirror that last answered */

static void set_err(char *err, size_t errsz, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void set_err(char *err, size_t errsz, const char *fmt, ...)
{
    va_list ap;
    if (!err || errsz == 0)
        return;
    va_start(ap, fmt);
    vsnprintf(err, errsz, fmt, ap);
    va_end(ap);
}

/* ---- strings -------------------------------------------------------------- */

static int is_unreserved(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
}

/* Percent-encodes everything outside RFC 3986's unreserved set. Deliberately
 * strict rather than clever about context: the same helper builds query values
 * ("rock & roll") and path segments (a tag with a slash in it), so anything
 * that means something in either place has to go. Returns 0, or -1 if the
 * encoded form would not fit. */
static int url_encode(const char *s, char *out, size_t outsz)
{
    static const char hex[] = "0123456789ABCDEF";
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    size_t o = 0;

    if (!out || outsz == 0)
        return -1;
    for (; *p; p++) {
        if (is_unreserved(*p)) {
            if (o + 1 >= outsz)
                return -1;
            out[o++] = (char)*p;
        } else {
            if (o + 3 >= outsz)
                return -1;
            out[o++] = '%';
            out[o++] = hex[*p >> 4];
            out[o++] = hex[*p & 0x0F];
        }
    }
    out[o] = '\0';
    return 0;
}

/* Largest length <= n that does not end inside a UTF-8 sequence. Station names
 * are full of accents and a half character is not text the font can draw. */
static size_t utf8_trim(const char *s, size_t n)
{
    size_t lead_at = n, need;
    unsigned char lead;

    while (lead_at > 0 && ((unsigned char)s[lead_at - 1] & 0xC0) == 0x80)
        lead_at--;
    if (lead_at == 0)
        return 0;                   /* continuation bytes all the way down */
    lead = (unsigned char)s[lead_at - 1];
    if (lead < 0x80)
        return n;                   /* ends on an ASCII byte: already a boundary */
    lead_at--;
    if ((lead & 0xE0) == 0xC0)
        need = 2;
    else if ((lead & 0xF0) == 0xE0)
        need = 3;
    else if ((lead & 0xF8) == 0xF0)
        need = 4;
    else
        return lead_at;             /* not a lead byte at all - drop it */
    return (n - lead_at >= need) ? n : lead_at;
}

/* Copies into a fixed field, truncating on a character boundary and always
 * terminating. Returns 0, or -1 if the value did not fit: a station with a very
 * long name is worth showing truncated, but a truncated URL or uuid is a
 * different address and a different station, so the caller has to be able to
 * tell the two cases apart.
 *
 * Tabs and newlines become spaces here rather than at save time. favourites.tsv
 * makes the same substitution on the way out (favourites.c fput_field), so
 * doing it on the way in is what stops the in-memory name and the persisted one
 * from disagreeing after a reload. */
static int copy_field(char *dst, size_t dstsz, const char *src)
{
    size_t n, i;
    int fits;

    if (!src)
        src = "";
    n = strlen(src);
    fits = (n < dstsz);
    if (!fits)
        n = utf8_trim(src, dstsz - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
    for (i = 0; i < n; i++)
        if (dst[i] == '\t' || dst[i] == '\n' || dst[i] == '\r')
            dst[i] = ' ';
    return fits ? 0 : -1;
}

/* ---- JSON field access ---------------------------------------------------- */

/* "" for a missing key, a null, or a value that is not a string. Every caller
 * wants a readable field, not the difference between those three. */
static const char *field_str(const JsonValue *obj, const char *key)
{
    const JsonValue *v = json_object_get(obj, key);

    if (!v || json_type(v) != JSON_STRING)
        return "";
    return json_string(v, "");
}

/* bitrate and hls are documented as numbers, but rows carry them as strings and
 * as booleans too. Take any of those; 0 for anything else. */
static int field_int(const JsonValue *obj, const char *key)
{
    const JsonValue *v = json_object_get(obj, key);
    long n;

    if (!v)
        return 0;
    switch (json_type(v)) {
    case JSON_NUMBER: {
        /* Range-check before the cast, not after: JSON has no bound on an
         * exponent, so "1e999" arrives as an infinity and casting that to long
         * is undefined - LONG_MIN here, a saturated INT32_MAX on ARM VFP. */
        double d = json_number(v, 0.0);
        if (!(d > 0.0 && d <= 1000000.0))
            return 0;
        n = (long)d;
        break;
    }
    case JSON_BOOL:
        n = json_bool(v, 0);
        break;
    case JSON_STRING:
        n = strtol(json_string(v, "0"), NULL, 10);
        break;
    default:
        return 0;
    }
    /* Anything outside this is somebody's typo, not a bitrate. */
    return (n > 0 && n <= 1000000L) ? (int)n : 0;
}

/* ---- transport ------------------------------------------------------------ */

/* ~1.2 KB of JSON per station measured. Sized from the request rather than left
 * to http_get's 1 MiB default so a runaway response is cut off early. */
static size_t body_budget(int limit)
{
    size_t want = (size_t)limit * RB_BYTES_PER_ROW + 8192u;

    return want > VR_HTTP_GET_MAX ? VR_HTTP_GET_MAX : want;
}

/* Walks the mirrors, last good one first, and stops at the first that answers.
 * http_get already fails anything that is not a 2xx with a body, so a transport
 * error and a bad status both arrive here as -1 and both move us on: there is
 * nothing mirror-specific about this API's 4xx, and a mirror that is dark
 * usually presents as one or the other depending on how it died. */
static int fetch_path(const char *path, size_t max_bytes, HttpDoc *doc,
                      char *err, size_t errsz)
{
    char url[RB_URL_LEN], last[RB_ERR_LEN];
    int i;

    last[0] = '\0';
    for (i = 0; i < RB_MIRROR_COUNT; i++) {
        int m = (s_mirror + i) % RB_MIRROR_COUNT;
        char one[RB_ERR_LEN];

        one[0] = '\0';
        snprintf(url, sizeof(url), "https://%s%s", s_mirrors[m], path);
        if (http_get(url, s_user_agent, s_ca_file[0] ? s_ca_file : NULL,
                     max_bytes, doc, one, sizeof(one)) == 0) {
            s_mirror = m;
            return 0;
        }
        snprintf(last, sizeof(last), "%s (%s)", s_mirrors[m],
                 one[0] ? one : "request failed");
    }
    set_err(err, errsz, "no radio-browser mirror answered - last tried %s", last);
    return -1;
}

/* ---- results -------------------------------------------------------------- */

static void result_reset(RbResult *r)
{
    if (r) {
        r->items = NULL;
        r->count = 0;
    }
}

static int clamp_limit(int limit)
{
    if (limit < 1)
        return 1;
    return limit > RB_MAX_RESULTS ? RB_MAX_RESULTS : limit;
}

/* Fills out from a parsed response. Rows with no URL at all are skipped - the
 * UI can do nothing with a station it cannot play - but a row missing anything
 * else is kept, because codec, bitrate and country are user-submitted and are
 * wrong or absent often enough that dropping on them would empty the list.
 *
 * A URL or a uuid that does not fit its field is skipped for the same reason as
 * a missing one: truncated, it is a different address and a different station,
 * so keeping the row would dial the wrong stream and register the click against
 * somebody else. Name, codec and country are only ever displayed, so a short
 * version of those is still useful and is kept. */
static int build_result(const JsonValue *root, int limit, RbResult *out,
                        char *err, size_t errsz)
{
    RbStation *items;
    int n, i, kept = 0;

    if (json_type(root) != JSON_ARRAY) {
        set_err(err, errsz, "radio-browser sent %s where a station list belongs",
                json_type(root) == JSON_OBJECT ? "an object" : "a single value");
        return -1;
    }
    n = json_array_count(root);
    if (n > limit)
        n = limit;
    if (n <= 0)
        return 0;               /* an empty array is a valid "no matches" */

    items = calloc((size_t)n, sizeof(*items));
    if (!items) {
        set_err(err, errsz, "out of memory");
        return -1;
    }
    for (i = 0; i < n; i++) {
        const JsonValue *o = json_array_at(root, i);
        const char *url;
        RbStation *st;

        if (!o || json_type(o) != JSON_OBJECT)
            continue;
        /* url_resolved is the post-redirect address and is the one that plays;
         * url is the fallback for rows the API has not resolved yet. */
        url = field_str(o, "url_resolved");
        if (!*url)
            url = field_str(o, "url");
        if (!*url)
            continue;

        st = &items[kept];
        if (copy_field(st->url, sizeof(st->url), url) != 0)
            continue;
        if (copy_field(st->uuid, sizeof(st->uuid), field_str(o, "stationuuid")) != 0)
            continue;
        copy_field(st->name, sizeof(st->name), field_str(o, "name"));
        copy_field(st->codec, sizeof(st->codec), field_str(o, "codec"));
        copy_field(st->country, sizeof(st->country), field_str(o, "countrycode"));
        st->bitrate = field_int(o, "bitrate");
        st->is_hls = field_int(o, "hls") != 0;
        kept++;
    }
    if (kept == 0) {
        free(items);
        return 0;
    }
    out->items = items;
    out->count = kept;
    return 0;
}

static int rb_fetch(const char *path, int limit, RbResult *out,
                    char *err, size_t errsz)
{
    HttpDoc doc;
    JsonValue *root;
    int rc;

    if (!out) {
        set_err(err, errsz, "no output");
        return -1;
    }
    if (fetch_path(path, body_budget(limit), &doc, err, errsz) != 0)
        return -1;

    root = json_parse(doc.data, doc.len);
    if (!root) {
        set_err(err, errsz, "radio-browser sent %lu bytes that are not JSON",
                (unsigned long)doc.len);
        http_doc_free(&doc);
        return -1;
    }
    rc = build_result(root, limit, out, err, errsz);
    json_free(root);
    http_doc_free(&doc);
    return rc;
}

/* ---- public --------------------------------------------------------------- */

int rb_search_name(const char *query, int limit, RbResult *out, char *err, size_t errsz)
{
    char enc[RB_ENC_LEN], path[RB_PATH_LEN];

    result_reset(out);
    if (!query || !*query) {
        set_err(err, errsz, "nothing to search for");
        return -1;
    }
    if (url_encode(query, enc, sizeof(enc)) != 0) {
        set_err(err, errsz, "search text is too long");
        return -1;
    }
    limit = clamp_limit(limit);
    snprintf(path, sizeof(path),
             "/json/stations/search?name=%s&limit=%d"
             "&hidebroken=true&order=clickcount&reverse=true", enc, limit);
    return rb_fetch(path, limit, out, err, errsz);
}

int rb_top_click(int limit, RbResult *out, char *err, size_t errsz)
{
    char path[RB_PATH_LEN];

    result_reset(out);
    limit = clamp_limit(limit);
    snprintf(path, sizeof(path), "/json/stations/topclick/%d", limit);
    return rb_fetch(path, limit, out, err, errsz);
}

int rb_by_country(const char *iso2, int limit, RbResult *out, char *err, size_t errsz)
{
    char cc[3], path[RB_PATH_LEN];
    int i;

    result_reset(out);
    if (!iso2 || strlen(iso2) != 2) {
        set_err(err, errsz, "country code must be two letters");
        return -1;
    }
    /* bycountrycodeexact matches exactly and the database stores alpha-2 codes
     * upper case, so "gb" has to become "GB" or it silently finds nothing. */
    for (i = 0; i < 2; i++) {
        char c = iso2[i];
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        if (c < 'A' || c > 'Z') {
            set_err(err, errsz, "\"%s\" is not a country code", iso2);
            return -1;
        }
        cc[i] = c;
    }
    cc[2] = '\0';

    limit = clamp_limit(limit);
    snprintf(path, sizeof(path),
             "/json/stations/bycountrycodeexact/%s?limit=%d"
             "&hidebroken=true&order=clickcount&reverse=true", cc, limit);
    return rb_fetch(path, limit, out, err, errsz);
}

int rb_by_tag(const char *tag, int limit, RbResult *out, char *err, size_t errsz)
{
    char enc[RB_ENC_LEN], path[RB_PATH_LEN];

    result_reset(out);
    if (!tag || !*tag) {
        set_err(err, errsz, "no tag given");
        return -1;
    }
    if (url_encode(tag, enc, sizeof(enc)) != 0) {
        set_err(err, errsz, "tag is too long");
        return -1;
    }
    limit = clamp_limit(limit);
    snprintf(path, sizeof(path),
             "/json/stations/bytagexact/%s?limit=%d"
             "&hidebroken=true&order=clickcount&reverse=true", enc, limit);
    return rb_fetch(path, limit, out, err, errsz);
}

void rb_result_free(RbResult *r)
{
    if (!r)
        return;
    free(r->items);
    r->items = NULL;
    r->count = 0;
}

void rb_register_click(const char *uuid)
{
    char enc[128], path[192], err[RB_ERR_LEN];
    HttpDoc doc;

    if (!uuid || !*uuid)
        return;
    if (url_encode(uuid, enc, sizeof(enc)) != 0)
        return;
    snprintf(path, sizeof(path), "/json/url/%s", enc);
    /* rbrowser.h: best effort, never surfaced. The error is not thrown away
     * though - fetch_path acts on it by moving to the next mirror, which is the
     * only handling a void function can do. */
    if (fetch_path(path, RB_CLICK_MAX, &doc, err, sizeof(err)) == 0)
        http_doc_free(&doc);
}

void rb_set_user_agent(const char *ua)
{
    /* The API asks for a descriptive agent and the built-in default is a real
     * one, so an empty argument keeps it rather than sending nothing. */
    if (ua && *ua)
        snprintf(s_user_agent, sizeof(s_user_agent), "%s", ua);
}

void rb_set_ca_file(const char *path)
{
    snprintf(s_ca_file, sizeof(s_ca_file), "%s", path ? path : "");
}
