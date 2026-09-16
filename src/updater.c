#include "updater.h"
#include "fs_util.h"
#include "pkg_meta.h"
#include "promote.h"
#include "release_json.h"
#include "version.h"
#include "zip_extract.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <psp2/appmgr.h>
#include <psp2/io/devctl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/rng.h>
#include <psp2/kernel/threadmgr.h>

#define RELEASE_API     "https://api.github.com/repos/stevenjc2009-byte/vita-radio/releases/latest"
#define DATA_DIR        "ux0:data/VitaRadio"
#define VPK_PATH        DATA_DIR "/update.vpk"
#define STAGE_DIR       DATA_DIR "/stage"
#define RESULT_PATH     DATA_DIR "/update_result.txt"
/* Our own staging directory, not ux0:data/pkg: that one belongs to the user,
 * who keeps VPKs there for VitaShell, and we delete the whole tree twice per
 * install. ScePromoterUtil promotes any directory it is handed. */
#define PKG_DIR         DATA_DIR "/pkg"
#define TOKEN_PATH      DATA_DIR "/update_token.bin"
#define PENDING_PATH    DATA_DIR "/update_pending.txt"
#define LAUNCH_FAIL     "launch "     /* marks a RESULT_PATH line we wrote ourselves */
#define HEADBIN_TMPL    "app0:assets/head.bin"
#define HELPER_EBOOT    "app0:updater/eboot.bin"
#define HELPER_SFO      "app0:updater/param.sfo"
#define MAIN_TITLE_ID   "VRAD00001"
#define HELPER_TITLE_ID "VRADUPDTR"

#define WORKER_STACK    (256 * 1024)
#define JSON_MAX        (512 * 1024)
#define VPK_MAX         (64L * 1024 * 1024)
#define COPY_CHUNK      (64 * 1024)
#define UNSAFE_HINT     "Enable Unsafe Homebrew in HENkaku Settings."

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static UpdateStatus    s_status;
static char            s_vpk_url[1024];
static char           *s_user_agent;
static char           *s_ca_file;
static pthread_t       s_thread;
static int             s_has_thread;
static atomic_int      s_busy;
static atomic_int      s_abort;

static void set_status(UpdateState state, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void set_status(UpdateState state, const char *fmt, ...)
{
    va_list ap;
    pthread_mutex_lock(&s_lock);
    s_status.state = state;
    va_start(ap, fmt);
    vsnprintf(s_status.message, sizeof(s_status.message), fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&s_lock);
}

/* Same lock as every other s_status write, so the UI can never read a stale
 * percentage from a previous attempt under a fresh download. */
static void clear_progress(void)
{
    pthread_mutex_lock(&s_lock);
    s_status.progress_pct = 0;
    pthread_mutex_unlock(&s_lock);
}

/* ---- files ---------------------------------------------------------------- */

/* Reads a whole small file. Returns bytes read, or -1 (also if it didn't fit). */
static long read_all(const char *path, uint8_t *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    size_t n = fread(buf, 1, cap, f);
    int bad = ferror(f) || (n == cap && fgetc(f) != EOF);
    fclose(f);
    return bad ? -1 : (long)n;
}

static int write_all(const char *path, const uint8_t *buf, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    size_t n = fwrite(buf, 1, len, f);
    return (fclose(f) != 0 || n != len) ? -1 : 0;
}

static int copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb"), *out = in ? fopen(dst, "wb") : NULL;
    uint8_t *buf = malloc(COPY_CHUNK);
    int ok = in && out && buf;
    while (ok) {
        size_t n = fread(buf, 1, COPY_CHUNK, in);
        if (n == 0) {
            ok = !ferror(in);
            break;
        }
        ok = fwrite(buf, 1, n, out) == n;
    }
    free(buf);
    if (in)
        fclose(in);
    if (out && fclose(out) != 0)
        ok = 0;
    return ok ? 0 : -1;
}

/* Writes <dir>/sce_sys/package/head.bin for the package staged in <dir>,
 * after checking its param.sfo really is want_title_id. */
static int write_head_bin(const char *dir, const char *want_title_id)
{
    uint8_t sfo[16 * 1024], tmpl[4096], out[4096];
    char path[FS_PATH_LEN], title_id[16], content_id[64];

    snprintf(path, sizeof(path), "%s/sce_sys/param.sfo", dir);
    long sfo_len = read_all(path, sfo, sizeof(sfo));
    if (sfo_len <= 0 ||
        sfo_get_string(sfo, (size_t)sfo_len, "TITLE_ID", title_id, sizeof(title_id)) != 0 ||
        strcmp(title_id, want_title_id) != 0)
        return -1;
    if (sfo_get_string(sfo, (size_t)sfo_len, "CONTENT_ID", content_id, sizeof(content_id)) != 0)
        content_id[0] = '\0';

    long tmpl_len = read_all(HEADBIN_TMPL, tmpl, sizeof(tmpl));
    if (tmpl_len <= 0 || headbin_make(tmpl, (size_t)tmpl_len, title_id, content_id, out) != 0)
        return -2;

    snprintf(path, sizeof(path), "%s/sce_sys/package", dir);
    fs_mkdir_p(path);
    snprintf(path, sizeof(path), "%s/sce_sys/package/head.bin", dir);
    return write_all(path, out, (size_t)tmpl_len) == 0 ? 0 : -3;
}

/* A package whose eboot.bin is missing or empty installs fine and then won't
 * launch - and since the updater lives inside the app being replaced, there is
 * no way back without a PC. Both files must be there before we promote. */
static int stage_looks_complete(const char *dir)
{
    struct stat sb;
    char path[FS_PATH_LEN];

    snprintf(path, sizeof(path), "%s/eboot.bin", dir);
    if (stat(path, &sb) != 0 || sb.st_size <= 0)
        return 0;
    snprintf(path, sizeof(path), "%s/sce_sys/param.sfo", dir);
    return stat(path, &sb) == 0 && sb.st_size > 0;
}

/* Writes the one-shot token that authorises the updater title to promote this
 * package, and only this one. Must land before the package moves to PKG_DIR. */
static int write_update_token(const char *dir, const char *tag)
{
    uint8_t sfo[16 * 1024], nonce[16];
    char path[FS_PATH_LEN];
    UpdateToken t;

    snprintf(path, sizeof(path), "%s/sce_sys/param.sfo", dir);
    long n = read_all(path, sfo, sizeof(sfo));
    if (n <= 0 || sceKernelGetRandomNumber(nonce, sizeof(nonce)) < 0)
        return -1;
    update_token_build(&t, tag, nonce, sfo, (size_t)n);
    return write_all(TOKEN_PATH, (const uint8_t *)&t, sizeof(t));
}

/* Free bytes on ux0, or -1 when the device won't say. */
static int64_t ux0_free_bytes(void)
{
    SceIoDevInfo info;
    memset(&info, 0, sizeof(info));
    if (sceIoDevctl("ux0:", 0x3001, NULL, 0, &info, sizeof(info)) < 0)
        return -1;
    return (int64_t)info.free_size;
}

/* ---- network -------------------------------------------------------------- */

typedef struct {
    char   *data;
    size_t  len;
} MemBuf;

static size_t mem_write(char *data, size_t size, size_t nmemb, void *user)
{
    MemBuf *m = user;
    size_t n = size * nmemb;
    if (m->len + n + 1 > JSON_MAX)
        return 0;
    memcpy(m->data + m->len, data, n);
    m->len += n;
    m->data[m->len] = '\0';
    return n;
}

static int progress_cb(void *user, curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t ultotal, curl_off_t ulnow)
{
    (void)ultotal; (void)ulnow;
    sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);   /* don't sleep mid-download */
    /* dltotal is 0 when the server sent no Content-Length; leaving the guard in
     * place leaves progress_pct at the 0 clear_progress() set, and the UI draws
     * an indeterminate bar rather than a percentage of an unknown total. */
    if (user && dltotal > 0) {
        /* int64_t rather than curl_off_t: curl_off_t is whatever the backend's
         * headers say, and the two TLS backends compile against different ones
         * (see the Makefile). A 32-bit intermediate wraps partway through a
         * 64 MB VPK, so pin a width that cannot. */
        int64_t p = (int64_t)dlnow * 100 / (int64_t)dltotal;
        /* A server may send more bytes than it promised - don't report 147%. */
        int pct = p < 0 ? 0 : (p > 100 ? 100 : (int)p);
        pthread_mutex_lock(&s_lock);
        char tag[sizeof(s_status.latest)];
        snprintf(tag, sizeof(tag), "%s", s_status.latest);
        snprintf(s_status.message, sizeof(s_status.message), "Downloading %s: %d%%", tag, pct);
        s_status.progress_pct = (unsigned)pct;
        pthread_mutex_unlock(&s_lock);
    }
    /* dltotal is only the server's claim, and is 0 for a chunked response;
     * dlnow is what actually reached the card, so cap that too. */
    if (dltotal > VPK_MAX || dlnow > VPK_MAX)
        return 1;
    return atomic_load(&s_abort) ? 1 : 0;
}

static CURL *new_request(const char *url, char *errbuf)
{
    CURL *c = curl_easy_init();
    if (!c)
        return NULL;
    errbuf[0] = '\0';
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_USERAGENT, s_user_agent);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
    /* We follow redirects, so the whole chain has to stay on https or the
     * CAINFO/VERIFYPEER work below can be stepped around by one 302. */
#if LIBCURL_VERSION_NUM >= 0x075500   /* 7.85.0 added the string forms */
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
    curl_easy_setopt(c, CURLOPT_PROTOCOLS, (long)CURLPROTO_HTTPS);
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS, (long)CURLPROTO_HTTPS);
#endif
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CAINFO, s_ca_file);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progress_cb);
    return c;
}

static const char *curl_err(CURLcode rc, const char *errbuf)
{
    return errbuf[0] ? errbuf : curl_easy_strerror(rc);
}

/* ---- workers -------------------------------------------------------------- */

static void *check_thread(void *arg)
{
    (void)arg;
    char errbuf[CURL_ERROR_SIZE], tag[32], url[sizeof(s_vpk_url)];
    MemBuf body = {malloc(JSON_MAX), 0};
    struct curl_slist *hdrs = curl_slist_append(NULL, "Accept: application/vnd.github+json");
    CURL *c = (body.data && hdrs) ? new_request(RELEASE_API, errbuf) : NULL;

    if (!c) {
        set_status(UPD_ERROR, "Update check failed: out of memory");
        goto done;
    }
    body.data[0] = '\0';
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, mem_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, NULL);

    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);

    if (atomic_load(&s_abort))
        set_status(UPD_IDLE, " ");
    else if (rc != CURLE_OK)
        set_status(UPD_ERROR, "Update check failed: %s", curl_err(rc, errbuf));
    else if (code == 403 || code == 429)
        set_status(UPD_ERROR, "GitHub rate limit reached - try again later");
    else if (code == 404)
        set_status(UPD_ERROR, "No release published yet");
    else if (code != 200)
        set_status(UPD_ERROR, "Update check failed: HTTP %ld", code);
    else {
        int pr = release_json_parse(body.data, tag, sizeof(tag), url, sizeof(url));
        if (pr == RELEASE_JSON_ERR_TAG_LONG)
            set_status(UPD_ERROR, "Update check failed: the release tag is too long");
        else if (pr != RELEASE_JSON_OK || !version_valid(tag))
            set_status(UPD_ERROR, "Update check failed: no usable VPK in the latest release");
        else {
            pthread_mutex_lock(&s_lock);
            snprintf(s_status.latest, sizeof(s_status.latest), "%s", tag);
            snprintf(s_vpk_url, sizeof(s_vpk_url), "%s", url);
            pthread_mutex_unlock(&s_lock);
            if (version_compare(tag, VR_VERSION) > 0)
                set_status(UPD_AVAILABLE, "Update %s available - press TRIANGLE to install", tag);
            else
                set_status(UPD_UP_TO_DATE, "Up to date (v%s)", VR_VERSION);
        }
    }

done:
    if (c)
        curl_easy_cleanup(c);
    curl_slist_free_all(hdrs);
    free(body.data);
    atomic_store(&s_busy, 0);
    return NULL;
}

static size_t file_write(char *data, size_t size, size_t nmemb, void *user)
{
    return fwrite(data, size, nmemb, (FILE *)user) * size;
}

static int download_vpk(const char *url)
{
    char errbuf[CURL_ERROR_SIZE];
    fs_mkdir_p(DATA_DIR);

    /* Refuse up front rather than filling the card and failing mid-write.
     * VPK_MAX is exactly what this function is allowed to write. */
    int64_t freeb = ux0_free_bytes();
    if (freeb >= 0 && freeb < VPK_MAX) {
        set_status(UPD_ERROR, "Not enough space on ux0: %ld MB free, %ld MB needed",
                   (long)(freeb / (1024 * 1024)), (long)(VPK_MAX / (1024 * 1024)));
        return -1;
    }

    FILE *f = fopen(VPK_PATH, "wb");
    if (!f) {
        set_status(UPD_ERROR, "Couldn't write " VPK_PATH);
        return -1;
    }
    CURL *c = new_request(url, errbuf);
    if (!c) {
        fclose(f);
        set_status(UPD_ERROR, "Download failed: out of memory");
        return -1;
    }
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, file_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, (void *)1);

    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    int close_bad = fclose(f) != 0;
    clear_progress();   /* the download is over, however it ended */

    if (atomic_load(&s_abort)) {
        set_status(UPD_IDLE, " ");
        return -1;
    }
    if (rc != CURLE_OK) {
        set_status(UPD_ERROR, "Download failed: %s", curl_err(rc, errbuf));
        return -1;
    }
    if (code != 200) {
        set_status(UPD_ERROR, "Download failed: HTTP %ld", code);
        return -1;
    }
    if (close_bad) {
        set_status(UPD_ERROR, "Download failed: memory card write error");
        return -1;
    }
    return 0;
}

static int unpack_tick(void *user)
{
    (void)user;
    sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
    return atomic_load(&s_abort);
}

/* Stages the helper title in PKG_DIR and installs it. */
static int install_helper(void)
{
    fs_rm_tree(PKG_DIR);
    fs_mkdir_p(PKG_DIR "/sce_sys");
    if (copy_file(HELPER_EBOOT, PKG_DIR "/eboot.bin") != 0 ||
        copy_file(HELPER_SFO, PKG_DIR "/sce_sys/param.sfo") != 0)
        return -1;
    if (write_head_bin(PKG_DIR, HELPER_TITLE_ID) != 0)
        return -2;
    return promote_pkg(PKG_DIR);
}

static void *install_thread(void *arg)
{
    (void)arg;
    char url[sizeof(s_vpk_url)], tag[sizeof(s_status.latest)];
    pthread_mutex_lock(&s_lock);
    snprintf(url, sizeof(url), "%s", s_vpk_url);
    snprintf(tag, sizeof(tag), "%s", s_status.latest);
    pthread_mutex_unlock(&s_lock);

    set_status(UPD_DOWNLOADING, "Downloading %s...", tag);
    if (download_vpk(url) != 0)
        goto done;

    set_status(UPD_INSTALLING, "Unpacking %s...", tag);
    fs_rm_tree(STAGE_DIR);
    int res = zip_extract(VPK_PATH, STAGE_DIR, unpack_tick, NULL);
    unlink(VPK_PATH);
    if (res != ZIP_OK) {
        fs_rm_tree(STAGE_DIR);      /* a partial tree is junk, and may be why the card filled */
        if (res == ZIP_ERR_ABORTED)
            set_status(UPD_IDLE, " ");
        else if (res == ZIP_ERR_TOO_BIG)
            set_status(UPD_ERROR, "Update package is too large to install");
        else
            set_status(UPD_ERROR, "Update package is damaged: %s", zip_strerror(res));
        goto done;
    }
    if (!stage_looks_complete(STAGE_DIR)) {
        fs_rm_tree(STAGE_DIR);
        set_status(UPD_ERROR, "Downloaded package is incomplete");
        goto done;
    }
    if (write_head_bin(STAGE_DIR, MAIN_TITLE_ID) != 0) {
        fs_rm_tree(STAGE_DIR);
        set_status(UPD_ERROR, "Downloaded package isn't a valid Vita Radio VPK");
        goto done;
    }

    set_status(UPD_INSTALLING, "Installing the updater...");
    res = install_helper();
    if (res != 0) {
        set_status(UPD_ERROR, "Couldn't install the updater (0x%08X). " UNSAFE_HINT,
                   (unsigned int)res);
        goto done;
    }

    fs_rm_tree(PKG_DIR);
    /* The token has to describe the package while it is still in STAGE_DIR and
     * be on the card before the package is, so the helper can never find a
     * package with no token beside it. */
    if (write_update_token(STAGE_DIR, tag) != 0) {
        fs_rm_tree(STAGE_DIR);
        set_status(UPD_ERROR, "Couldn't authorise the update on " DATA_DIR);
        goto done;
    }
    if (rename(STAGE_DIR, PKG_DIR) != 0) {
        unlink(TOKEN_PATH);
        fs_rm_tree(STAGE_DIR);
        set_status(UPD_ERROR, "Couldn't move the update into " PKG_DIR);
        goto done;
    }
    /* Remembered so the next boot can tell "installed" from "silently didn't". */
    write_all(PENDING_PATH, (const uint8_t *)tag, strlen(tag));
    unlink(RESULT_PATH);
    set_status(UPD_READY, "Restarting to install %s...", tag);

done:
    atomic_store(&s_busy, 0);
    return NULL;
}

static void start_worker(void *(*fn)(void *))
{
    if (s_has_thread) {
        pthread_join(s_thread, NULL);
        s_has_thread = 0;
    }
    atomic_store(&s_abort, 0);
    atomic_store(&s_busy, 1);

    pthread_attr_t attr;
    int err = pthread_attr_init(&attr);
    if (err == 0) {
        pthread_attr_setstacksize(&attr, WORKER_STACK);
        err = pthread_create(&s_thread, &attr, fn, NULL);
        pthread_attr_destroy(&attr);
    }
    if (err != 0) {
        atomic_store(&s_busy, 0);
        set_status(UPD_ERROR, "Updater couldn't start a thread");
        return;
    }
    s_has_thread = 1;
}

/* ---- public --------------------------------------------------------------- */

void updater_init(const char *user_agent, const char *ca_file)
{
    s_user_agent = strdup(user_agent);
    s_ca_file = strdup(ca_file);
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = UPD_IDLE;

    /* the helper title leaves its result here (and so do we, if it never ran) */
    uint8_t line[32];
    long n = read_all(RESULT_PATH, line, sizeof(line) - 1);
    if (n > 0) {
        line[n] = '\0';
        const char *p = (const char *)line;
        int launch_fail = strncmp(p, LAUNCH_FAIL, sizeof(LAUNCH_FAIL) - 1) == 0;
        if (launch_fail)
            p += sizeof(LAUNCH_FAIL) - 1;
        unsigned long code = strtoul(p, NULL, 0);

        /* The tag we staged, so a "success" that didn't change VR_VERSION is
         * caught instead of being offered again on every check from now on. */
        uint8_t want[sizeof(s_status.latest)];
        long w = read_all(PENDING_PATH, want, sizeof(want) - 1);
        if (w > 0) {
            want[w] = '\0';
            while (w > 0 && (want[w - 1] == '\n' || want[w - 1] == '\r'))
                want[--w] = '\0';
        }

        if (launch_fail)
            set_status(UPD_ERROR, "Couldn't start the updater (0x%08lX). " UNSAFE_HINT, code);
        else if (code != 0)
            set_status(UPD_ERROR, "Update install failed (0x%08lX). " UNSAFE_HINT, code);
        else if (w > 0 && version_compare((const char *)want, VR_VERSION) > 0)
            set_status(UPD_ERROR, "Update did not take effect - reinstall manually");
        else
            set_status(UPD_UP_TO_DATE, "Update installed - now v%s", VR_VERSION);

        unlink(RESULT_PATH);
        unlink(PENDING_PATH);
        unlink(TOKEN_PATH);
        fs_rm_tree(PKG_DIR);
    }
}

int updater_busy(void)
{
    return atomic_load(&s_busy);
}

void updater_check(void)
{
    if (updater_busy())
        return;
    set_status(UPD_CHECKING, "Checking for updates...");
    start_worker(check_thread);
}

void updater_install(void)
{
    if (updater_busy())
        return;
    pthread_mutex_lock(&s_lock);
    int ok = s_status.state == UPD_AVAILABLE && s_vpk_url[0];
    pthread_mutex_unlock(&s_lock);
    if (!ok)
        return;
    clear_progress();   /* before the worker starts, so no stale bar is drawn */
    set_status(UPD_DOWNLOADING, "Starting download...");
    start_worker(install_thread);
}

void updater_get_status(UpdateStatus *out)
{
    pthread_mutex_lock(&s_lock);
    *out = s_status;
    pthread_mutex_unlock(&s_lock);
}

void updater_launch(void)
{
    const char *uri = "psgm:play?titleid=" HELPER_TITLE_ID;
    sceAppMgrLaunchAppByUri(0xFFFFF, uri);
    sceKernelDelayThread(10 * 1000);
    int res = sceAppMgrLaunchAppByUri(0xFFFFF, uri);
    if (res < 0) {
        /* The UI is already down and the helper will never run, so nothing else
         * can report this. Leave the reason where the next boot reads it - if
         * the helper does start after all, it overwrites this with its own. */
        char line[32];
        int len = snprintf(line, sizeof(line), LAUNCH_FAIL "0x%08X\n", (unsigned int)res);
        write_all(RESULT_PATH, (const uint8_t *)line, (size_t)len);
        return;   /* main() falls through to its own sceKernelExitProcess */
    }
    sceKernelExitProcess(0);
}

void updater_shutdown(void)
{
    atomic_store(&s_abort, 1);
    if (s_has_thread) {
        pthread_join(s_thread, NULL);
        s_has_thread = 0;
    }
    free(s_user_agent);
    free(s_ca_file);
    s_user_agent = s_ca_file = NULL;
}
