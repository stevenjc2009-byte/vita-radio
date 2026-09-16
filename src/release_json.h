#ifndef VR_RELEASE_JSON_H
#define VR_RELEASE_JSON_H

#include <stddef.h>

enum {
    RELEASE_JSON_OK           =  0,
    RELEASE_JSON_ERR          = -1,   /* malformed, or no usable .vpk asset */
    RELEASE_JSON_ERR_TAG_LONG = -2    /* tag_name found but didn't fit `tag` */
};

/* Pulls what the updater needs out of a GitHub "releases/latest" response:
 * tag_name, and the browser_download_url of the first asset that is an https
 * URL ending in ".vpk". Plain http and other schemes are refused, because the
 * download follows redirects and must stay on a verified TLS connection.
 * Returns RELEASE_JSON_OK when both were found, else a RELEASE_JSON_ERR*. */
int release_json_parse(const char *json, char *tag, size_t tagsz, char *url, size_t urlsz);

#endif
