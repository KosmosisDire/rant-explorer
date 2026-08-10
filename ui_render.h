/* Vendored Clay -> SDL3 renderer with grayscale-AA text.

   Replaces Clay's stock renderers/SDL3/clay_renderer_SDL3.c. We only ever used
   its rounded-rect + arc fills, so those are ported below (MIT, (c) Clay
   authors); the rest of that file (measure function, SDL_image/SDL_main pulls)
   is dropped. ui_render() runs the command loop itself; text goes through
   TTF_RenderText_Blended, whose alpha-channel output blends over whatever is
   already in the target, so no background reconstruction is needed.

   Text is neither rasterized nor re-uploaded per frame. Doing so was the
   renderer's dominant cost and, over a long session, FATAL:
   SDL_CreateTextureFromSurface burns about three SDL_PropertiesID values per call
   (the texture's own, SDL_GetTextureProperties, SDL_GetSurfaceProperties) off one
   global 32-bit counter that is never recycled. At this app's text volume the
   counter saturates in roughly a day, and SDL_CreateProperties then spins
   FOREVER: its allocation loop computes 0xFFFFFFFF + 1 == 0, hits its
   `if (props == 0) continue;` guard, and can never reach the compare-and-swap
   that would advance it (still present in SDL main as of 3.2.16). The window
   freezes with a core pegged and nothing can free it.

   Two mechanisms below, because one is not enough. The CACHE keys textures by
   (font, color, string) and covers text that repeats. The POOL covers text that
   does not: a subscribed message feed mints a unique string per message, which
   misses the cache forever, so misses must not create textures either. They
   borrow a size-classed texture and overwrite it with SDL_UpdateTexture, which
   allocates no properties at all. Together, a steadily scrolling feed consumes
   ZERO property IDs once the pool warms.

   Requires clay.h, SDL3, SDL3_ttf. */
#ifndef UI_RENDER_H
#define UI_RENDER_H

#include "clay.h"
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    SDL_Renderer *renderer;
    TTF_Font    **fonts;     /* indexed by Clay fontId */
} UiRenderer;

#define UI_CIRCLE_SEGMENTS 16

/* ---- geometry scratch pool ----
   Every rounded rect and every border arc used to malloc/free a vertex and an
   index array, so a frame paid hundreds of allocation pairs for buffers with the
   same handful of shapes. One growable scratch serves them all instead: ui_render
   runs only on the main thread, and neither filler holds its buffers across a call
   or nests inside the other, so a single shared pair is safe. Capacity ratchets to
   the frame's worst shape and stays there. */
static SDL_Vertex *g_geo_vtx;
static int        *g_geo_idx;
static int         g_geo_vtx_cap, g_geo_idx_cap;

/* grow the scratch to hold nv vertices and ni indices. 0 = out of memory (caller skips the shape). */
static int ui_geo_reserve(int nv, int ni){
    if (nv > g_geo_vtx_cap){
        SDL_Vertex *p = (SDL_Vertex *)realloc(g_geo_vtx, (size_t)nv * sizeof *p);
        if (!p) return 0;
        g_geo_vtx = p; g_geo_vtx_cap = nv;
    }
    if (ni > g_geo_idx_cap){
        int *p = (int *)realloc(g_geo_idx, (size_t)ni * sizeof *p);
        if (!p) return 0;
        g_geo_idx = p; g_geo_idx_cap = ni;
    }
    return 1;
}

/* ---- text texture pool ----
   The cache below handles text that REPEATS. It cannot help text that is unique
   every time it appears: a subscribed message feed mints a brand new string per
   message, so those miss forever, and a miss that creates a texture is exactly
   what burns property IDs. So misses do not create textures. They borrow one from
   this pool, sized by class the way the SHM path sizes its chunks, and overwrite
   its pixels with SDL_UpdateTexture -- which, unlike SDL_CreateTextureFromSurface,
   allocates no properties at all. Evicted entries return their texture here rather
   than destroying it. Steady-state property consumption is then ZERO no matter how
   fast the feed scrolls: the pool stops growing once it covers the working set.

   A texture is drawn through a source rect of the text's true size, so the slack
   in its class never shows, and NEAREST + 1:1 scaling means no filtering samples
   into it either. A string past the widest or tallest class bypasses the pool and
   is created and destroyed directly, the old behaviour: that covers unusually long
   lines, and hero text at extreme DPI x zoom. Those are the only remaining source
   of per-miss texture creation, and the cache still spares them per-frame. */
#define UI_POOL_WCLASS  9    /* 32 << i: 32 .. 8192 px */
#define UI_POOL_HCLASS  7    /* 16 * (i+1): 16 .. 112 px */
/* The free list must absorb a whole sweep's worth of releases, or the pool starves:
   the sweep frees in bursts while misses acquire continuously, so a list too short
   throws away the burst and the next second of misses creates textures again. Size
   it to (message rate x idle window), not to the visible row count. */
#define UI_POOL_KEEP    1024
#define UI_POOL_BYTES   (8u * 1024u * 1024u)   /* VRAM a single class may sit on while idle */
#define UI_TEXT_FMT     SDL_PIXELFORMAT_ARGB8888   /* what TTF_RenderText_Blended produces */

typedef struct {
    SDL_Texture *idle[UI_POOL_KEEP];   /* borrowed out on acquire, handed back on release */
    int          n;
} UiTexBucket;

/* ~0.5 MB of pointers, plus the cache's ~0.8 MB of entries: static, not VRAM.
   The textures themselves are bounded by UI_POOL_BYTES per class. */
static UiTexBucket g_pool[UI_POOL_WCLASS * UI_POOL_HCLASS];
static int         g_pool_made;   /* textures ever created (property-ID cost, should plateau) */

/* smallest class holding w x h, or -1 when it is bigger than every class */
static int ui_pool_bucket(int w, int h, int *cw, int *ch){
    int wi = 0, hi = (h - 1) / 16;
    int width = 32;
    while (width < w && wi < UI_POOL_WCLASS - 1){ width <<= 1; wi++; }
    if (width < w || hi >= UI_POOL_HCLASS || h <= 0) return -1;
    if (hi < 0) hi = 0;
    *cw = width; *ch = (hi + 1) * 16;
    return wi * UI_POOL_HCLASS + hi;
}

/* a texture at least w x h. *bucket is where to give it back (-1 = destroy it). */
static SDL_Texture *ui_pool_acquire(SDL_Renderer *ren, int w, int h, int *bucket){
    int cw = w, ch = h;
    int b = ui_pool_bucket(w, h, &cw, &ch);
    SDL_Texture *t;
    *bucket = b;
    if (b >= 0 && g_pool[b].n > 0) return g_pool[b].idle[--g_pool[b].n];
    t = SDL_CreateTexture(ren, UI_TEXT_FMT, SDL_TEXTUREACCESS_STATIC, cw, ch);
    if (!t) return NULL;
    g_pool_made++;
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);   /* TTF alpha over what is already drawn */
    SDL_SetTextureScaleMode(t, SDL_SCALEMODE_NEAREST);
    return t;
}

/* how many idle textures this class may hold: a fixed count would let the widest
   class sit on hundreds of MB, so cap by bytes and keep a small floor. */
static int ui_pool_keep(int bucket){
    unsigned cw = 32u << (bucket / UI_POOL_HCLASS);
    unsigned ch = (unsigned)((bucket % UI_POOL_HCLASS) + 1) * 16u;
    unsigned n  = UI_POOL_BYTES / (cw * ch * 4u);
    if (n < 4) n = 4;
    return n > UI_POOL_KEEP ? UI_POOL_KEEP : (int)n;
}

static void ui_pool_release(SDL_Texture *t, int bucket){
    if (!t) return;
    if (bucket < 0 || g_pool[bucket].n >= ui_pool_keep(bucket)){ SDL_DestroyTexture(t); return; }
    g_pool[bucket].idle[g_pool[bucket].n++] = t;
}

static void ui_pool_free_all(void){
    size_t b;
    for (b = 0; b < sizeof g_pool / sizeof *g_pool; b++){
        while (g_pool[b].n > 0) SDL_DestroyTexture(g_pool[b].idle[--g_pool[b].n]);
    }
}

/* ---- text texture cache ----
   Keyed by (font id, rgb, string bytes), which is everything that determines the
   raster. Open addressing with linear probing and no tombstones: entries are only
   ever removed by the sweep, which rebuilds the table wholesale, so a probe run
   still terminates at the first empty slot. Entries carry their key inline up to
   UI_TEXT_INLINE bytes and spill to the heap past it, so the common short label
   costs no allocation. The sweep drops anything untouched for UI_TEXT_IDLE_FRAMES,
   which is what returns pooled textures as text scrolls out of view. */
#define UI_TEXT_SLOTS        8192u   /* power of two: the probe mask depends on it */
#define UI_TEXT_MAX_LIVE     5461    /* two thirds load, so probe runs stay short */
#define UI_TEXT_IDLE_FRAMES  90u     /* ~1.5 s at 60 fps before an unused entry is dropped */
#define UI_TEXT_SWEEP_FRAMES 30u     /* ~0.5 s between sweeps: small, frequent batches */
#define UI_TEXT_INLINE       48      /* keys up to this live in the entry itself */

typedef struct {
    SDL_Texture *tex;                /* NULL = free slot */
    char        *heap;               /* key bytes when they do not fit inline */
    char         inl[UI_TEXT_INLINE];
    Uint32       hash;
    Uint32       used;               /* frame counter at the last hit */
    int          len, w, h;          /* w/h are the TEXT size, not the pooled texture's */
    int          bucket;             /* pool class to return tex to (-1 = destroy) */
    Uint16       font;
    Uint8        r, g, b;
} UiTextEntry;

static UiTextEntry g_text[UI_TEXT_SLOTS];
static Uint32      g_text_frame;     /* bumped once per ui_render */
static int         g_text_live;
static Uint32      g_text_swept;     /* frame of the last sweep */

static inline const char *ui_text_key(const UiTextEntry *e){ return e->heap ? e->heap : e->inl; }

static Uint32 ui_text_hash(Uint16 font, SDL_Color fg, const char *s, int len){
    Uint32 h = 2166136261u;
    int i;
    h = (h ^ (Uint32)(font & 0xFF)) * 16777619u;
    h = (h ^ (Uint32)(font >> 8))   * 16777619u;
    h = (h ^ fg.r) * 16777619u;
    h = (h ^ fg.g) * 16777619u;
    h = (h ^ fg.b) * 16777619u;
    for (i = 0; i < len; i++) h = (h ^ (Uint8)s[i]) * 16777619u;
    return h;
}

static void ui_text_drop(UiTextEntry *e){
    ui_pool_release(e->tex, e->bucket);   /* recycled, not destroyed: that is the whole point */
    free(e->heap);
    memset(e, 0, sizeof *e);
}

/* reinsert a detached entry. A free slot must exist (the load factor guarantees it). */
static void ui_text_place(const UiTextEntry *e){
    Uint32 i = e->hash & (UI_TEXT_SLOTS - 1);
    while (g_text[i].tex) i = (i + 1) & (UI_TEXT_SLOTS - 1);
    g_text[i] = *e;
}

/* destroy every cached texture. Call before the renderer dies, and whenever the
   glyph raster changes underneath the keys (a DPI reload reopens every font at a
   new pixel size, so every cached texture is the wrong size). */
static void ui_text_cache_clear(void){
    Uint32 i;
    for (i = 0; i < UI_TEXT_SLOTS; i++) if (g_text[i].tex) ui_text_drop(&g_text[i]);
    g_text_live = 0;
}

/* drop entries idle for `idle` frames or more, then rebuild the table so the
   probe runs the removals broke are contiguous again. */
static void ui_text_cache_sweep(Uint32 idle){
    UiTextEntry *keep;
    int n = 0;
    Uint32 i;

    g_text_swept = g_text_frame;
    for (i = 0; i < UI_TEXT_SLOTS; i++)
        if (g_text[i].tex && (g_text_frame - g_text[i].used) >= idle) ui_text_drop(&g_text[i]);

    for (i = 0; i < UI_TEXT_SLOTS; i++) if (g_text[i].tex) n++;
    g_text_live = n;
    if (!n) return;

    keep = (UiTextEntry *)malloc((size_t)n * sizeof *keep);
    if (!keep){ ui_text_cache_clear(); return; }   /* cold, but correct: rebuild from scratch */
    n = 0;
    for (i = 0; i < UI_TEXT_SLOTS; i++) if (g_text[i].tex) keep[n++] = g_text[i];
    memset(g_text, 0, sizeof g_text);
    for (i = 0; i < (Uint32)n; i++) ui_text_place(&keep[i]);
    free(keep);
}

/* Draw one string at (x, y), reusing its texture when we already have it.
   Falls back to an uncached create/draw/destroy if the table is genuinely full of
   strings all used this same frame, so a pathological frame still renders. */
static void ui_draw_text(SDL_Renderer *ren, TTF_Font *font, Uint16 font_id,
                         const char *s, int len, SDL_Color fg, float x, float y){
    Uint32 hash = ui_text_hash(font_id, fg, s, len);
    Uint32 i = hash & (UI_TEXT_SLOTS - 1);
    UiTextEntry ne;
    SDL_Surface *surf;
    SDL_Texture *tex;
    int tw, th, bucket;

    while (g_text[i].tex){
        UiTextEntry *e = &g_text[i];
        if (e->hash == hash && e->font == font_id && e->len == len &&
            e->r == fg.r && e->g == fg.g && e->b == fg.b &&
            !memcmp(ui_text_key(e), s, (size_t)len)){
            SDL_FRect src = { 0.0f, 0.0f, (float)e->w, (float)e->h };   /* ignore the class slack */
            SDL_FRect dst = { x, y, (float)e->w, (float)e->h };
            e->used = g_text_frame;
            SDL_RenderTexture(ren, e->tex, &src, &dst);
            return;
        }
        i = (i + 1) & (UI_TEXT_SLOTS - 1);
    }

    surf = TTF_RenderText_Blended(font, s, (size_t)len, fg);
    if (!surf) return;
    if (surf->format != UI_TEXT_FMT){            /* SDL_UpdateTexture wants the pool's format */
        SDL_Surface *conv = SDL_ConvertSurface(surf, UI_TEXT_FMT);
        SDL_DestroySurface(surf);
        surf = conv;
        if (!surf) return;
    }
    tw = surf->w; th = surf->h;
    tex = ui_pool_acquire(ren, tw, th, &bucket);
    if (tex){
        SDL_Rect box = { 0, 0, tw, th };
        if (!SDL_UpdateTexture(tex, &box, surf->pixels, surf->pitch)){
            ui_pool_release(tex, bucket);
            tex = NULL;
        }
    }
    SDL_DestroySurface(surf);
    if (!tex) return;
    {   SDL_FRect src = { 0.0f, 0.0f, (float)tw, (float)th };
        SDL_FRect dst = { x, y, (float)tw, (float)th };   /* integer pos, 1:1 size */
        SDL_RenderTexture(ren, tex, &src, &dst);
    }

    if (g_text_live >= UI_TEXT_MAX_LIVE){
        ui_text_cache_sweep(1);                    /* keep only what this frame touched */
        if (g_text_live >= UI_TEXT_MAX_LIVE){      /* one frame really does need them all */
            ui_pool_release(tex, bucket);
            return;
        }
    }

    memset(&ne, 0, sizeof ne);
    ne.tex = tex; ne.hash = hash; ne.used = g_text_frame; ne.bucket = bucket;
    ne.len = len; ne.font = font_id;
    ne.r = fg.r; ne.g = fg.g; ne.b = fg.b;
    ne.w = tw; ne.h = th;
    if (len > UI_TEXT_INLINE){
        ne.heap = (char *)malloc((size_t)len);
        if (!ne.heap){ ui_pool_release(tex, bucket); return; }
        memcpy(ne.heap, s, (size_t)len);
    } else {
        memcpy(ne.inl, s, (size_t)len);
    }
    ui_text_place(&ne);
    g_text_live++;
}

/* release every renderer-owned resource this file holds. Must run BEFORE
   SDL_DestroyRenderer, which invalidates the textures. */
static void ui_render_shutdown(void){
    ui_text_cache_clear();   /* returns every live texture to the pool... */
    ui_pool_free_all();      /* ...which then actually destroys them */
    free(g_geo_vtx); g_geo_vtx = NULL; g_geo_vtx_cap = 0;
    free(g_geo_idx); g_geo_idx = NULL; g_geo_idx_cap = 0;
}

/* ported from Clay's SDL3 renderer: a filled rounded rectangle in one
   SDL_RenderGeometry call (center quad + corner triangle fans + edge quads). */
static void ui_fill_rounded_rect(SDL_Renderer *ren, const SDL_FRect rect, const float cornerRadius, const Clay_Color _color){
    const SDL_FColor color = { _color.r/255, _color.g/255, _color.b/255, _color.a/255 };
    int indexCount = 0, vertexCount = 0;
    const float minRadius = SDL_min(rect.w, rect.h) / 2.0f;
    const float clampedRadius = SDL_min(cornerRadius, minRadius);
    const int numCircleSegments = SDL_max(UI_CIRCLE_SEGMENTS, (int) clampedRadius * 0.5f);
    int totalVertices = 4 + (4 * (numCircleSegments * 2)) + 2*4;
    int totalIndices = 6 + (4 * (numCircleSegments * 3)) + 6*4;
    /* pooled, not a VLA: MSVC has no C99 variable-length arrays, and the counts
       grow with the corner radius so a fixed stack buffer would have to be huge. */
    SDL_Vertex *vertices;
    int        *indices;
    if (!ui_geo_reserve(totalVertices, totalIndices)) return;
    vertices = g_geo_vtx;
    indices  = g_geo_idx;

    vertices[vertexCount++] = (SDL_Vertex){ {rect.x + clampedRadius, rect.y + clampedRadius}, color, {0, 0} };
    vertices[vertexCount++] = (SDL_Vertex){ {rect.x + rect.w - clampedRadius, rect.y + clampedRadius}, color, {1, 0} };
    vertices[vertexCount++] = (SDL_Vertex){ {rect.x + rect.w - clampedRadius, rect.y + rect.h - clampedRadius}, color, {1, 1} };
    vertices[vertexCount++] = (SDL_Vertex){ {rect.x + clampedRadius, rect.y + rect.h - clampedRadius}, color, {0, 1} };
    indices[indexCount++] = 0; indices[indexCount++] = 1; indices[indexCount++] = 3;
    indices[indexCount++] = 1; indices[indexCount++] = 2; indices[indexCount++] = 3;

    const float step = (SDL_PI_F/2) / numCircleSegments;
    for (int i = 0; i < numCircleSegments; i++) {
        const float angle1 = (float)i * step;
        const float angle2 = ((float)i + 1.0f) * step;
        for (int j = 0; j < 4; j++) {
            float cx, cy, signX, signY;
            switch (j) {
                case 0: cx = rect.x + clampedRadius; cy = rect.y + clampedRadius; signX = -1; signY = -1; break;
                case 1: cx = rect.x + rect.w - clampedRadius; cy = rect.y + clampedRadius; signX = 1; signY = -1; break;
                case 2: cx = rect.x + rect.w - clampedRadius; cy = rect.y + rect.h - clampedRadius; signX = 1; signY = 1; break;
                case 3: cx = rect.x + clampedRadius; cy = rect.y + rect.h - clampedRadius; signX = -1; signY = 1; break;
                default: return;
            }
            vertices[vertexCount++] = (SDL_Vertex){ {cx + SDL_cosf(angle1) * clampedRadius * signX, cy + SDL_sinf(angle1) * clampedRadius * signY}, color, {0, 0} };
            vertices[vertexCount++] = (SDL_Vertex){ {cx + SDL_cosf(angle2) * clampedRadius * signX, cy + SDL_sinf(angle2) * clampedRadius * signY}, color, {0, 0} };
            indices[indexCount++] = j;
            indices[indexCount++] = vertexCount - 2;
            indices[indexCount++] = vertexCount - 1;
        }
    }

    vertices[vertexCount++] = (SDL_Vertex){ {rect.x + clampedRadius, rect.y}, color, {0, 0} };          /* top edge */
    vertices[vertexCount++] = (SDL_Vertex){ {rect.x + rect.w - clampedRadius, rect.y}, color, {1, 0} };
    indices[indexCount++] = 0; indices[indexCount++] = vertexCount - 2; indices[indexCount++] = vertexCount - 1;
    indices[indexCount++] = 1; indices[indexCount++] = 0; indices[indexCount++] = vertexCount - 1;
    vertices[vertexCount++] = (SDL_Vertex){ {rect.x + rect.w, rect.y + clampedRadius}, color, {1, 0} };   /* right edge */
    vertices[vertexCount++] = (SDL_Vertex){ {rect.x + rect.w, rect.y + rect.h - clampedRadius}, color, {1, 1} };
    indices[indexCount++] = 1; indices[indexCount++] = vertexCount - 2; indices[indexCount++] = vertexCount - 1;
    indices[indexCount++] = 2; indices[indexCount++] = 1; indices[indexCount++] = vertexCount - 1;
    vertices[vertexCount++] = (SDL_Vertex){ {rect.x + rect.w - clampedRadius, rect.y + rect.h}, color, {1, 1} }; /* bottom edge */
    vertices[vertexCount++] = (SDL_Vertex){ {rect.x + clampedRadius, rect.y + rect.h}, color, {0, 1} };
    indices[indexCount++] = 2; indices[indexCount++] = vertexCount - 2; indices[indexCount++] = vertexCount - 1;
    indices[indexCount++] = 3; indices[indexCount++] = 2; indices[indexCount++] = vertexCount - 1;
    vertices[vertexCount++] = (SDL_Vertex){ {rect.x, rect.y + rect.h - clampedRadius}, color, {0, 1} };   /* left edge */
    vertices[vertexCount++] = (SDL_Vertex){ {rect.x, rect.y + clampedRadius}, color, {0, 0} };
    indices[indexCount++] = 3; indices[indexCount++] = vertexCount - 2; indices[indexCount++] = vertexCount - 1;
    indices[indexCount++] = 0; indices[indexCount++] = 3; indices[indexCount++] = vertexCount - 1;

    SDL_RenderGeometry(ren, NULL, vertices, vertexCount, indices, indexCount);
}

/* a filled annular sector (one rounded-border corner) via SDL_RenderGeometry.
   It uses the SAME center, outer radius and angular sampling as
   ui_fill_rounded_rect's corner fans, so the border's outer edge is vertex-for-
   vertex coincident with the fill edge: no seam, no offset, no AA fringe. The
   ring spans rOuter (the corner radius) inward to rInner (radius - border width;
   0 = a solid pie when the border is thicker than the radius). */
static void ui_fill_arc(SDL_Renderer *ren, const SDL_FPoint center, const float rOuter, const float rInner,
                        const float startAngle, const float endAngle, const Clay_Color _color){
    const SDL_FColor color = { _color.r/255, _color.g/255, _color.b/255, _color.a/255 };
    const float radStart = startAngle * (SDL_PI_F / 180.0f);
    const float radEnd   = endAngle   * (SDL_PI_F / 180.0f);
    /* segment count computed exactly as the fill does, so the shared outer-arc
       vertices land on the same points and the seam is perfect. */
    const int   segs = SDL_max(UI_CIRCLE_SEGMENTS, (int)rOuter * 0.5f);
    const float step = (radEnd - radStart) / (float)segs;
    const int   vtxCount = (segs + 1) * 2;
    const int   idxCount = segs * 6;
    /* pooled, not a VLA (MSVC); counts grow with the corner radius. */
    SDL_Vertex *vertices;
    int        *indices;
    int vc = 0, ic = 0;
    if (!ui_geo_reserve(vtxCount, idxCount)) return;
    vertices = g_geo_vtx;
    indices  = g_geo_idx;
    for (int i = 0; i <= segs; i++) {
        const float a = radStart + (float)i * step;
        const float ca = SDL_cosf(a), sa = SDL_sinf(a);
        vertices[vc++] = (SDL_Vertex){ {center.x + ca * rOuter, center.y + sa * rOuter}, color, {0, 0} };
        vertices[vc++] = (SDL_Vertex){ {center.x + ca * rInner, center.y + sa * rInner}, color, {0, 0} };
        if (i < segs) {                                  /* two triangles per quad of the strip */
            const int o0 = i*2, in0 = i*2 + 1, o1 = i*2 + 2, in1 = i*2 + 3;
            indices[ic++] = o0;  indices[ic++] = in0; indices[ic++] = o1;
            indices[ic++] = in0; indices[ic++] = in1; indices[ic++] = o1;
        }
    }
    SDL_RenderGeometry(ren, NULL, vertices, vc, indices, ic);
}

/* run a Clay command array: rects/borders direct, text alpha-blended over them. */
static void ui_render(UiRenderer *rd, Clay_RenderCommandArray *cmds){
    SDL_Renderer *ren = rd->renderer;

    g_text_frame++;
    if ((g_text_frame - g_text_swept) >= UI_TEXT_SWEEP_FRAMES) ui_text_cache_sweep(UI_TEXT_IDLE_FRAMES);

    for (size_t i = 0; i < cmds->length; i++){
        Clay_RenderCommand *cmd = Clay_RenderCommandArray_Get(cmds, i);
        Clay_BoundingBox bb = cmd->boundingBox;
        SDL_FRect rect = { (float)(int)bb.x, (float)(int)bb.y, (float)(int)bb.width, (float)(int)bb.height };

        switch (cmd->commandType){
            case CLAY_RENDER_COMMAND_TYPE_RECTANGLE: {
                Clay_RectangleRenderData *c = &cmd->renderData.rectangle;
                SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(ren, (Uint8)c->backgroundColor.r, (Uint8)c->backgroundColor.g, (Uint8)c->backgroundColor.b, (Uint8)c->backgroundColor.a);
                if (c->cornerRadius.topLeft > 0) ui_fill_rounded_rect(ren, rect, c->cornerRadius.topLeft, c->backgroundColor);
                else SDL_RenderFillRect(ren, &rect);
            } break;

            case CLAY_RENDER_COMMAND_TYPE_TEXT: {
                Clay_TextRenderData *c = &cmd->renderData.text;
                TTF_Font *font = rd->fonts[c->fontId];
                SDL_Color fg = { (Uint8)c->textColor.r, (Uint8)c->textColor.g, (Uint8)c->textColor.b, 255 };
                if (!c->stringContents.length || !font) break;
                ui_draw_text(ren, font, c->fontId, c->stringContents.chars,
                             (int)c->stringContents.length, fg, rect.x, rect.y);
            } break;

            case CLAY_RENDER_COMMAND_TYPE_BORDER: {
                Clay_BorderRenderData *c = &cmd->renderData.border;
                float minR = SDL_min(rect.w, rect.h) / 2.0f;
                float tl = SDL_min(c->cornerRadius.topLeft, minR),    tr = SDL_min(c->cornerRadius.topRight, minR);
                float bl = SDL_min(c->cornerRadius.bottomLeft, minR), br = SDL_min(c->cornerRadius.bottomRight, minR);
                SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(ren, (Uint8)c->color.r, (Uint8)c->color.g, (Uint8)c->color.b, (Uint8)c->color.a);
                if (c->width.left > 0){   SDL_FRect l = { rect.x, rect.y + tl, (float)c->width.left, rect.h - tl - bl }; SDL_RenderFillRect(ren, &l); }
                if (c->width.right > 0){  SDL_FRect l = { rect.x + rect.w - (float)c->width.right, rect.y + tr, (float)c->width.right, rect.h - tr - br }; SDL_RenderFillRect(ren, &l); }
                if (c->width.top > 0){    SDL_FRect l = { rect.x + tl, rect.y, rect.w - tl - tr, (float)c->width.top }; SDL_RenderFillRect(ren, &l); }
                if (c->width.bottom > 0){ SDL_FRect l = { rect.x + bl, rect.y + rect.h - (float)c->width.bottom, rect.w - bl - br, (float)c->width.bottom }; SDL_RenderFillRect(ren, &l); }
                /* centers match ui_fill_rounded_rect's corner centers exactly (no fudge offset);
                   inner radius = corner radius - the adjoining border width. */
                if (tl > 0) ui_fill_arc(ren, (SDL_FPoint){ rect.x + tl, rect.y + tl }, tl, SDL_max(tl - (float)c->width.top, 0.0f), 180.0f, 270.0f, c->color);
                if (tr > 0) ui_fill_arc(ren, (SDL_FPoint){ rect.x + rect.w - tr, rect.y + tr }, tr, SDL_max(tr - (float)c->width.top, 0.0f), 270.0f, 360.0f, c->color);
                if (bl > 0) ui_fill_arc(ren, (SDL_FPoint){ rect.x + bl, rect.y + rect.h - bl }, bl, SDL_max(bl - (float)c->width.bottom, 0.0f), 90.0f, 180.0f, c->color);
                if (br > 0) ui_fill_arc(ren, (SDL_FPoint){ rect.x + rect.w - br, rect.y + rect.h - br }, br, SDL_max(br - (float)c->width.bottom, 0.0f), 0.0f, 90.0f, c->color);
            } break;

            case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START: {
                SDL_Rect clip = { (int)bb.x, (int)bb.y, (int)bb.width, (int)bb.height };
                SDL_SetRenderClipRect(ren, &clip);
            } break;
            case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END:
                SDL_SetRenderClipRect(ren, NULL);
                break;

            case CLAY_RENDER_COMMAND_TYPE_IMAGE: {
                Clay_ImageRenderData *c = &cmd->renderData.image;
                SDL_Texture *tex = (SDL_Texture *)c->imageData;
                if (tex){
                    /* backgroundColor is the image tint: our icons are rasterized white,
                       so color-mod recolors them to any theme color (alpha = AA edges). */
                    if (c->backgroundColor.a > 0.0f)
                        SDL_SetTextureColorMod(tex, (Uint8)c->backgroundColor.r, (Uint8)c->backgroundColor.g, (Uint8)c->backgroundColor.b);
                    else
                        SDL_SetTextureColorMod(tex, 255, 255, 255);
                    SDL_RenderTexture(ren, tex, NULL, &rect);
                }
            } break;

            default: break;
        }
    }
}

#endif /* UI_RENDER_H */
