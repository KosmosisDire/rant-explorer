/* Reusable Clay leaf/component helpers. Components with a fixed internal
   structure (dot, chip, pill, label) are plain functions that emit a CLAY block;
   container layouts with caller-provided children stay inline in the tab files.

   Click handling: Clay_Hovered() reports pointer-over for the open element;
   combined with g_pointer_pressed (set once per frame from the mouse) it yields a
   click. Requires clay.h, ui_theme.h, ui_fonts.h first. */
#ifndef UI_WIDGETS_H
#define UI_WIDGETS_H

/* UISC yields a float; Clay's padding/childGap/border-width fields are uint16_t.
   UISCI scales and casts in one step, to keep layout literals readable. */
#define UISCI(px) ((uint16_t)UISC(px))

static bool g_pointer_pressed = false;   /* left mouse pressed this frame; main sets it */
static bool g_right_pressed = false;     /* right mouse pressed this frame (context menus) */
static bool g_pointer_pressed_raw = false;   /* like the two above, but never consumed by a */
static bool g_right_pressed_raw = false;     /* widget: dismiss checks see every press */
static uint32_t g_now_ms = 0;            /* monotonic ms this frame; main sets it (transient feedback) */
static float g_pointer_x = 0.0f, g_pointer_y = 0.0f;   /* pointer position, physical px; main sets it */
static float g_view_w = 0.0f, g_view_h = 0.0f;         /* render output size, physical px; main sets it */

/* a filled or hollow status dot (a circle = a rect with full corner radius) */
static void ui_dot(float d, Clay_Color fill, Clay_Color border){
    uint16_t bw = (uint16_t)(border.a > 0 ? 1 : 0);
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_FIXED(d), .height = CLAY_SIZING_FIXED(d) } },
           .backgroundColor = fill, .cornerRadius = CLAY_CORNER_RADIUS(d * 0.5f),
           .border = { .width = { bw, bw, bw, bw, 0 }, .color = border } }) {}
}

/* a small uppercase section header: caption size, normal weight, faint */
static void ui_section_label(const Palette *P, Clay_String txt){
    CLAY_TEXT(txt, CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_CAPTION),
                                      .textColor = P->faint, .letterSpacing = 1,
                                      .wrapMode = CLAY_TEXT_WRAP_NONE }));
}

/* a compact, centered-text button. Returns true on click. border.a==0 = no border. */
static bool ui_pill(const Palette *P, Clay_String label, FontFamily fam, FontWeight wt, FontSize sz,
                    Clay_Color fg, Clay_Color bg, Clay_Color border, float height){
    bool clicked = false;
    uint16_t bw = (uint16_t)(border.a > 0 ? 1 : 0);
    CLAY({ .layout = { .sizing = { .height = CLAY_SIZING_FIXED(height) },
                       .padding = { .left = (uint16_t)UISC(12), .right = (uint16_t)UISC(12) },
                       .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } },
           .backgroundColor = bg, .cornerRadius = CLAY_CORNER_RADIUS(UISC(5)),
           .border = { .width = { bw, bw, bw, bw, 0 }, .color = border } }) {
        if (Clay_Hovered() && g_pointer_pressed) clicked = true;
        CLAY_TEXT(label, CLAY_TEXT_CONFIG({ UI_FONT(fam, wt, sz), .textColor = fg,
                                            .wrapMode = CLAY_TEXT_WRAP_NONE }));
    }
    return clicked;
}

/* a flat endpoint chip: rounded panel2 pill, leading dot, mono label. Click = navigate. */
static bool ui_chip(const Palette *P, Clay_String label, Clay_Color dot){
    bool clicked = false;
    CLAY({ .layout = { .padding = { .left = (uint16_t)UISC(9), .right = (uint16_t)UISC(9),
                                    .top = (uint16_t)UISC(5), .bottom = (uint16_t)UISC(5) },
                       .childGap = (uint16_t)UISC(6), .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
           .backgroundColor = P->panel2, .cornerRadius = CLAY_CORNER_RADIUS(UISC(4)),
           .border = { .width = CLAY_BORDER_OUTSIDE(1), .color = P->border } }) {
        if (Clay_Hovered() && g_pointer_pressed) clicked = true;
        ui_dot(UISC(6), dot, dot);
        CLAY_TEXT(label, CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_SMALL), .textColor = P->text }));
    }
    return clicked;
}

/* a square icon button: the icon centered in a box, faint hover fill. Returns true
   on click. fg/hover_fg tint the icon; box is the clickable square's side (px). */
static bool ui_icon_button(const Palette *P, IconId id, float icon_px, float box,
                           Clay_Color fg, Clay_Color hover_fg){
    bool clicked = false, hov;
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_FIXED(UISC(box)), .height = CLAY_SIZING_FIXED(UISC(box)) },
                       .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } },
           .cornerRadius = CLAY_CORNER_RADIUS(UISC(5)),
           .backgroundColor = Clay_Hovered() ? P->panel2 : UI_NONE }) {   /* hover wash behind the icon */
        hov = Clay_Hovered();
        if (hov && g_pointer_pressed) clicked = true;
        ui_icon(id, icon_px, hov ? hover_fg : fg);
    }
    return clicked;
}

/* a small copy-to-clipboard pill: copy icon + label, dim -> text on hover. After a copy the
   caller passes copied=1 for a moment, flipping it to a green check + "Copied". Click = true. */
static bool ui_copy_pill(const Palette *P, int copied){
    bool clicked = false, hov;
    Clay_Color fg;
    CLAY({ .layout = { .sizing = { .height = CLAY_SIZING_FIXED(UISC(22)) },
                       .padding = { .left = (uint16_t)UISC(7), .right = (uint16_t)UISC(8) },
                       .childGap = (uint16_t)UISC(5),
                       .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } },
           .backgroundColor = Clay_Hovered() ? P->panel2 : UI_NONE,
           .cornerRadius = CLAY_CORNER_RADIUS(UISC(4)),
           .border = { .width = CLAY_BORDER_OUTSIDE(1), .color = copied ? P->green : P->border } }) {
        hov = Clay_Hovered();
        if (hov && g_pointer_pressed) clicked = true;
        fg = copied ? P->green : (hov ? P->text : P->dim);
        ui_icon(copied ? ICON_CHECK : ICON_COPY, 12, fg);
        CLAY_TEXT(copied ? CLAY_STRING("Copied") : CLAY_STRING("Copy DSL"),
                  CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_SEMI, FS_CAPTION), .textColor = fg,
                                     .wrapMode = CLAY_TEXT_WRAP_NONE }));
    }
    return clicked;
}

/* ---- generic context menu: one open at a time, keyed by an owner pointer ---- */

enum { UI_MENU_ON = 1, UI_MENU_OFF };   /* UiMenuItem.check states (0 = plain item) */

typedef struct {
    Clay_String label;
    Clay_String keys;     /* right-aligned shortcut hint ("" = none) */
    int enabled;          /* 0 = dimmed, not clickable */
    int check;            /* 0 = plain item; UI_MENU_ON/OFF = a leading checkbox, and a
                             click reports the toggle but keeps the menu open (checklists) */
} UiMenuItem;

static const void *g_menu_owner = NULL;   /* whose menu is open (NULL = none) */
static float g_menu_x, g_menu_y;          /* anchor, physical px */
static int   g_menu_fresh = 0;            /* opened this frame: skip the dismiss checks once */

static void ui_menu_open(const void *owner, float x, float y){
    g_menu_owner = owner; g_menu_x = x; g_menu_y = y; g_menu_fresh = 1;
}
static void ui_menu_close(void){ g_menu_owner = NULL; }
static int  ui_menu_is_open(const void *owner){ return g_menu_owner == owner; }
static int  ui_menu_pointer_over(void){ return g_menu_owner && Clay_PointerOver(CLAY_ID("ui_ctx_menu")); }

/* one menu row; returns true on click */
static bool ui_menu_item(const Palette *P, const UiMenuItem *it){
    bool clicked = false;
    int on = it->check == UI_MENU_ON;
    Clay_Color fg = !it->enabled ? P->faint : (it->check == UI_MENU_OFF ? P->dim : P->text);
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(UISC(24)) },
                       .padding = { .left = UISCI(10), .right = UISCI(10) }, .childGap = UISCI(18),
                       .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
           .backgroundColor = it->enabled && Clay_Hovered() ? P->panel3 : UI_NONE }) {
        if (it->enabled && Clay_Hovered() && g_pointer_pressed){ clicked = true; g_pointer_pressed = false; }
        /* the checkbox: accent box + check when on, empty bordered box when off */
        if (it->check)
            CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_FIXED(UISC(15)), .height = CLAY_SIZING_FIXED(UISC(15)) },
                               .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } },
                   .backgroundColor = on ? P->accent_bg : UI_NONE,
                   .cornerRadius = CLAY_CORNER_RADIUS(UISC(3)),
                   .border = { .width = CLAY_BORDER_OUTSIDE(1), .color = on ? P->accent : P->border2 } }) {
                if (on) ui_icon(ICON_CHECK, 11, P->accent);
            }
        CLAY_TEXT(it->label, CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_SMALL),
                                                .textColor = fg,
                                                .wrapMode = CLAY_TEXT_WRAP_NONE }));
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
        if (it->keys.length)
            CLAY_TEXT(it->keys, CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_CAPTION),
                                                   .textColor = P->faint, .wrapMode = CLAY_TEXT_WRAP_NONE }));
    }
    return clicked;
}

/* draw the owner's open menu: floating at the anchor, clamped to the window, z
   above everything. Returns the clicked item index or -1; a plain item closes
   the menu, a check item leaves it open so the caller's toggle shows next
   frame. Any press outside it dismisses it. Call every frame while open; a
   no-op (-1) when this owner's menu isn't. */
static int ui_menu(const Palette *P, const void *owner, const UiMenuItem *items, int n, float w){
    int clicked = -1, i;
    float mxp = g_menu_x, myp = g_menu_y, mh = (float)n * UISC(24) + UISC(8);
    if (!ui_menu_is_open(owner)) return -1;
    if (g_menu_fresh) g_menu_fresh = 0;      /* the opening press must not also dismiss */
    else if ((g_pointer_pressed_raw || g_right_pressed_raw) && !ui_menu_pointer_over()){
        ui_menu_close();
        return -1;
    }
    if (g_view_w > 0 && mxp > g_view_w - UISC(w) - UISC(10)) mxp = g_view_w - UISC(w) - UISC(10);
    if (g_view_h > 0 && myp > g_view_h - mh - UISC(10))      myp = g_view_h - mh - UISC(10);
    CLAY({ .id = CLAY_ID("ui_ctx_menu"),
           .layout = { .sizing = { .width = CLAY_SIZING_FIXED(UISC(w)) },
                       .layoutDirection = CLAY_TOP_TO_BOTTOM, .padding = CLAY_PADDING_ALL(UISC(4)) },
           .backgroundColor = P->panel, .cornerRadius = CLAY_CORNER_RADIUS(UISC(6)),
           .border = { .width = { 1, 1, 1, 1, 0 }, .color = P->border2 },
           .floating = { .offset = { mxp, myp }, .zIndex = 1000,
                         .attachTo = CLAY_ATTACH_TO_ROOT } }) {
        for (i = 0; i < n; i++)
            if (ui_menu_item(P, &items[i])) clicked = i;
    }
    if (clicked >= 0 && !items[clicked].check) ui_menu_close();
    return clicked;
}

/* ---- generic vertical scrollbar for Clay clip/scroll containers ----

   Clay itself has no scrollbar: it only exposes each clip container's live scroll
   position + content/viewport sizes (Clay_GetScrollContainerData), leaving the bar
   to the app. This draws one for ANY vertical scroll container by id, as a floating
   overlay pinned to the container's right edge, and makes it draggable.

   Two phases per frame, because a bar overlays the rows behind it and a press must
   reach the bar first (Clay_Hovered has no z-order, so a floating bar can't steal a
   press from a row underneath it). ui_scrollbars_pre() runs at the TOP of the frame
   over LAST frame's set of bars: using last frame's geometry it handles thumb drag /
   track paging and CONSUMES the press before any row sees it. Then ui_scrollbar()
   at each container site both registers the id (for next frame's pre pass) and draws
   the bar for the current scroll state. Wheel scrolling is Clay's already. */

#define UI_SB_W    10.0f    /* bar width, css px */
#define UI_SB_MIN  28.0f    /* minimum thumb length, css px */
#define UI_SB_MAX  32       /* max distinct scroll containers tracked per frame */

static int      g_mouse_held = 0;              /* left button currently down; main sets it each frame */
static uint32_t g_sb_ids[UI_SB_MAX]; static int g_sb_n = 0;   /* ids drawn this frame (rolls to "last frame") */
static uint32_t g_sb_drag = 0;                 /* id of the bar being dragged (0 = none) */
static float    g_sb_grab = 0.0f;              /* pointer offset within the thumb at grab, physical px */

/* thumb/track geometry derived from a container's scroll data (physical px) */
typedef struct { int scrollable; float view_h, range, track_h, thumb_h, inset; } UiSbGeom;
static UiSbGeom ui_sb_geom(const Clay_ScrollContainerData *sd){
    UiSbGeom g; float content_h;
    g.scrollable = 0; g.range = g.track_h = g.thumb_h = 0.0f;
    g.inset  = UISC(2);
    g.view_h = sd->scrollContainerDimensions.height;
    content_h = sd->contentDimensions.height;
    if (!sd->scrollPosition || content_h <= g.view_h + 0.5f) return g;   /* content fits: no bar */
    g.range   = content_h - g.view_h;
    g.track_h = g.view_h - 2.0f * g.inset;
    g.thumb_h = g.track_h * (g.view_h / content_h);
    if (g.thumb_h < UISC(UI_SB_MIN)) g.thumb_h = UISC(UI_SB_MIN);
    if (g.thumb_h > g.track_h)       g.thumb_h = g.track_h;
    g.scrollable = 1;
    return g;
}

/* set a container's scroll offset from a thumb-top position (physical px within the track) */
static void ui_sb_set_from_thumb(const Clay_ScrollContainerData *sd, const UiSbGeom *g, float thumb_top){
    float denom = g->track_h - g->thumb_h;
    float t = denom > 0.5f ? thumb_top / denom : 0.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    sd->scrollPosition->y = -(t * g->range);
}

/* interaction pass: run once at the top of the frame, before any content. Drives drag
   and track paging for whichever bar the pointer is on, using last frame's geometry,
   and consumes the press so rows behind the bar don't also react. */
static void ui_scrollbars_pre(void){
    int i, n = g_sb_n;
    uint32_t ids[UI_SB_MAX];
    for (i = 0; i < n; i++) ids[i] = g_sb_ids[i];   /* snapshot last frame's set */
    g_sb_n = 0;                                     /* this frame's containers refill it */

    for (i = 0; i < n; i++){
        Clay_ElementId id = { .id = ids[i] };
        Clay_ScrollContainerData sd = Clay_GetScrollContainerData(id);
        Clay_ElementData ed = Clay_GetElementData(id);
        UiSbGeom g;
        float bar_x, top;
        if (!sd.found || !ed.found){ if (g_sb_drag == id.id) g_sb_drag = 0; continue; }
        g = ui_sb_geom(&sd);
        if (!g.scrollable){ if (g_sb_drag == id.id) g_sb_drag = 0; continue; }
        bar_x = ed.boundingBox.x + ed.boundingBox.width - UISC(UI_SB_W);
        top   = ed.boundingBox.y;

        if (g_sb_drag == id.id){                    /* continue an in-progress drag */
            if (g_mouse_held) ui_sb_set_from_thumb(&sd, &g, g_pointer_y - top - g.inset - g_sb_grab);
            else              g_sb_drag = 0;
            continue;
        }
        /* a fresh press inside the bar column starts a drag (on the thumb) or pages (on the track) */
        if (g_pointer_pressed &&
            g_pointer_x >= bar_x && g_pointer_x <= bar_x + UISC(UI_SB_W) &&
            g_pointer_y >= top   && g_pointer_y <= top + g.view_h){
            float pos = -sd.scrollPosition->y;
            float thumb_sy;
            if (pos < 0.0f) pos = 0.0f;
            if (pos > g.range) pos = g.range;
            thumb_sy = top + g.inset + (g.track_h - g.thumb_h) * (g.range > 0.0f ? pos / g.range : 0.0f);
            if (g_pointer_y >= thumb_sy && g_pointer_y <= thumb_sy + g.thumb_h){
                g_sb_grab = g_pointer_y - thumb_sy;          /* grabbed the thumb where it sits */
            } else {
                g_sb_grab = g.thumb_h * 0.5f;                /* clicked the track: center thumb on cursor */
                ui_sb_set_from_thumb(&sd, &g, g_pointer_y - top - g.inset - g_sb_grab);
            }
            g_sb_drag = id.id;
            g_pointer_pressed = false;                        /* consume: rows under the bar must not fire */
        }
    }
}

/* draw the bar for one scroll container and register it for next frame's pre pass.
   Call immediately after the container's CLAY{} block closes, with the same id.
   A no-op bar (just the registration) when the content fits. */
static void ui_scrollbar(const Palette *P, Clay_ElementId id){
    Clay_ScrollContainerData sd = Clay_GetScrollContainerData(id);
    Clay_ElementData ed;
    UiSbGeom g;
    float pos, thumb_y;
    int hot;
    if (g_sb_n < UI_SB_MAX) g_sb_ids[g_sb_n++] = id.id;   /* register (even when it fits, so drags end cleanly) */
    if (!sd.found) return;
    g = ui_sb_geom(&sd);
    if (!g.scrollable) return;
    pos = -sd.scrollPosition->y;
    if (pos < 0.0f) pos = 0.0f;
    if (pos > g.range) pos = g.range;
    thumb_y = g.inset + (g.track_h - g.thumb_h) * (g.range > 0.0f ? pos / g.range : 0.0f);

    ed = Clay_GetElementData(id);                          /* hover: pointer within the bar column */
    hot = g_sb_drag == id.id ||
          (ed.found &&
           g_pointer_x >= ed.boundingBox.x + ed.boundingBox.width - UISC(UI_SB_W) &&
           g_pointer_x <= ed.boundingBox.x + ed.boundingBox.width &&
           g_pointer_y >= ed.boundingBox.y &&
           g_pointer_y <= ed.boundingBox.y + g.view_h);

    /* floating track pinned to the container's right edge; the thumb sits at thumb_y */
    CLAY({ .floating = { .attachTo = CLAY_ATTACH_TO_ELEMENT_WITH_ID, .parentId = id.id,
                         .attachPoints = { .element = CLAY_ATTACH_POINT_RIGHT_TOP,
                                           .parent  = CLAY_ATTACH_POINT_RIGHT_TOP },
                         .zIndex = 600 },
           .layout = { .sizing = { .width = CLAY_SIZING_FIXED(UISC(UI_SB_W)),
                                   .height = CLAY_SIZING_FIXED(g.view_h) },
                       .layoutDirection = CLAY_TOP_TO_BOTTOM,
                       .padding = { .left = UISCI(2), .right = UISCI(2), .top = (uint16_t)thumb_y } } }) {
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0),
                                       .height = CLAY_SIZING_FIXED(g.thumb_h) } },
               .backgroundColor = hot ? P->dim : P->border2,
               .cornerRadius = CLAY_CORNER_RADIUS(UISC(3)) }) {}
    }
}

/* faint centered text filling the remaining space; marks an empty region */
static void ui_placeholder(const Palette *P, Clay_String txt){
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0) },
                       .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } } }) {
        CLAY_TEXT(txt, CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_BODY), .textColor = P->faint }));
    }
}

#endif /* UI_WIDGETS_H */
