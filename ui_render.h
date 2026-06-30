/* Vendored Clay -> SDL3 renderer with grayscale-AA text.

   Replaces Clay's stock renderers/SDL3/clay_renderer_SDL3.c. We only ever used
   its rounded-rect + arc fills, so those are ported below (MIT, (c) Clay
   authors); the rest of that file (measure function, SDL_image/SDL_main pulls)
   is dropped. ui_render() runs the command loop itself; text goes through
   TTF_RenderText_Blended, whose alpha-channel output blends over whatever is
   already in the target, so no background reconstruction is needed.
   Requires clay.h, SDL3, SDL3_ttf. */
#ifndef UI_RENDER_H
#define UI_RENDER_H

#include "clay.h"
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

typedef struct {
    SDL_Renderer *renderer;
    TTF_Font    **fonts;     /* indexed by Clay fontId */
} UiRenderer;

#define UI_CIRCLE_SEGMENTS 16

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
    /* heap, not a VLA: MSVC has no C99 variable-length arrays, and the counts grow
       with the corner radius so a fixed stack buffer would have to be huge. */
    SDL_Vertex *vertices = (SDL_Vertex *)malloc((size_t)totalVertices * sizeof *vertices);
    int        *indices  = (int *)       malloc((size_t)totalIndices  * sizeof *indices);
    if (!vertices || !indices){ free(vertices); free(indices); return; }

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
                default: free(vertices); free(indices); return;
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
    free(vertices);
    free(indices);
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
    /* heap, not a VLA (MSVC); counts grow with the corner radius. */
    SDL_Vertex *vertices = (SDL_Vertex *)malloc((size_t)vtxCount * sizeof *vertices);
    int        *indices  = (int *)       malloc((size_t)idxCount * sizeof *indices);
    if (!vertices || !indices){ free(vertices); free(indices); return; }
    int vc = 0, ic = 0;
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
    free(vertices);
    free(indices);
}

/* run a Clay command array: rects/borders direct, text alpha-blended over them. */
static void ui_render(UiRenderer *rd, Clay_RenderCommandArray *cmds){
    SDL_Renderer *ren = rd->renderer;

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
                SDL_Surface *s;
                if (!c->stringContents.length || !font) break;
                s = TTF_RenderText_Blended(font, c->stringContents.chars, (size_t)c->stringContents.length, fg);
                if (s){
                    SDL_Texture *t = SDL_CreateTextureFromSurface(ren, s);
                    if (t){
                        SDL_FRect dst = { rect.x, rect.y, (float)s->w, (float)s->h };   /* integer pos, 1:1 size */
                        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_NEAREST);
                        SDL_RenderTexture(ren, t, NULL, &dst);
                        SDL_DestroyTexture(t);
                    }
                    SDL_DestroySurface(s);
                }
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
