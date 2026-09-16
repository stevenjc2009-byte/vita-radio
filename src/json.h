#ifndef VR_JSON_H
#define VR_JSON_H

#include <stddef.h>

/* Small read-only JSON DOM, enough for the radio-browser API.
 *
 * release_json.c's strstr approach cannot be reused: it finds two fixed keys in
 * a known-shaped document, and this has to walk an array of ~100 objects whose
 * string values contain escapes and, in station names, non-ASCII. */

typedef enum {
    JSON_NULL = 0, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT
} JsonType;

typedef struct JsonValue JsonValue;

/* Parses text (need not be NUL-terminated). Returns NULL on malformed input or
 * if the nesting exceeds a fixed depth limit - a network document must never be
 * able to blow the stack. */
JsonValue      *json_parse(const char *text, size_t len);
void            json_free(JsonValue *v);

JsonType        json_type(const JsonValue *v);

int             json_array_count(const JsonValue *v);      /* 0 if not an array */
const JsonValue*json_array_at(const JsonValue *v, int i);  /* NULL out of range */

/* NULL if v is not an object or the key is absent. */
const JsonValue*json_object_get(const JsonValue *v, const char *key);

/* Accessors that never fault: each returns the fallback unless the value is of
 * that exact type. Strings are decoded to UTF-8, \uXXXX and surrogate pairs
 * included, and stay owned by the tree. */
const char     *json_string(const JsonValue *v, const char *fallback);
double          json_number(const JsonValue *v, double fallback);
int             json_bool(const JsonValue *v, int fallback);

#endif
