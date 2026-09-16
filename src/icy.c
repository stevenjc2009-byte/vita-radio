#include "icy.h"

#include <string.h>

enum { ICY_AUDIO = 0, ICY_LEN = 1, ICY_META = 2 };

void icy_init(IcyParser *p, size_t metaint, IcyAudioFn on_audio, IcyTitleFn on_title, void *user)
{
    memset(p, 0, sizeof(*p));
    p->metaint    = metaint;
    p->audio_left = metaint;
    p->state      = ICY_AUDIO;
    p->on_audio   = on_audio;
    p->on_title   = on_title;
    p->user       = user;
}

static void icy_finish_block(IcyParser *p)
{
    char title[sizeof(p->last_title)];

    p->meta[p->meta_len] = '\0';
    if (!icy_extract_title(p->meta, p->meta_len, title, sizeof(title)))
        return;
    if (title[0] == '\0' || strcmp(title, p->last_title) == 0)
        return;
    memcpy(p->last_title, title, sizeof(title));
    if (p->on_title)
        p->on_title(p->user, p->last_title);
}

int icy_feed(IcyParser *p, const unsigned char *data, size_t len)
{
    if (len == 0)
        return 0;

    if (p->metaint == 0)
        return (p->on_audio && p->on_audio(p->user, data, len) < 0) ? -1 : 0;

    while (len > 0) {
        switch (p->state) {
        case ICY_AUDIO: {
            size_t n = p->audio_left < len ? p->audio_left : len;
            if (n > 0 && p->on_audio && p->on_audio(p->user, data, n) < 0)
                return -1;
            data += n;
            len -= n;
            p->audio_left -= n;
            if (p->audio_left == 0)
                p->state = ICY_LEN;
            break;
        }
        case ICY_LEN:
            p->meta_left = (size_t)data[0] * 16u;
            p->meta_len  = 0;
            data++;
            len--;
            if (p->meta_left == 0) {
                p->audio_left = p->metaint;
                p->state      = ICY_AUDIO;
            } else {
                p->state = ICY_META;
            }
            break;
        default: { /* ICY_META */
            size_t n = p->meta_left < len ? p->meta_left : len;
            memcpy(p->meta + p->meta_len, data, n);
            p->meta_len += n;
            p->meta_left -= n;
            data += n;
            len -= n;
            if (p->meta_left == 0) {
                icy_finish_block(p);
                p->audio_left = p->metaint;
                p->state      = ICY_AUDIO;
            }
            break;
        }
        }
    }
    return 0;
}

/* Bounded substring search (no NUL termination assumed). */
static const char *find_bytes(const char *hay, size_t hlen, const char *needle, size_t nlen)
{
    size_t i;
    if (nlen == 0 || hlen < nlen)
        return NULL;
    for (i = 0; i + nlen <= hlen; i++)
        if (hay[i] == needle[0] && memcmp(hay + i, needle, nlen) == 0)
            return hay + i;
    return NULL;
}

int icy_extract_title(const char *meta, size_t len, char *out, size_t outsz)
{
    static const char key[] = "StreamTitle='";
    const char *nul, *start, *end;
    size_t vlen;

    if (!meta || !out || outsz == 0)
        return 0;

    /* A block is NUL padded; nothing after the first NUL is metadata. */
    nul = memchr(meta, '\0', len);
    if (nul)
        len = (size_t)(nul - meta);

    start = find_bytes(meta, len, key, sizeof(key) - 1);
    if (!start)
        return 0;
    start += sizeof(key) - 1;

    end = find_bytes(start, len - (size_t)(start - meta), "';", 2);
    if (!end)
        return 0;

    vlen = (size_t)(end - start);
    if (vlen > outsz - 1) {
        vlen = outsz - 1;
        /* Titles are UTF-8. Cutting one mid-sequence leaves bare continuation
         * bytes for the text renderer to choke on, so back the cut up to the
         * start of the character it landed inside. */
        while (vlen > 0 && ((unsigned char)start[vlen] & 0xC0) == 0x80)
            vlen--;
    }
    memcpy(out, start, vlen);
    out[vlen] = '\0';
    return 1;
}
