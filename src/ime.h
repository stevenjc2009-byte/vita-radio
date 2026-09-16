#ifndef VR_IME_H
#define VR_IME_H

#include <stddef.h>

/* On-screen keyboard (sceImeDialog) for the station search box.
 *
 * Blocking by design: it runs its own draw/pump loop until the user confirms or
 * cancels, because the dialog is composited into the back buffer by
 * vita2d_common_dialog_update() and simply will not appear unless something
 * calls that every single frame. Playback is unaffected - the network and
 * decode threads keep running. */

/* Once at startup, after vita2d_init (the dialog renders through GXM).
 * Returns 0, -1 on failure. */
int ime_init(void);

/* title and initial are UTF-8; out receives UTF-8.
 * Returns 1 if the user confirmed, 0 if cancelled, -1 on error. */
int ime_prompt(const char *title, const char *initial, char *out, size_t outsz);

#endif
