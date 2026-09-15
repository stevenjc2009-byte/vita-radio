#ifndef VR_RELEASE_JSON_H
#define VR_RELEASE_JSON_H

#include <stddef.h>

/* Pulls what the updater needs out of a GitHub "releases/latest" response:
 * tag_name, and the browser_download_url of the first asset ending in ".vpk".
 * Returns 0 when both were found, -1 otherwise. */
int release_json_parse(const char *json, char *tag, size_t tagsz, char *url, size_t urlsz);

#endif
