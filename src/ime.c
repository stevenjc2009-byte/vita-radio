#include "ime.h"

#include <stdint.h>
#include <string.h>

#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/display.h>
#include <psp2/ime_dialog.h>
#include <psp2/sysmodule.h>
#include <psp2/system_param.h>
#include <vita2d.h>

#define SCREEN_W 960
#define SCREEN_H 544

/* Station searches are short; the API allows up to SCE_IME_DIALOG_MAX_TEXT_LENGTH. */
#define IME_MAX_TEXT  64
#define IME_TITLE_MAX SCE_IME_DIALOG_MAX_TITLE_LENGTH

/* The dialog composites over whatever is already in the back buffer. */
#define COL_DIM RGBA8(0, 0, 0, 170)

/* The dialog writes into inputTextBuffer while it is up, so these outlive any
 * single frame. ime_prompt is single-entry (s_busy), so sharing them is safe. */
static SceWChar16 s_title[IME_TITLE_MAX + 1];
static SceWChar16 s_initial[IME_MAX_TEXT + 1];
static SceWChar16 s_input[IME_MAX_TEXT + 1];

static int s_ready;
static int s_busy;

/* --- UTF-8 <-> UTF-16 -------------------------------------------------------
 * sceImeDialog speaks UTF-16 only and VitaSDK ships no converter. Both
 * directions handle the full BMP plus surrogate pairs, and skip malformed
 * input rather than emitting replacement bytes. */

/* Writes at most dst_units - 1 code units plus a terminator. */
static void utf8_to_utf16(const char *src, SceWChar16 *dst, size_t dst_units)
{
    const unsigned char *s = (const unsigned char *)(src ? src : "");
    size_t o = 0;

    if (dst_units == 0) return;

    while (*s && o + 1 < dst_units) {
        unsigned char c = s[0];
        uint32_t cp;

        if (c < 0x80) {
            cp = c;
            s += 1;
        } else if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
            cp = ((uint32_t)(c & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
            s += 2;
        } else if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
            cp = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) |
                 (uint32_t)(s[2] & 0x3F);
            s += 3;
        } else if ((c & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 &&
                   (s[3] & 0xC0) == 0x80) {
            cp = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
                 ((uint32_t)(s[2] & 0x3F) << 6) | (uint32_t)(s[3] & 0x3F);
            s += 4;
        } else {
            s += 1;     /* malformed lead or truncated sequence */
            continue;
        }

        /* Lone surrogates and out-of-range code points are not representable. */
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) continue;

        if (cp >= 0x10000) {
            if (o + 2 >= dst_units) break;   /* no room for the pair + terminator */
            cp -= 0x10000;
            dst[o++] = (SceWChar16)(0xD800 | (cp >> 10));
            dst[o++] = (SceWChar16)(0xDC00 | (cp & 0x3FF));
        } else {
            dst[o++] = (SceWChar16)cp;
        }
    }

    dst[o] = 0;
}

/* Writes at most dst_size - 1 bytes plus a terminator; never overflows dst. */
static void utf16_to_utf8(const SceWChar16 *src, char *dst, size_t dst_size)
{
    size_t i = 0, o = 0;

    if (dst_size == 0) return;

    while (src[i]) {
        uint32_t cp = src[i++];
        size_t need;

        if (cp >= 0xD800 && cp <= 0xDBFF) {
            uint32_t lo = src[i];
            if (lo < 0xDC00 || lo > 0xDFFF) continue;   /* unpaired high surrogate */
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            i++;
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            continue;                                   /* unpaired low surrogate */
        }

        need = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
        if (o + need >= dst_size) break;                /* keep room for the terminator */

        if (need == 1) {
            dst[o++] = (char)cp;
        } else if (need == 2) {
            dst[o++] = (char)(0xC0 | (cp >> 6));
            dst[o++] = (char)(0x80 | (cp & 0x3F));
        } else if (need == 3) {
            dst[o++] = (char)(0xE0 | (cp >> 12));
            dst[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            dst[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            dst[o++] = (char)(0xF0 | (cp >> 18));
            dst[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            dst[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            dst[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }

    dst[o] = '\0';
}

/* --- public API ----------------------------------------------------------- */

int ime_init(void)
{
    SceAppUtilInitParam init;
    SceAppUtilBootParam boot;
    SceCommonDialogConfigParam config;
    int lang = SCE_SYSTEM_PARAM_LANG_ENGLISH_US;
    int enter = SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS;

    if (s_ready) return 0;

    /* ime_dialog.h's usage note requires this module. Loading one that is
     * already up also reports an error, so a failure here is not fatal - let
     * sceImeDialogInit be the thing that reports a genuinely missing IME. */
    sceSysmoduleLoadModule(SCE_SYSMODULE_IME);

    memset(&init, 0, sizeof(init));
    memset(&boot, 0, sizeof(boot));
    if (sceAppUtilInit(&init, &boot) < 0) return -1;

    /* Fall back to the defaults above if the system refuses either query. */
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &lang);
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, &enter);

    sceCommonDialogConfigParamInit(&config);
    config.language = (SceSystemParamLang)lang;
    config.enterButtonAssign = (SceSystemParamEnterButtonAssign)enter;
    if (sceCommonDialogSetConfigParam(&config) < 0) return -1;

    s_ready = 1;
    return 0;
}

int ime_prompt(const char *title, const char *initial, char *out, size_t outsz)
{
    SceImeDialogParam param;
    SceImeDialogResult result;
    SceCommonDialogStatus status;
    int confirmed;

    if (!s_ready || !out || outsz == 0) return -1;
    if (s_busy) return -1;      /* sceImeDialogInit would return IME_IN_USE */

    memset(s_title, 0, sizeof(s_title));
    memset(s_initial, 0, sizeof(s_initial));
    memset(s_input, 0, sizeof(s_input));
    utf8_to_utf16(title ? title : "", s_title, IME_TITLE_MAX + 1);
    utf8_to_utf16(initial ? initial : "", s_initial, IME_MAX_TEXT + 1);

    sceImeDialogParamInit(&param);
    param.supportedLanguages = SCE_IME_LANGUAGE_ENGLISH;
    param.languagesForced = SCE_TRUE;
    param.type = SCE_IME_TYPE_BASIC_LATIN;
    param.option = 0;
    param.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_DEFAULT;
    param.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT;
    param.enterLabel = SCE_IME_ENTER_LABEL_SEARCH;
    param.maxTextLength = IME_MAX_TEXT;
    param.title = s_title;
    param.initialText = s_initial;
    param.inputTextBuffer = s_input;

    if (sceImeDialogInit(&param) < 0) return -1;
    s_busy = 1;

    /* The dialog draws nothing itself: vita2d_common_dialog_update() composites
     * it into the back buffer, and must run on every frame the dialog is up or
     * the user sees a black screen. */
    for (;;) {
        vita2d_start_drawing();
        vita2d_clear_screen();
        vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, COL_DIM);
        vita2d_end_drawing();
        vita2d_common_dialog_update();
        vita2d_swap_buffers();
        sceDisplayWaitVblankStart();

        status = sceImeDialogGetStatus();
        if (status == SCE_COMMON_DIALOG_STATUS_FINISHED) break;
        if (status == SCE_COMMON_DIALOG_STATUS_NONE) {
            /* Dialog went away without finishing - don't spin forever. */
            sceImeDialogTerm();
            s_busy = 0;
            return -1;
        }
    }

    memset(&result, 0, sizeof(result));
    sceImeDialogGetResult(&result);
    sceImeDialogTerm();
    s_busy = 0;

    /* .button is the user's choice; .result is the dialog's own status. */
    confirmed = result.button == SCE_IME_DIALOG_BUTTON_ENTER;
    if (!confirmed) return 0;   /* CLOSE / NONE: leave out untouched */

    utf16_to_utf8(s_input, out, outsz);
    return 1;
}
