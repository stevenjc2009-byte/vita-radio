#include "json.h"

#include <stdlib.h>
#include <string.h>

/* Read-only JSON DOM for the radio-browser API.
 *
 * The documents come straight off the network, so the parser is written to be
 * unable to run away: every read is bounded by an explicit end pointer (the
 * text is not required to be NUL-terminated) and container nesting is capped,
 * which bounds the recursion in both parse_value() and json_free(). */

#define JSON_MAX_DEPTH 64

typedef struct {
    char      *key;
    JsonValue *val;
} JsonMember;

struct JsonValue {
    JsonType type;
    union {
        int    b;                                              /* JSON_BOOL   */
        double num;                                            /* JSON_NUMBER */
        char  *str;                                            /* JSON_STRING */
        struct { JsonValue **items; int count; int cap; } arr; /* JSON_ARRAY  */
        struct { JsonMember *mem;   int count; int cap; } obj; /* JSON_OBJECT */
    } u;
};

typedef struct {
    const char *p;
    const char *end;
    int         depth;
} Ctx;

static JsonValue *parse_value(Ctx *c);

/* ---- small helpers ------------------------------------------------- */

static int at_end(const Ctx *c)
{
    return c->p >= c->end;
}

static void skip_ws(Ctx *c)
{
    while (c->p < c->end &&
           (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r'))
        c->p++;
}

static int hex_val(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static JsonValue *new_value(JsonType t)
{
    JsonValue *v = (JsonValue *)calloc(1, sizeof *v);
    if (v)
        v->type = t;
    return v;
}

void json_free(JsonValue *v)
{
    int i;

    if (!v)
        return;

    switch (v->type) {
    case JSON_STRING:
        free(v->u.str);
        break;
    case JSON_ARRAY:
        for (i = 0; i < v->u.arr.count; i++)
            json_free(v->u.arr.items[i]);
        free(v->u.arr.items);
        break;
    case JSON_OBJECT:
        for (i = 0; i < v->u.obj.count; i++) {
            free(v->u.obj.mem[i].key);
            json_free(v->u.obj.mem[i].val);
        }
        free(v->u.obj.mem);
        break;
    default:
        break;
    }
    free(v);
}

/* ---- strings -------------------------------------------------------- */

/* Writes the UTF-8 form of cp (never a surrogate) and returns its length. */
static size_t utf8_put(char *out, unsigned cp)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Validates a quoted string at c->p and reports the raw span between the
 * quotes, leaving c->p just past the closing quote. Returns 0 if the string is
 * unterminated, holds a raw control character, or carries a bad escape. */
static int scan_string(Ctx *c, const char **startp, size_t *lenp)
{
    const char *p;

    if (at_end(c) || *c->p != '"')
        return 0;

    p = c->p + 1;
    *startp = p;

    while (p < c->end) {
        unsigned char ch = (unsigned char)*p;

        if (ch == '"') {
            *lenp = (size_t)(p - *startp);
            c->p = p + 1;
            return 1;
        }
        if (ch < 0x20)
            return 0;               /* raw control characters are not legal */
        if (ch != '\\') {
            p++;
            continue;
        }

        p++;                        /* the backslash */
        if (p >= c->end)
            return 0;
        switch (*p) {
        case '"': case '\\': case '/':
        case 'b': case 'f': case 'n': case 'r': case 't':
            p++;
            break;
        case 'u': {
            int k;
            p++;
            for (k = 0; k < 4; k++) {
                if (p >= c->end || hex_val(*p) < 0)
                    return 0;
                p++;
            }
            break;
        }
        default:
            return 0;
        }
    }
    return 0;                       /* ran out of input before the quote */
}

/* Decodes a span already validated by scan_string into a fresh NUL-terminated
 * UTF-8 buffer. No escape ever expands: "\uXXXX" is 6 bytes in and at most 3
 * out, a surrogate pair 12 in and 4 out, so len+1 is always enough. */
static char *decode_string(const char *s, size_t len)
{
    char  *out = (char *)malloc(len + 1);
    size_t i = 0, o = 0;

    if (!out)
        return NULL;

    while (i < len) {
        if (s[i] != '\\') {
            out[o++] = s[i++];
            continue;
        }
        i++;                        /* the backslash */
        switch (s[i]) {
        case '"':  out[o++] = '"';  i++; break;
        case '\\': out[o++] = '\\'; i++; break;
        case '/':  out[o++] = '/';  i++; break;
        case 'b':  out[o++] = '\b'; i++; break;
        case 'f':  out[o++] = '\f'; i++; break;
        case 'n':  out[o++] = '\n'; i++; break;
        case 'r':  out[o++] = '\r'; i++; break;
        case 't':  out[o++] = '\t'; i++; break;
        default: {
            /* 'u': scan_string guaranteed the four hex digits are there. */
            unsigned cp = (unsigned)((hex_val(s[i + 1]) << 12) |
                                     (hex_val(s[i + 2]) << 8)  |
                                     (hex_val(s[i + 3]) << 4)  |
                                      hex_val(s[i + 4]));
            i += 5;

            if (cp >= 0xD800 && cp <= 0xDBFF) {
                /* High surrogate: consume the low half only if it is really
                 * there, and only within the span - never past it. */
                unsigned lo = 0;
                if (i + 6 <= len && s[i] == '\\' && s[i + 1] == 'u')
                    lo = (unsigned)((hex_val(s[i + 2]) << 12) |
                                    (hex_val(s[i + 3]) << 8)  |
                                    (hex_val(s[i + 4]) << 4)  |
                                     hex_val(s[i + 5]));
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                    i += 6;
                } else {
                    cp = 0xFFFD;    /* unpaired high surrogate */
                }
            } else if (cp == 0 || (cp >= 0xDC00 && cp <= 0xDFFF)) {
                /* Stray low surrogate, or a NUL. json_string() returns a bare
                 * char* with no length, so a NUL here would end the value for
                 * every consumer - a station URL would be stored and dialled
                 * truncated. U+FFFD keeps the 6-in/3-out size invariant. */
                cp = 0xFFFD;
            }
            o += utf8_put(out + o, cp);
            break;
        }
        }
    }
    out[o] = '\0';
    return out;
}

static JsonValue *parse_string(Ctx *c)
{
    const char *s;
    size_t      n = 0;
    JsonValue  *v;

    if (!scan_string(c, &s, &n))
        return NULL;

    v = new_value(JSON_STRING);
    if (!v)
        return NULL;
    v->u.str = decode_string(s, n);
    if (!v->u.str) {
        free(v);
        return NULL;
    }
    return v;
}

/* ---- scalars -------------------------------------------------------- */

static JsonValue *parse_literal(Ctx *c, const char *lit, size_t n,
                                JsonType t, int b)
{
    JsonValue *v;

    if ((size_t)(c->end - c->p) < n || memcmp(c->p, lit, n) != 0)
        return NULL;

    v = new_value(t);
    if (!v)
        return NULL;
    v->u.b = b;
    c->p += n;
    return v;
}

/* Validates the JSON number grammar by hand, then hands a NUL-terminated copy
 * of exactly that token to strtod - strtod on the raw buffer could run past
 * the end of a document that is not NUL-terminated. */
static JsonValue *parse_number(Ctx *c)
{
    const char *s = c->p;
    const char *p = s;
    char        small[64];
    char       *buf;
    size_t      n;
    JsonValue  *v;

    if (p < c->end && *p == '-')
        p++;

    if (p >= c->end)
        return NULL;
    if (*p == '0') {
        p++;                                    /* no leading zeroes allowed */
    } else if (*p >= '1' && *p <= '9') {
        while (p < c->end && *p >= '0' && *p <= '9')
            p++;
    } else {
        return NULL;
    }

    if (p < c->end && *p == '.') {
        p++;
        if (p >= c->end || *p < '0' || *p > '9')
            return NULL;
        while (p < c->end && *p >= '0' && *p <= '9')
            p++;
    }

    if (p < c->end && (*p == 'e' || *p == 'E')) {
        p++;
        if (p < c->end && (*p == '+' || *p == '-'))
            p++;
        if (p >= c->end || *p < '0' || *p > '9')
            return NULL;
        while (p < c->end && *p >= '0' && *p <= '9')
            p++;
    }

    n = (size_t)(p - s);
    buf = (n < sizeof small) ? small : (char *)malloc(n + 1);
    if (!buf)
        return NULL;
    memcpy(buf, s, n);
    buf[n] = '\0';

    v = new_value(JSON_NUMBER);
    if (v)
        v->u.num = strtod(buf, NULL);
    if (buf != small)
        free(buf);

    if (!v)
        return NULL;
    c->p = p;
    return v;
}

/* ---- containers ----------------------------------------------------- */

static int arr_push(JsonValue *a, JsonValue *item)
{
    if (a->u.arr.count == a->u.arr.cap) {
        int         ncap = a->u.arr.cap ? a->u.arr.cap * 2 : 4;
        JsonValue **ni = (JsonValue **)realloc(a->u.arr.items,
                                               (size_t)ncap * sizeof *ni);
        if (!ni)
            return 0;
        a->u.arr.items = ni;
        a->u.arr.cap   = ncap;
    }
    a->u.arr.items[a->u.arr.count++] = item;
    return 1;
}

static int obj_push(JsonValue *o, char *key, JsonValue *val)
{
    if (o->u.obj.count == o->u.obj.cap) {
        int         ncap = o->u.obj.cap ? o->u.obj.cap * 2 : 8;
        JsonMember *nm = (JsonMember *)realloc(o->u.obj.mem,
                                               (size_t)ncap * sizeof *nm);
        if (!nm)
            return 0;
        o->u.obj.mem = nm;
        o->u.obj.cap = ncap;
    }
    o->u.obj.mem[o->u.obj.count].key = key;
    o->u.obj.mem[o->u.obj.count].val = val;
    o->u.obj.count++;
    return 1;
}

static JsonValue *parse_array(Ctx *c)
{
    JsonValue *a;

    c->p++;                                     /* '[' */
    a = new_value(JSON_ARRAY);
    if (!a)
        return NULL;

    skip_ws(c);
    if (!at_end(c) && *c->p == ']') {
        c->p++;
        return a;
    }

    for (;;) {
        JsonValue *item;

        skip_ws(c);
        item = parse_value(c);
        if (!item || !arr_push(a, item)) {
            json_free(item);
            json_free(a);
            return NULL;
        }

        skip_ws(c);
        if (at_end(c))
            break;
        if (*c->p == ',') {
            c->p++;
            continue;                           /* a trailing ',' then fails */
        }
        if (*c->p == ']') {
            c->p++;
            return a;
        }
        break;                                  /* two values, no separator */
    }

    json_free(a);
    return NULL;
}

static JsonValue *parse_object(Ctx *c)
{
    JsonValue *o;

    c->p++;                                     /* '{' */
    o = new_value(JSON_OBJECT);
    if (!o)
        return NULL;

    skip_ws(c);
    if (!at_end(c) && *c->p == '}') {
        c->p++;
        return o;
    }

    for (;;) {
        const char *ks;
        size_t      kn = 0;
        char       *key;
        JsonValue  *val;

        skip_ws(c);
        if (!scan_string(c, &ks, &kn))
            break;
        key = decode_string(ks, kn);
        if (!key)
            break;

        skip_ws(c);
        if (at_end(c) || *c->p != ':') {
            free(key);
            break;
        }
        c->p++;

        skip_ws(c);
        val = parse_value(c);
        if (!val || !obj_push(o, key, val)) {
            free(key);
            json_free(val);
            break;
        }

        skip_ws(c);
        if (at_end(c))
            break;
        if (*c->p == ',') {
            c->p++;
            continue;
        }
        if (*c->p == '}') {
            c->p++;
            return o;
        }
        break;
    }

    json_free(o);
    return NULL;
}

static JsonValue *parse_value(Ctx *c)
{
    if (at_end(c))
        return NULL;

    switch (*c->p) {
    case '{':
    case '[': {
        JsonValue *v;
        if (c->depth >= JSON_MAX_DEPTH)
            return NULL;            /* refuse rather than recurse any deeper */
        c->depth++;
        v = (*c->p == '{') ? parse_object(c) : parse_array(c);
        c->depth--;
        return v;
    }
    case '"':
        return parse_string(c);
    case 't':
        return parse_literal(c, "true", 4, JSON_BOOL, 1);
    case 'f':
        return parse_literal(c, "false", 5, JSON_BOOL, 0);
    case 'n':
        return parse_literal(c, "null", 4, JSON_NULL, 0);
    default:
        return parse_number(c);
    }
}

/* ---- public API ----------------------------------------------------- */

JsonValue *json_parse(const char *text, size_t len)
{
    Ctx        c;
    JsonValue *v;

    if (!text || len == 0)
        return NULL;

    c.p     = text;
    c.end   = text + len;
    c.depth = 0;

    skip_ws(&c);
    v = parse_value(&c);
    if (!v)
        return NULL;

    skip_ws(&c);
    if (c.p != c.end) {             /* trailing garbage is not half-accepted */
        json_free(v);
        return NULL;
    }
    return v;
}

JsonType json_type(const JsonValue *v)
{
    return v ? v->type : JSON_NULL;
}

int json_array_count(const JsonValue *v)
{
    return (v && v->type == JSON_ARRAY) ? v->u.arr.count : 0;
}

const JsonValue *json_array_at(const JsonValue *v, int i)
{
    if (!v || v->type != JSON_ARRAY || i < 0 || i >= v->u.arr.count)
        return NULL;
    return v->u.arr.items[i];
}

/* Members are kept in document order and scanned forwards, so a duplicated key
 * resolves to its first occurrence. */
const JsonValue *json_object_get(const JsonValue *v, const char *key)
{
    int i;

    if (!v || v->type != JSON_OBJECT || !key)
        return NULL;
    for (i = 0; i < v->u.obj.count; i++)
        if (strcmp(v->u.obj.mem[i].key, key) == 0)
            return v->u.obj.mem[i].val;
    return NULL;
}

const char *json_string(const JsonValue *v, const char *fallback)
{
    return (v && v->type == JSON_STRING) ? v->u.str : fallback;
}

double json_number(const JsonValue *v, double fallback)
{
    return (v && v->type == JSON_NUMBER) ? v->u.num : fallback;
}

int json_bool(const JsonValue *v, int fallback)
{
    return (v && v->type == JSON_BOOL) ? v->u.b : fallback;
}
