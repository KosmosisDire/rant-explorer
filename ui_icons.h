/* Lucide icons as real SVG: each is embedded with a white stroke, rasterized once by
   nanosvg into an RGBA texture and drawn through Clay's image path tinted by the theme. */
#ifndef UI_ICONS_H
#define UI_ICONS_H

#include "nanosvg.h"
#include "nanosvgrast.h"
#include "logo_png.h"      /* stb is included once, with its implementation, by discovery_explorer.c */

typedef enum {
    ICON_X = 0,      /* close / dismiss */
    ICON_SEARCH,     /* filter boxes */
    ICON_BOX,        /* Nodes tab */
    ICON_RADIO,      /* Topics tab (broadcast) */
    ICON_LOGO,       /* the Rant logo (Logo.png, full color) */
    ICON_SUN,        /* theme: currently light */
    ICON_MOON,       /* theme: currently dark */
    ICON_CHEVRON_DOWN,   /* tree caret: expanded */
    ICON_CHEVRON_RIGHT,  /* tree caret: collapsed */
    ICON_CHEVRON_LEFT,   /* back, from an inspected message to its topic */
    ICON_PLUS,           /* add a new topic */
    ICON_CHECK,          /* column menu: a shown field */
    ICON_COPY,           /* copy the schema DSL to the clipboard */
    ICON_RSS,            /* a plain pub/sub topic (a feed you subscribe to) */
    ICON_FUNCTION,       /* patterns: a function channel (req/resp) */
    ICON_VARIABLE,       /* patterns: a variable channel (value/set) */
    ICON_TASK,           /* patterns: a task, Lucide activity */
    ICON_FILTER,         /* topic-tree category filter (funnel) */
    ICON_COUNT
} IconId;

/* exact 24x24 Lucide bodies (verbatim from lucide-icons/lucide), wrapped with a
   white stroke so SDL color-mod can recolor them to any theme tint. */
#define UI_ICON_HEAD "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"24\" height=\"24\" " \
    "viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#ffffff\" stroke-width=\"2\" " \
    "stroke-linecap=\"round\" stroke-linejoin=\"round\">"
#define UI_ICON(body) (UI_ICON_HEAD body "</svg>")


static const char *const UI_ICON_SVG[ICON_COUNT] = {
    /* X      */ UI_ICON("<path d=\"M18 6 6 18\"/><path d=\"m6 6 12 12\"/>"),
    /* SEARCH */ UI_ICON("<path d=\"m21 21-4.34-4.34\"/><circle cx=\"11\" cy=\"11\" r=\"8\"/>"),
    /* BOX    */ UI_ICON("<path d=\"M21 8a2 2 0 0 0-1-1.73l-7-4a2 2 0 0 0-2 0l-7 4A2 2 0 0 0 3 8v8a2 2 0 0 0 1 1.73l7 4a2 2 0 0 0 2 0l7-4A2 2 0 0 0 21 16Z\"/><path d=\"m3.3 7 8.7 5 8.7-5\"/><path d=\"M12 22V12\"/>"),
    /* RADIO  */ UI_ICON("<path d=\"M16.247 7.761a6 6 0 0 1 0 8.478\"/><path d=\"M19.075 4.933a10 10 0 0 1 0 14.134\"/><path d=\"M4.925 19.067a10 10 0 0 1 0-14.134\"/><path d=\"M7.753 16.239a6 6 0 0 1 0-8.478\"/><circle cx=\"12\" cy=\"12\" r=\"2\"/>"),
    /* LOGO   */ NULL,   /* Logo.png, see ui_logo_texture */
    /* SUN    */ UI_ICON("<circle cx=\"12\" cy=\"12\" r=\"4\"/><path d=\"M12 2v2\"/><path d=\"M12 20v2\"/><path d=\"m4.93 4.93 1.41 1.41\"/><path d=\"m17.66 17.66 1.41 1.41\"/><path d=\"M2 12h2\"/><path d=\"M20 12h2\"/><path d=\"m6.34 17.66-1.41 1.41\"/><path d=\"m19.07 4.93-1.41 1.41\"/>"),
    /* MOON   */ UI_ICON("<path d=\"M20.985 12.486a9 9 0 1 1-9.473-9.472c.405-.022.617.46.402.803a6 6 0 0 0 8.268 8.268c.344-.215.825-.004.803.401\"/>"),
    /* CHEV_DN */ UI_ICON("<path d=\"m6 9 6 6 6-6\"/>"),
    /* CHEV_RT */ UI_ICON("<path d=\"m9 18 6-6-6-6\"/>"),
    /* CHEV_LF */ UI_ICON("<path d=\"m15 18-6-6 6-6\"/>"),
    /* PLUS   */ UI_ICON("<path d=\"M5 12h14\"/><path d=\"M12 5v14\"/>"),
    /* CHECK  */ UI_ICON("<path d=\"M20 6 9 17l-5-5\"/>"),
    /* COPY   */ UI_ICON("<rect width=\"14\" height=\"14\" x=\"8\" y=\"8\" rx=\"2\" ry=\"2\"/><path d=\"M4 16c-1.1 0-2-.9-2-2V4c0-1.1.9-2 2-2h10c1.1 0 2 .9 2 2\"/>"),
    /* RSS    */ UI_ICON("<path d=\"M4 11a9 9 0 0 1 9 9\"/><path d=\"M4 4a16 16 0 0 1 16 16\"/><circle cx=\"5\" cy=\"19\" r=\"1\"/>"),
    /* FUNCTION */ UI_ICON("<rect width=\"18\" height=\"18\" x=\"3\" y=\"3\" rx=\"2\"/><path d=\"M9 17c2 0 2.8-1 2.8-2.8V10c0-2 1-3.3 3.2-3\"/><path d=\"M9 11.2h5.7\"/>"),
    /* VARIABLE */ UI_ICON("<path d=\"M8 21s-4-3-4-9 4-9 4-9\"/><path d=\"M16 3s4 3 4 9-4 9-4 9\"/><line x1=\"15\" x2=\"9\" y1=\"9\" y2=\"15\"/><line x1=\"9\" x2=\"15\" y1=\"9\" y2=\"15\"/>"),
    /* TASK   */ UI_ICON("<path d=\"M22 12h-2.48a2 2 0 0 0-1.93 1.46l-2.35 8.36a.25.25 0 0 1-.48 0L9.24 2.18a.25.25 0 0 0-.48 0l-2.35 8.36A2 2 0 0 1 4.49 12H2\"/>"),
    /* FILTER */ UI_ICON("<polygon points=\"22 3 2 3 10 12.46 10 19 14 21 14 12.46 22 3\"/>"),
};

/* Icons rasterize at a supersample of their on screen size and SDL shrinks them, since
   shrinking always anti aliases. UISC folds in density and zoom, so a DPI change recomputes. */
#define UI_ICON_SUPERSAMPLE 2      /* texture px per draw px before the linear shrink */
#define UI_ICON_MAX_CSS     24     /* largest icon drawn (the 22px logo) plus a margin */
static int          g_icon_raster = 64;   /* texture px, recomputed per load from the UI scale */
static SDL_Texture *g_icon_tex[ICON_COUNT];

static SDL_Texture *ui_icon_rasterize(SDL_Renderer *ren, const char *svg, int raster){
    NSVGimage *img; NSVGrasterizer *rast; unsigned char *rgba; SDL_Texture *tex = NULL;
    char *copy; size_t len = strlen(svg); float scale;
    copy = (char *)malloc(len + 1);                 /* nsvgParse mutates its input */
    if (!copy) return NULL;
    memcpy(copy, svg, len + 1);
    img = nsvgParse(copy, "px", 96.0f);
    free(copy);
    if (!img) return NULL;
    rast = nsvgCreateRasterizer();
    rgba = (unsigned char *)calloc((size_t)raster * raster * 4, 1);
    if (rast && rgba){
        scale = (float)raster / (img->width > 1.0f ? img->width : 24.0f);
        nsvgRasterize(rast, img, 0, 0, scale, rgba, raster, raster, raster * 4);
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
                                raster, raster);
        if (tex){
            SDL_UpdateTexture(tex, NULL, rgba, raster * 4);
            SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
        }
    }
    if (rast) nsvgDeleteRasterizer(rast);
    free(rgba);
    nsvgDelete(img);
    return tex;
}

/* Logo.png shrunk to raster px on the CPU, since a single linear GPU shrink from 512 px aliases */
static SDL_Texture *ui_logo_texture(SDL_Renderer *ren, int raster){
    int w, h; SDL_Texture *tex = NULL; unsigned char *small;
    unsigned char *rgba = stbi_load_from_memory(UI_LOGO_PNG, (int)sizeof UI_LOGO_PNG, &w, &h, NULL, 4);
    if (!rgba) return NULL;
    small = stbir_resize_uint8_srgb(rgba, w, h, 0, NULL, raster, raster, 0, STBIR_RGBA);
    stbi_image_free(rgba);
    if (!small) return NULL;
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, raster, raster);
    if (tex){
        SDL_UpdateTexture(tex, NULL, small, raster * 4);
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
    }
    free(small);
    return tex;
}

/* title bar, taskbar and dock icon from Logo.png. The Windows exe file icon is rant_explorer.rc */
static void ui_window_icon(SDL_Window *win){
    int w, h; SDL_Surface *s;
    unsigned char *rgba = stbi_load_from_memory(UI_LOGO_PNG, (int)sizeof UI_LOGO_PNG, &w, &h, NULL, 4);
    if (!rgba) return;
    s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, rgba, w * 4);
    if (s){ SDL_SetWindowIcon(win, s); SDL_DestroySurface(s); }
    stbi_image_free(rgba);
}

/* rasterize every icon to a texture. 0 if any failed, a NULL icon just draws nothing */
static int ui_icons_load(SDL_Renderer *ren){
    int i, ok = 1, raster;
    raster = (int)(UISC(UI_ICON_MAX_CSS) * UI_ICON_SUPERSAMPLE + 0.5f);
    if (raster < 32)  raster = 32;     /* keep tiny UI scales legible */
    if (raster > 512) raster = 512;    /* cap memory at extreme zoom + HiDPI */
    g_icon_raster = raster;
    for (i = 0; i < ICON_COUNT; i++){
        g_icon_tex[i] = i == ICON_LOGO ? ui_logo_texture(ren, raster)
                                       : ui_icon_rasterize(ren, UI_ICON_SVG[i], raster);
        if (!g_icon_tex[i]) ok = 0;
    }
    return ok;
}
static void ui_icons_unload(void){
    int i;
    for (i = 0; i < ICON_COUNT; i++){ if (g_icon_tex[i]) SDL_DestroyTexture(g_icon_tex[i]); g_icon_tex[i] = NULL; }
}
/* re rasterize at the current UI scale. Call after a DPI change, like the fonts */
static int ui_icons_reload(SDL_Renderer *ren){
    ui_icons_unload();
    return ui_icons_load(ren);
}

/* emit a square icon, px CSS pixels, tinted by color (the image path color-mods it) */
static void ui_icon(IconId id, float px, Clay_Color color){
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_FIXED(UISC(px)), .height = CLAY_SIZING_FIXED(UISC(px)) } },
           .image = { .imageData = (id >= 0 && id < ICON_COUNT) ? g_icon_tex[id] : NULL },
           .backgroundColor = color }) {}
}

#endif /* UI_ICONS_H */
