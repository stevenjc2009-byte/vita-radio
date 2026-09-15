#include "release_json.h"

#include <string.h>

/* Copies the JSON string value that follows *p (at a key's closing quote) into
 * out. Unescapes \" \\ \/; other escapes become '?'. Returns the position after
 * the value, or NULL if it isn't a string or doesn't fit. */
static const char *read_string_value(const char *p, char *out, size_t outsz)
{
    size_t n = 0;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    if (*p++ != ':')
        return NULL;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    if (*p++ != '"')
        return NULL;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\') {
            c = *p++;
            if (c == 'u') {   /* \uXXXX */
                for (int i = 0; i < 4 && *p; i++)
                    p++;
                c = '?';
            } else if (c != '"' && c != '\\' && c != '/') {
                if (!c)
                    return NULL;
                c = '?';
            }
        }
        if (n + 1 >= outsz)
            return NULL;
        out[n++] = c;
    }
    if (*p != '"')
        return NULL;
    out[n] = '\0';
    return p + 1;
}

static int ends_with(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

int release_json_parse(const char *json, char *tag, size_t tagsz, char *url, size_t urlsz)
{
    static const char TAG_KEY[] = "\"tag_name\"";
    static const char URL_KEY[] = "\"browser_download_url\"";
    const char *p;

    if (!json || !tag || !url || !tagsz || !urlsz)
        return -1;
    tag[0] = url[0] = '\0';

    p = strstr(json, TAG_KEY);
    if (!p || !read_string_value(p + sizeof(TAG_KEY) - 1, tag, tagsz) || !tag[0]) {
        tag[0] = '\0';
        return -1;
    }

    for (p = strstr(json, URL_KEY); p; p = strstr(p + 1, URL_KEY)) {
        if (read_string_value(p + sizeof(URL_KEY) - 1, url, urlsz) && ends_with(url, ".vpk"))
            return 0;
    }
    url[0] = '\0';
    return -1;
}
