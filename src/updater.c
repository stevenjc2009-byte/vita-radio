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
#include <unistd.h>

#include <psp2/appmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#define RELEASE_API     "https://api.github.com/repos/stevenjc2009-byte/vita-radio/releases/latest"
#define DATA_DIR        "ux0:data/VitaRadio"
#define VPK_PATH        DATA_DIR "/update.vpk"
#define STAGE_DIR       DATA_DIR "/stage"
#define RESULT_PATH     DATA_DIR "/update_result.txt"
#define PKG_DIR         "ux0:data/pkg"      /* the path VitaShell promotes from */
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
    if (user && dltotal > 0) {
        int pct = (int)(dlnow * 100 / dltotal);
        pthread_mutex_lock(&s_lock);
        char tag[sizeof(s_status.latest)];
        snprintf(tag, sizeof(tag), "%s", s_status.latest);
        snprintf(s_status.message, sizeof(s_status.message), "Downloading %s: %d%%", tag, pct);
        pthread_mutex_unlock(&s_lock);
    }
    if (dltotal > VPK_MAX)
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
    else if (release_json_parse(body.data, tag, sizeof(tag), url, sizeof(url)) != 0 ||
             !version_valid(tag))
        set_status(UPD_ERROR, "Update check failed: no VPK in the latest release");
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

/* Stages the helper title in ux0:data/pkg and installs it. */
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
        if (res == ZIP_ERR_ABORTED)
            set_status(UPD_IDLE, " ");
        else
            set_status(UPD_ERROR, "Update package is damaged: %s", zip_strerror(res));
        goto done;
    }
    if (write_head_bin(STAGE_DIR, MAIN_TITLE_ID) != 0) {
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
    if (rename(STAGE_DIR, PKG_DIR) != 0) {
        set_status(UPD_ERROR, "Couldn't move the update into " PKG_DIR);
        goto done;
    }
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

    /* the helper title leaves its result here */
    uint8_t line[32];
    long n = read_all(RESULT_PATH, line, sizeof(line) - 1);
    if (n > 0) {
        line[n] = '\0';
        unsigned long code = strtoul((const char *)line, NULL, 0);
        if (code == 0)
            set_status(UPD_UP_TO_DATE, "Update installed - now v%s", VR_VERSION);
        else
            set_status(UPD_ERROR, "Update install failed (0x%08lX). " UNSAFE_HINT, code);
        unlink(RESULT_PATH);
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
    sceAppMgrLaunchAppByUri(0xFFFFF, uri);
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
