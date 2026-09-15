#ifndef VR_ICY_H
#define VR_ICY_H

#include <stddef.h>

/* Return 0 to continue, -1 to stop the stream. */
typedef int  (*IcyAudioFn)(void *user, const unsigned char *data, size_t len);
typedef void (*IcyTitleFn)(void *user, const char *title);

/* Splits an Icecast/SHOUTcast body into audio bytes and in-band metadata.
 * Layout: metaint audio bytes, 1 length byte (x16), that many metadata bytes, repeat. */
typedef struct {
    size_t     metaint;     /* 0 = stream carries no metadata: everything is audio */
    size_t     audio_left;  /* audio bytes until the next length byte */
    size_t     meta_left;   /* metadata bytes still to collect */
    size_t     meta_len;    /* metadata bytes collected so far */
    int        state;       /* internal */
    char       meta[4081];  /* 255*16 + NUL */
    IcyAudioFn on_audio;
    IcyTitleFn on_title;    /* called only when a non-empty StreamTitle changes */
    void      *user;
    char       last_title[256];
} IcyParser;

void icy_init(IcyParser *p, size_t metaint, IcyAudioFn on_audio, IcyTitleFn on_title, void *user);

/* Feed any number of body bytes. Returns 0, or -1 if on_audio asked to stop. */
int  icy_feed(IcyParser *p, const unsigned char *data, size_t len);

/* Pulls StreamTitle='...'; out of a metadata block (block may be NUL-padded and
 * the title may contain apostrophes; the value ends at the "';" terminator).
 * Returns 1 and writes a NUL-terminated (truncated if needed) title, else 0. */
int  icy_extract_title(const char *meta, size_t len, char *out, size_t outsz);

#endif
