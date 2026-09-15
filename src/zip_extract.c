#include "zip_extract.h"
#include "fs_util.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define SIG_EOCD    0x06054b50u
#define SIG_CENTRAL 0x02014b50u
#define SIG_LOCAL   0x04034b50u
#define EOCD_LEN    22
#define CENTRAL_LEN 46
#define LOCAL_LEN   30
#define NAME_LEN    255
#define CHUNK       (64 * 1024)

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int read_at(FILE *f, long off, uint8_t *buf, size_t n)
{
    return fseek(f, off, SEEK_SET) == 0 && fread(buf, 1, n, f) == n ? 0 : -1;
}

int zip_name_safe(const char *name)
{
    if (!name || !name[0] || name[0] == '/' || name[0] == '\\' || strchr(name, ':'))
        return 0;
    const char *p = name;
    while (*p) {
        size_t len = strcspn(p, "/\\");
        if (len == 2 && p[0] == '.' && p[1] == '.')
            return 0;
        p += len;
        if (*p)
            p++;
    }
    return 1;
}

const char *zip_strerror(int err)
{
    switch (err) {
    case ZIP_OK:              return "ok";
    case ZIP_ERR_OPEN:        return "can't open file";
    case ZIP_ERR_READ:        return "read error";
    case ZIP_ERR_FORMAT:      return "not a valid zip";
    case ZIP_ERR_UNSUPPORTED: return "unsupported zip feature";
    case ZIP_ERR_UNSAFE_NAME: return "unsafe file name";
    case ZIP_ERR_WRITE:       return "write error";
    case ZIP_ERR_DATA:        return "corrupt data";
    case ZIP_ERR_CRC:         return "CRC mismatch";
    case ZIP_ERR_ABORTED:     return "cancelled";
    case ZIP_ERR_NOMEM:       return "out of memory";
    case ZIP_ERR_EMPTY:       return "empty archive";
    }
    return "unknown error";
}

/* Locates the end-of-central-directory record. */
static int find_eocd(FILE *f, long size, long *cd_off, unsigned *count)
{
    long span = size < EOCD_LEN + 65535L ? size : EOCD_LEN + 65535L;
    uint8_t *buf = malloc((size_t)span);
    if (!buf)
        return ZIP_ERR_NOMEM;
    if (read_at(f, size - span, buf, (size_t)span) != 0) {
        free(buf);
        return ZIP_ERR_READ;
    }
    int res = ZIP_ERR_FORMAT;
    for (long i = span - EOCD_LEN; i >= 0; i--) {
        const uint8_t *e = buf + i;
        if (rd32(e) != SIG_EOCD || i + EOCD_LEN + rd16(e + 20) > span)
            continue;
        uint16_t entries = rd16(e + 10);
        uint32_t cd_size = rd32(e + 12), off = rd32(e + 16);
        if (rd16(e + 4) != 0 || rd16(e + 6) != 0 || entries != rd16(e + 8)) {
            res = ZIP_ERR_UNSUPPORTED;      /* multi-disk */
        } else if (entries == 0xFFFF || off == 0xFFFFFFFFu) {
            res = ZIP_ERR_UNSUPPORTED;      /* zip64 */
        } else if ((uint64_t)off + cd_size > (uint64_t)(size - span + i)) {
            res = ZIP_ERR_FORMAT;
        } else {
            *cd_off = (long)off;
            *count = entries;
            res = ZIP_OK;
        }
        break;
    }
    free(buf);
    return res;
}

typedef struct {
    FILE    *zip;
    long     zip_size;
    uint8_t *in, *out;
    ZipTick  tick;
    void    *user;
} Ctx;

static int copy_entry(Ctx *c, long data_off, uint32_t csize, uint32_t usize, int method,
                      uint32_t want_crc, FILE *dst)
{
    if (fseek(c->zip, data_off, SEEK_SET) != 0)
        return ZIP_ERR_READ;

    uLong crc = crc32(0L, Z_NULL, 0);
    uint32_t produced = 0, left = csize;
    int res = ZIP_OK;

    if (method == 0) {
        if (csize != usize)
            return ZIP_ERR_FORMAT;
        while (left > 0) {
            size_t n = left < CHUNK ? left : CHUNK;
            if (fread(c->in, 1, n, c->zip) != n)
                return ZIP_ERR_READ;
            if (fwrite(c->in, 1, n, dst) != n)
                return ZIP_ERR_WRITE;
            crc = crc32(crc, c->in, (uInt)n);
            left -= (uint32_t)n;
            produced += (uint32_t)n;
            if (c->tick && c->tick(c->user))
                return ZIP_ERR_ABORTED;
        }
    } else {
        z_stream zs;
        memset(&zs, 0, sizeof(zs));
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK)
            return ZIP_ERR_NOMEM;
        int zr = Z_OK;
        while (zr != Z_STREAM_END) {
            if (zs.avail_in == 0) {
                if (left == 0) {
                    res = ZIP_ERR_DATA;     /* stream ended early */
                    break;
                }
                size_t n = left < CHUNK ? left : CHUNK;
                if (fread(c->in, 1, n, c->zip) != n) {
                    res = ZIP_ERR_READ;
                    break;
                }
                left -= (uint32_t)n;
                zs.next_in = c->in;
                zs.avail_in = (uInt)n;
            }
            zs.next_out = c->out;
            zs.avail_out = CHUNK;
            zr = inflate(&zs, Z_NO_FLUSH);
            if (zr != Z_OK && zr != Z_STREAM_END) {
                res = ZIP_ERR_DATA;
                break;
            }
            size_t have = CHUNK - zs.avail_out;
            if (have > usize - produced) {
                res = ZIP_ERR_DATA;
                break;
            }
            if (have && fwrite(c->out, 1, have, dst) != have) {
                res = ZIP_ERR_WRITE;
                break;
            }
            crc = crc32(crc, c->out, (uInt)have);
            produced += (uint32_t)have;
            if (c->tick && c->tick(c->user)) {
                res = ZIP_ERR_ABORTED;
                break;
            }
        }
        inflateEnd(&zs);
        if (res != ZIP_OK)
            return res;
    }

    if (produced != usize || crc != want_crc)
        return ZIP_ERR_CRC;
    return ZIP_OK;
}

static int extract_all(Ctx *c, const char *dir)
{
    long cd_off;
    unsigned count;
    int res = find_eocd(c->zip, c->zip_size, &cd_off, &count);
    if (res != ZIP_OK)
        return res;
    if (count == 0)
        return ZIP_ERR_EMPTY;

    long pos = cd_off;
    for (unsigned i = 0; i < count; i++) {
        uint8_t h[CENTRAL_LEN], lh[LOCAL_LEN];
        char name[NAME_LEN + 1], path[FS_PATH_LEN];

        if (read_at(c->zip, pos, h, sizeof(h)) != 0)
            return ZIP_ERR_READ;
        if (rd32(h) != SIG_CENTRAL)
            return ZIP_ERR_FORMAT;
        uint16_t flags = rd16(h + 8), method = rd16(h + 10);
        uint32_t crc = rd32(h + 16), csize = rd32(h + 20), usize = rd32(h + 24);
        uint16_t nlen = rd16(h + 28), xlen = rd16(h + 30), clen = rd16(h + 32);
        uint32_t loff = rd32(h + 42);

        if (nlen == 0 || nlen > NAME_LEN)
            return ZIP_ERR_FORMAT;
        if (read_at(c->zip, pos + CENTRAL_LEN, (uint8_t *)name, nlen) != 0)
            return ZIP_ERR_READ;
        name[nlen] = '\0';
        pos += CENTRAL_LEN + nlen + xlen + clen;

        if (strlen(name) != nlen || !zip_name_safe(name))
            return ZIP_ERR_UNSAFE_NAME;
        if ((flags & 1) || (method != 0 && method != 8) ||
            csize == 0xFFFFFFFFu || usize == 0xFFFFFFFFu || loff == 0xFFFFFFFFu)
            return ZIP_ERR_UNSUPPORTED;
        if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path))
            return ZIP_ERR_UNSUPPORTED;

        size_t plen = strlen(path);
        if (path[plen - 1] == '/') {
            fs_mkdir_p(path);
            continue;
        }

        if (read_at(c->zip, (long)loff, lh, sizeof(lh)) != 0)
            return ZIP_ERR_READ;
        if (rd32(lh) != SIG_LOCAL)
            return ZIP_ERR_FORMAT;
        long data_off = (long)loff + LOCAL_LEN + rd16(lh + 26) + rd16(lh + 28);
        if ((uint64_t)data_off + csize > (uint64_t)c->zip_size)
            return ZIP_ERR_FORMAT;

        char *slash = strrchr(path, '/');
        *slash = '\0';
        fs_mkdir_p(path);
        *slash = '/';

        FILE *dst = fopen(path, "wb");
        if (!dst)
            return ZIP_ERR_WRITE;
        res = copy_entry(c, data_off, csize, usize, method, crc, dst);
        if (fclose(dst) != 0 && res == ZIP_OK)
            res = ZIP_ERR_WRITE;
        if (res != ZIP_OK)
            return res;
    }
    return ZIP_OK;
}

int zip_extract(const char *zip_path, const char *dir, ZipTick tick, void *user)
{
    Ctx c = {0};
    c.tick = tick;
    c.user = user;
    c.zip = fopen(zip_path, "rb");
    if (!c.zip)
        return ZIP_ERR_OPEN;

    int res;
    if (fseek(c.zip, 0, SEEK_END) != 0 || (c.zip_size = ftell(c.zip)) < 0) {
        res = ZIP_ERR_READ;
    } else if (c.zip_size < EOCD_LEN) {
        res = ZIP_ERR_FORMAT;
    } else {
        c.in = malloc(CHUNK);
        c.out = malloc(CHUNK);
        if (!c.in || !c.out) {
            res = ZIP_ERR_NOMEM;
        } else {
            fs_mkdir_p(dir);
            res = extract_all(&c, dir);
        }
    }
    free(c.in);
    free(c.out);
    fclose(c.zip);
    return res;
}
