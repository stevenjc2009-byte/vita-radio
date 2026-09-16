#include "release_json.h"

#include <string.h>

/* Copies the JSON string value that follows *p (at a key's closing quote) into
 * out. Unescapes \" \\ \/; other escapes become '?'. Returns the position after
 * the value, or NULL if it isn't a string or doesn't fit; when it didn't fit,
 * *too_long (may be NULL) is set so the caller can say so. */
static const char *read_string_value(const char *p, char *out, size_t outsz, int *too_long)
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
        if (n + 1 >= outsz) {
            if (too_long)
                *too_long = 1;
            return NULL;
        }
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
    static const char HTTPS[] = "https://";
    const char *p;
    int tag_long = 0;

    if (!json || !tag || !url || !tagsz || !urlsz)
        return RELEASE_JSON_ERR;
    tag[0] = url[0] = '\0';

    p = strstr(json, TAG_KEY);
    if (!p || !read_string_value(p + sizeof(TAG_KEY) - 1, tag, tagsz, &tag_long) || !tag[0]) {
        tag[0] = '\0';
        return tag_long ? RELEASE_JSON_ERR_TAG_LONG : RELEASE_JSON_ERR;
    }

    /* The asset URL is the last point at which the scheme can be pinned: the
     * download follows redirects, so anything but https here is refused. */
    for (p = strstr(json, URL_KEY); p; p = strstr(p + 1, URL_KEY)) {
        if (read_string_value(p + sizeof(URL_KEY) - 1, url, urlsz, NULL) &&
            strncmp(url, HTTPS, sizeof(HTTPS) - 1) == 0 && ends_with(url, ".vpk"))
            return RELEASE_JSON_OK;
    }
    url[0] = '\0';
    return RELEASE_JSON_ERR;
}
