/* Fonts, DPI handling, and the per-frame string pool (SDL3_ttf backend).

   Typeface: the mockup uses CSS `system-ui` (sans) and `ui-monospace` (mono),
   i.e. the OS's native UI fonts. We load those same system fonts so the app
   matches the design per platform (Segoe UI + Consolas on Windows).

   Why SDL3_ttf: it rasterizes through FreeType, whose hinting snaps stems to the
   pixel grid the way the browser/OS text does, so glyphs come out crisp and at
   the right weight instead of stb_truetype's lighter grayscale.

   Sizing: FreeType's pixel size IS the em square, which is exactly what CSS
   font-size means, so a CSS px maps straight to a TTF point size (no em-ratio
   correction like the raylib path needed). Each (family, weight, size) is its own
   TTF_Font; Clay's fontId selects it, and the SDL3 renderer draws at the font's
   own size, ignoring the config fontSize.

   DPI: we lay out and render in PHYSICAL pixels. UISC() and the font sizes both
   fold in ui_dpi (the window's pixel density), so at 100% nothing changes and a
   HiDPI display scales uniformly. ui_fonts_reload() re-opens at a new density when
   the window crosses to a different monitor.

   String pool: Clay stores {chars,length} pointers and does NOT copy, so any
   runtime-generated string must outlive the render. ui_fmt() bumps into a
   frame-scoped buffer reset once per frame; ui_str() wraps a persistent C string.
   Requires clay.h + SDL3_ttf first. */
#ifndef UI_FONTS_H
#define UI_FONTS_H

#include <SDL3_ttf/SDL_ttf.h>

/* All sizes below are the mockup's CSS px. Final px = css_px * ui_scale (a UI
   zoom) * ui_dpi (display pixel density). ui_scale is an optional zoom (the design
   preview may be shown enlarged); ui_dpi matches the browser scaling CSS px by
   devicePixelRatio. main sets ui_scale (DART_UI_ZOOM); ui_fonts_load sets ui_dpi. */
static float ui_scale = 1.0f;
static float ui_dpi   = 1.0f;
#define UISC(px) ((float)(px) * ui_scale * ui_dpi)

typedef enum { FS_CAPTION, FS_SMALL, FS_BODY, FS_VALUE, FS_TITLE, FS_HERO, FS_COUNT } FontSize;
static const int UI_FONT_PX[FS_COUNT] = { 10, 11, 13, 14, 15, 19 };   /* the mockup's CSS px */

typedef enum { WT_REG, WT_SEMI, WT_BOLD, WT_COUNT } FontWeight;
typedef enum { FAM_SANS, FAM_MONO, FAM_COUNT } FontFamily;

/* system font files per (family, weight), matching the mockup's CSS stacks */
#ifdef _WIN32
static const char *UI_FONT_FILE[FAM_COUNT][WT_COUNT] = {
    { "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/seguisb.ttf",  "C:/Windows/Fonts/segoeuib.ttf" },
    { "C:/Windows/Fonts/consola.ttf", "C:/Windows/Fonts/consolab.ttf", "C:/Windows/Fonts/consolab.ttf" },
};
#elif defined(__APPLE__)
static const char *UI_FONT_FILE[FAM_COUNT][WT_COUNT] = {
    { "/System/Library/Fonts/SFNS.ttf", "/System/Library/Fonts/SFNS.ttf", "/System/Library/Fonts/SFNS.ttf" },
    { "/System/Library/Fonts/SFNSMono.ttf", "/System/Library/Fonts/SFNSMono.ttf", "/System/Library/Fonts/SFNSMono.ttf" },
};
#else
static const char *UI_FONT_FILE[FAM_COUNT][WT_COUNT] = {
    { "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf" },
    { "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf" },
};
#endif

#define UI_FONT_COUNT (FAM_COUNT * WT_COUNT * FS_COUNT)
static TTF_Font *g_fonts[UI_FONT_COUNT];      /* indexed by ui_font_id; passed to Clay measure + SDL3 renderer */
static int       g_font_size[UI_FONT_COUNT];  /* per-font pixel size (physical), CSS px * ui_scale * ui_dpi */
static float     g_atlas_scale = 1.0f;        /* pixel density the fonts are currently opened at */

static inline int ui_font_id(FontFamily fam, FontWeight wt, FontSize sz){
    return (fam * WT_COUNT + wt) * FS_COUNT + sz;
}

/* open every (family, weight, size) at its physical pixel size. 0 on failure. */
static int ui_fonts_load(float dpi){
    int fam, wt, sz;
    if (dpi <= 0.0f) dpi = 1.0f;
    g_atlas_scale = dpi;
    ui_dpi = dpi;
    for (fam = 0; fam < FAM_COUNT; fam++)
        for (wt = 0; wt < WT_COUNT; wt++)
            for (sz = 0; sz < FS_COUNT; sz++){
                int id = ui_font_id((FontFamily)fam, (FontWeight)wt, (FontSize)sz);
                float px = UI_FONT_PX[sz] * ui_scale * dpi;
                TTF_Font *f = TTF_OpenFont(UI_FONT_FILE[fam][wt], px);
                if (!f) return 0;
                TTF_SetFontHinting(f, TTF_HINTING_NORMAL);   /* grid-fit stems: crisp, browser-like */
                g_fonts[id] = f;
                g_font_size[id] = (int)(px + 0.5f);
            }
    return 1;
}

static void ui_fonts_unload(void){
    int i;
    for (i = 0; i < UI_FONT_COUNT; i++){ if (g_fonts[i]) TTF_CloseFont(g_fonts[i]); g_fonts[i] = NULL; }
}

/* re-open at a new pixel density (window moved to a different-DPI monitor) */
static int ui_fonts_reload(float dpi){
    ui_fonts_unload();
    return ui_fonts_load(dpi);
}

/* Clay text measurement: the SDL3 renderer ships none, so we measure with the
   same TTF_Font it will draw with (sizing/wrapping then match the render 1:1). */
static Clay_Dimensions ui_measure_text(Clay_StringSlice text, Clay_TextElementConfig *config, void *userData){
    TTF_Font **fonts = (TTF_Font **)userData;
    TTF_Font  *font  = fonts[config->fontId];
    int w = 0, h = 0;
    if (font){
        if (text.length) TTF_GetStringSize(font, text.chars, (size_t)text.length, &w, &h);
        else             h = TTF_GetFontHeight(font);
    }
    return (Clay_Dimensions){ (float)w, (float)h };
}

/* fill a CLAY_TEXT_CONFIG's font fields together (the SDL3 renderer keys off
   fontId; fontSize is carried for completeness but the font's own size wins) */
#define UI_FONT(fam, wt, sz) .fontId = (uint16_t)ui_font_id(fam, wt, sz), \
    .fontSize = (uint16_t)g_font_size[ui_font_id(fam, wt, sz)]

/* width in physical px of the first n bytes of s, rendered in (fam, wt, sz) */
static float ui_text_width(const char *s, int n, FontFamily fam, FontWeight wt, FontSize sz){
    TTF_Font *f = g_fonts[ui_font_id(fam, wt, sz)];
    int w = 0, h = 0;
    if (f && n > 0) TTF_GetStringSize(f, s, (size_t)n, &w, &h);
    return (float)w;
}

/* the same, expressed back in the layout's css px (undoes the atlas density), so it
   composes with UISC() the way the layout literals do */
static float ui_text_w_css(const char *s, int n, FontFamily fam, FontWeight wt, FontSize sz){
    float d = ui_scale * ui_dpi, p = ui_text_width(s, n, fam, wt, sz);
    return d > 0.0f ? p / d : p;
}

/* ---- per-frame string pool (Clay does not copy strings) ---- */
static char   g_strpool[128 * 1024];
static size_t g_strpool_used;
static void ui_strpool_reset(void){ g_strpool_used = 0; }

static Clay_String ui_fmt(const char *fmt, ...){
    char *p = g_strpool + g_strpool_used;
    size_t avail = sizeof g_strpool - g_strpool_used;
    int n; va_list ap;
    if (avail < 2) return (Clay_String){ .isStaticallyAllocated = false, .length = 0, .chars = "" };
    va_start(ap, fmt);
    n = vsnprintf(p, avail, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if ((size_t)n >= avail) n = (int)avail - 1;
    g_strpool_used += (size_t)n + 1;
    return (Clay_String){ .isStaticallyAllocated = false, .length = n, .chars = p };
}

static inline Clay_String ui_str(const char *s){
    return (Clay_String){ .isStaticallyAllocated = false, .length = (int)strlen(s), .chars = s };
}

#endif /* UI_FONTS_H */
