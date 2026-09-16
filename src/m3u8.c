#include "m3u8.h"

#include "url_util.h"

#include <stdlib.h>
#include <string.h>

/* RFC 8216 playlist parsing. Text in, structs out: no I/O, no allocation of
 * anything the caller does not own through the M3u8 it passed in. */

#define M3U8_MAX_URL  2048
#define ATTR_KEY_MAX    64
#define ATTR_VAL_MAX  1024
#define METHOD_MAX      32
#define IV_TEXT_MAX     64

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char  *d = malloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

static void copy_trunc(char *dst, size_t dstsz, const char *src)
{
    size_t n = strlen(src);
    if (n >= dstsz)
        n = dstsz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int is_space(char c)
{
    return c == ' ' || c == '\t';
}

/* Matches "#TAG" exactly or "#TAG:value"; *args is set to the value (empty for
 * the bare form). Deliberately strict so that "#EXT-X-DISCONTINUITY-SEQUENCE"
 * does not read as "#EXT-X-DISCONTINUITY". */
static int tag_match(const char *line, const char *tag, const char **args)
{
    size_t n = strlen(tag);

    if (strncmp(line, tag, n) != 0)
        return 0;
    if (line[n] == '\0') {
        *args = line + n;
        return 1;
    }
    if (line[n] == ':') {
        *args = line + n + 1;
        return 1;
    }
    return 0;
}

/* Walks a comma-separated attribute list. A quoted value may itself contain
 * commas (CODECS="mp4a.40.2,avc1.4d401f"), so the list cannot be split on ','.
 * Returns the scan position after this attribute, or NULL when the list ends. */
static const char *attr_next(const char *p,
                             char *key, size_t keysz,
                             char *val, size_t valsz)
{
    size_t w;

    if (!p)
        return NULL;
    while (*p && (is_space(*p) || *p == ','))
        p++;
    if (!*p)
        return NULL;

    w = 0;
    while (*p && *p != '=' && *p != ',') {
        if (w + 1 < keysz)
            key[w++] = *p;
        p++;
    }
    while (w > 0 && is_space(key[w - 1]))
        w--;
    key[w] = '\0';

    val[0] = '\0';
    if (*p != '=')
        return p;               /* attribute with no value */
    p++;

    w = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"') {
            if (w + 1 < valsz)
                val[w++] = *p;
            p++;
        }
        if (*p == '"')
            p++;
        while (*p && *p != ',')     /* tolerate junk between quote and comma */
            p++;
    } else {
        while (*p && *p != ',') {
            if (w + 1 < valsz)
                val[w++] = *p;
            p++;
        }
        while (w > 0 && is_space(val[w - 1]))
            w--;
    }
    val[w] = '\0';
    return p;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* "0x" followed by exactly 32 hex digits. */
static int parse_iv(const char *s, unsigned char iv[16])
{
    int i;

    if (s[0] != '0' || (s[1] != 'x' && s[1] != 'X'))
        return -1;
    s += 2;
    if (strlen(s) != 32)
        return -1;
    for (i = 0; i < 16; i++) {
        int hi = hex_val(s[2 * i]);
        int lo = hex_val(s[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        iv[i] = (unsigned char)((hi << 4) | lo);
    }
    return 0;
}

/* Default IV: the media sequence number, big-endian, zero-padded to 16 bytes. */
static void seq_iv(long long seq, unsigned char iv[16])
{
    unsigned long long v = (unsigned long long)seq;
    int i;

    memset(iv, 0, 16);
    for (i = 0; i < 8; i++)
        iv[15 - i] = (unsigned char)((v >> (8 * i)) & 0xFFu);
}

static int push_variant(M3u8 *out, int *cap, const M3u8Variant *v)
{
    if (out->variant_count >= *cap) {
        int          nc = *cap ? *cap * 2 : 8;
        M3u8Variant *np = realloc(out->variants, (size_t)nc * sizeof(*np));
        if (!np)
            return -1;
        out->variants = np;
        *cap = nc;
    }
    out->variants[out->variant_count++] = *v;
    return 0;
}

static int push_segment(M3u8 *out, int *cap, const M3u8Segment *s)
{
    if (out->segment_count >= *cap) {
        int          nc = *cap ? *cap * 2 : 8;
        M3u8Segment *np = realloc(out->segments, (size_t)nc * sizeof(*np));
        if (!np)
            return -1;
        out->segments = np;
        *cap = nc;
    }
    out->segments[out->segment_count++] = *s;
    return 0;
}

int m3u8_parse(const char *text, size_t len, const char *base_url, M3u8 *out)
{
    char     *buf, *p;
    size_t    off = 0;
    int       rc = -1, seen_header = 0;
    int       vcap = 0, scap = 0;
    int       have_extinf = 0, have_streaminf = 0, pending_disc = 0;
    double    pending_dur = 0.0;
    long      pending_bw = 0;
    char      pending_codecs[sizeof(((M3u8Variant *)0)->codecs)];
    long long next_seq = 0;
    /* The EXT-X-KEY in force for the segments that follow it. */
    int           key_enc = 0, key_have_iv = 0;
    char         *key_uri = NULL;
    unsigned char key_iv[16];

    if (!out)
        return -1;
    memset(out, 0, sizeof(*out));
    if (!text)
        return -1;

    buf = malloc(len + 1);
    if (!buf)
        return -1;
    memcpy(buf, text, len);
    buf[len] = '\0';

    pending_codecs[0] = '\0';
    memset(key_iv, 0, sizeof(key_iv));

    if (len >= 3 && (unsigned char)buf[0] == 0xEF &&
                    (unsigned char)buf[1] == 0xBB &&
                    (unsigned char)buf[2] == 0xBF)
        off = 3;                                    /* UTF-8 BOM */

    p = buf + off;
    while (p) {
        char  *line = p;
        char  *nl = strchr(p, '\n');
        size_t l;

        if (nl) {
            *nl = '\0';
            p = nl + 1;
        } else {
            p = NULL;
        }

        l = strlen(line);
        while (l > 0 && (line[l - 1] == '\r' || is_space(line[l - 1])))
            line[--l] = '\0';
        while (is_space(*line))
            line++;

        if (*line == '\0')
            continue;

        if (!seen_header) {
            if (strncmp(line, "#EXTM3U", 7) != 0)
                goto done;              /* not a playlist at all */
            seen_header = 1;
            continue;
        }

        if (*line == '#') {
            const char *a;

            if (tag_match(line, "#EXTINF", &a)) {
                pending_dur = strtod(a, NULL);
                have_extinf = 1;
            } else if (tag_match(line, "#EXT-X-STREAM-INF", &a)) {
                char k[ATTR_KEY_MAX], v[ATTR_VAL_MAX];

                pending_bw = 0;
                pending_codecs[0] = '\0';
                while ((a = attr_next(a, k, sizeof(k), v, sizeof(v))) != NULL) {
                    if (strcmp(k, "BANDWIDTH") == 0)
                        pending_bw = strtol(v, NULL, 10);
                    else if (strcmp(k, "CODECS") == 0)
                        copy_trunc(pending_codecs, sizeof(pending_codecs), v);
                }
                have_streaminf = 1;
                out->kind = VR_M3U8_MASTER;
            } else if (tag_match(line, "#EXT-X-TARGETDURATION", &a)) {
                out->target_duration = strtod(a, NULL);
                if (out->kind == VR_M3U8_UNKNOWN)
                    out->kind = VR_M3U8_MEDIA;
            } else if (tag_match(line, "#EXT-X-MEDIA-SEQUENCE", &a)) {
                out->media_sequence = strtoll(a, NULL, 10);
                next_seq = out->media_sequence;
            } else if (tag_match(line, "#EXT-X-DISCONTINUITY", &a)) {
                pending_disc = 1;
            } else if (tag_match(line, "#EXT-X-ENDLIST", &a)) {
                out->endlist = 1;
            } else if (tag_match(line, "#EXT-X-KEY", &a)) {
                char k[ATTR_KEY_MAX], v[ATTR_VAL_MAX];
                char method[METHOD_MAX], uri[M3U8_MAX_URL], iv_text[IV_TEXT_MAX];

                method[0] = uri[0] = iv_text[0] = '\0';
                while ((a = attr_next(a, k, sizeof(k), v, sizeof(v))) != NULL) {
                    if (strcmp(k, "METHOD") == 0)
                        copy_trunc(method, sizeof(method), v);
                    else if (strcmp(k, "URI") == 0)
                        copy_trunc(uri, sizeof(uri), v);
                    else if (strcmp(k, "IV") == 0)
                        copy_trunc(iv_text, sizeof(iv_text), v);
                }

                free(key_uri);
                key_uri = NULL;
                key_have_iv = 0;

                if (method[0] == '\0' || strcmp(method, "NONE") == 0) {
                    key_enc = 0;
                } else if (strcmp(method, "AES-128") == 0) {
                    char abs_uri[M3U8_MAX_URL];

                    key_enc = 1;
                    if (uri[0] &&
                        url_resolve(base_url, uri, abs_uri, sizeof(abs_uri)) == 0)
                        key_uri = dup_str(abs_uri);
                    if (iv_text[0] && parse_iv(iv_text, key_iv) == 0)
                        key_have_iv = 1;
                } else {
                    /* A method this parser cannot do (SAMPLE-AES, ...): the
                     * segments are encrypted but there is no usable key, so
                     * the caller sees encrypted with key_uri NULL and can
                     * refuse cleanly instead of playing noise. */
                    key_enc = 1;
                }
            }
            /* Anything else - "##" comments, EXT-X-VERSION, tags added after
             * this was written - is ignored by design (RFC 8216 4.1). */
            continue;
        }

        /* A URI line: belongs to the tag that came immediately before it. */
        if (have_streaminf || have_extinf) {
            char abs_uri[M3U8_MAX_URL];

            if (url_resolve(base_url, line, abs_uri, sizeof(abs_uri)) != 0) {
                /* Cannot be made absolute; drop it rather than hand back a
                 * URI the caller cannot fetch. */
                have_streaminf = have_extinf = pending_disc = 0;
                continue;
            }

            if (have_streaminf) {
                M3u8Variant v;

                memset(&v, 0, sizeof(v));
                v.uri = dup_str(abs_uri);
                v.bandwidth = pending_bw;
                copy_trunc(v.codecs, sizeof(v.codecs), pending_codecs);
                if (!v.uri || push_variant(out, &vcap, &v) != 0) {
                    free(v.uri);
                    goto done;
                }
                have_streaminf = 0;
            } else {
                M3u8Segment s;

                memset(&s, 0, sizeof(s));
                s.uri = dup_str(abs_uri);
                s.duration = pending_dur;
                s.seq = next_seq++;
                s.discontinuity = pending_disc;
                s.encrypted = key_enc;
                s.key_uri = key_uri ? dup_str(key_uri) : NULL;
                if (key_have_iv)
                    memcpy(s.iv, key_iv, sizeof(s.iv));
                else
                    seq_iv(s.seq, s.iv);
                if (!s.uri || push_segment(out, &scap, &s) != 0) {
                    free(s.uri);
                    free(s.key_uri);
                    goto done;
                }
                have_extinf = 0;
                pending_disc = 0;
            }
        }
    }

    /* Input with no lines at all (empty, blank-only, a lone BOM) never reaches
     * the check inside the loop, so the header is confirmed here too. */
    rc = seen_header ? 0 : -1;

done:
    free(buf);
    free(key_uri);
    if (rc != 0)
        m3u8_free(out);
    return rc;
}

void m3u8_free(M3u8 *p)
{
    int i;

    if (!p)
        return;
    for (i = 0; i < p->variant_count; i++)
        free(p->variants[i].uri);
    free(p->variants);
    for (i = 0; i < p->segment_count; i++) {
        free(p->segments[i].uri);
        free(p->segments[i].key_uri);
    }
    free(p->segments);
    memset(p, 0, sizeof(*p));
}

int m3u8_pick_variant(const M3u8 *master)
{
    int i, best = -1;

    if (!master || !master->variants || master->variant_count <= 0)
        return -1;
    for (i = 0; i < master->variant_count; i++) {
        if (master->variants[i].bandwidth <= 0)
            continue;               /* no BANDWIDTH declared */
        if (best < 0 || master->variants[i].bandwidth < master->variants[best].bandwidth)
            best = i;
    }
    /* If nothing declared a BANDWIDTH there is nothing to rank on: the first
     * variant is as good a guess as any. */
    return best >= 0 ? best : 0;
}
