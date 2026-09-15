/* Host tests for the updater's pure logic: version comparison and parsing a
 * GitHub releases/latest response. */
#include "release_json.h"
#include "version.h"

#include <stdio.h>
#include <string.h>

static int passed, failed;

#define CHECK(cond, name) do { \
    if (cond) { passed++; } else { failed++; printf("FAIL: %s\n", name); } \
} while (0)

#define SIGN(x) ((x) > 0 ? 1 : (x) < 0 ? -1 : 0)

static void test_versions(void)
{
    CHECK(SIGN(version_compare("1.0.0", "1.0.0")) == 0, "equal");
    CHECK(SIGN(version_compare("v1.0.0", "1.0.0")) == 0, "leading v ignored");
    CHECK(SIGN(version_compare("V1.0.1", "v1.0.0")) == 1, "patch newer");
    CHECK(SIGN(version_compare("1.0.9", "1.0.10")) == -1, "numeric not string order");
    CHECK(SIGN(version_compare("1.10.0", "1.9.9")) == 1, "minor numeric");
    CHECK(SIGN(version_compare("2.0", "1.9.9")) == 1, "major wins");
    CHECK(SIGN(version_compare("1.0", "1.0.0")) == 0, "missing part is 0");
    CHECK(SIGN(version_compare("1.0.0", "1.0.0.1")) == -1, "extra part newer");
    CHECK(SIGN(version_compare("1.0.1-beta", "1.0.1")) == 0, "suffix ignored");
    CHECK(SIGN(version_compare("0.9.0", VR_VERSION)) == -1, "older than app");
    CHECK(version_valid("v1.0.0") && version_valid("3"), "valid");
    CHECK(!version_valid("latest") && !version_valid("") && !version_valid(NULL) && !version_valid("v"),
          "invalid");
}

static const char SAMPLE[] =
    "{\n"
    "  \"url\": \"https://api.github.com/repos/o/r/releases/1\",\n"
    "  \"tag_name\" : \"v1.2.3\",\n"
    "  \"name\": \"Vita Radio 1.2.3 \\\"quoted\\\"\",\n"
    "  \"assets\": [\n"
    "    { \"name\": \"notes.txt\",\n"
    "      \"browser_download_url\": \"https://github.com/o/r/releases/download/v1.2.3/notes.txt\" },\n"
    "    { \"name\": \"VitaRadio.vpk\",\n"
    "      \"browser_download_url\": \"https:\\/\\/github.com\\/o\\/r\\/releases\\/download\\/v1.2.3\\/VitaRadio.vpk\" }\n"
    "  ]\n"
    "}\n";

static void test_json(void)
{
    char tag[32], url[256];

    CHECK(release_json_parse(SAMPLE, tag, sizeof(tag), url, sizeof(url)) == 0, "sample parses");
    CHECK(strcmp(tag, "v1.2.3") == 0, "tag value");
    CHECK(strcmp(url, "https://github.com/o/r/releases/download/v1.2.3/VitaRadio.vpk") == 0,
          "skips non-vpk asset, unescapes slashes");

    CHECK(release_json_parse("{\"tag_name\":\"v1.0.0\",\"assets\":[]}", tag, sizeof(tag), url, sizeof(url)) == -1,
          "no vpk asset fails");
    CHECK(url[0] == '\0', "url cleared on failure");
    CHECK(release_json_parse("{\"message\":\"API rate limit exceeded\"}", tag, sizeof(tag), url, sizeof(url)) == -1,
          "rate-limit body fails");
    CHECK(tag[0] == '\0', "tag cleared on failure");
    CHECK(release_json_parse("{\"tag_name\":\"v1.0.0\"", tag, 4, url, sizeof(url)) == -1, "tag too long fails");
    CHECK(release_json_parse("{\"tag_name\":\"v1.0.0", tag, sizeof(tag), url, sizeof(url)) == -1,
          "unterminated string fails");
    CHECK(release_json_parse("{\"tag_name\":5}", tag, sizeof(tag), url, sizeof(url)) == -1, "non-string tag fails");
    CHECK(release_json_parse(NULL, tag, sizeof(tag), url, sizeof(url)) == -1, "NULL json");
}

int main(void)
{
    test_versions();
    test_json();
    printf("test_updater_logic: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
