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

/* faint centered text filling the remaining space; marks an empty region */
static void ui_placeholder(const Palette *P, Clay_String txt){
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0) },
                       .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } } }) {
        CLAY_TEXT(txt, CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_BODY), .textColor = P->faint }));
    }
}

#endif /* UI_WIDGETS_H */
