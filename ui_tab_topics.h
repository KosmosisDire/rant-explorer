/* Topics tab: topic tree (264) + live message feed (grow) + inline Inspect/Publish
   drawer (332, only when open). Skeleton renders each region's chrome with empty
   bodies. */
#ifndef UI_TAB_TOPICS_H
#define UI_TAB_TOPICS_H

static void ui_filter_box(const Palette *P, Clay_String hint){
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(UISC(30)) },
                       .padding = { .left = (uint16_t)UISC(9), .right = (uint16_t)UISC(9) },
                       .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
           .backgroundColor = P->panel2, .cornerRadius = CLAY_CORNER_RADIUS(UISC(5)),
           .border = { .width = CLAY_BORDER_OUTSIDE(1), .color = P->border } }) {
        CLAY_TEXT(hint, CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_SMALL), .textColor = P->faint,
                                           .wrapMode = CLAY_TEXT_WRAP_NONE }));
    }
}

/* Inspect / Publish tabs (both semibold; active = accent color + wash) plus close */
static void topics_drawer_header(AppState *app, const Palette *P){
    int insp = app->drawer_mode == DRW_INSPECT;
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(UISC(40)) },
                       .padding = { .left = (uint16_t)UISC(12), .right = (uint16_t)UISC(6) },
                       .childGap = (uint16_t)UISC(2), .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
           .border = { .width = { 0, 0, 0, (uint16_t)1, 0 }, .color = P->border } }) {
        if (ui_pill(P, CLAY_STRING("Inspect"), FAM_SANS, WT_SEMI, FS_SMALL,
                    insp ? P->accent : P->dim, insp ? P->accent_bg : P->panel, UI_NONE, UISC(28)))
            app->drawer_mode = DRW_INSPECT;
        if (ui_pill(P, CLAY_STRING("Publish"), FAM_SANS, WT_SEMI, FS_SMALL,
                    !insp ? P->accent : P->dim, !insp ? P->accent_bg : P->panel, UI_NONE, UISC(28)))
            app->drawer_mode = DRW_PUBLISH;
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}   /* spacer */
        if (ui_pill(P, CLAY_STRING("x"), FAM_SANS, WT_REG, FS_BODY, P->dim, P->panel, UI_NONE, UISC(26)))
            app->drawer_open = 0;
    }
}

static void topics_tab(AppState *app, const Palette *P){
    /* tree column */
    CLAY({ .id = CLAY_ID("topics_tree"),
           .layout = { .sizing = { .width = CLAY_SIZING_FIXED(UISC(264)), .height = CLAY_SIZING_GROW(0) },
                       .layoutDirection = CLAY_TOP_TO_BOTTOM, .padding = CLAY_PADDING_ALL(UISC(12)),
                       .childGap = (uint16_t)UISC(8) },
           .backgroundColor = P->panel,
           .border = { .width = { 0, (uint16_t)1, 0, 0, 0 }, .color = P->border } }) {
        ui_filter_box(P, CLAY_STRING("Filter topics..."));
        ui_placeholder(P, CLAY_STRING("topic tree"));
        ui_section_label(P, CLAY_STRING("0 SUBSCRIBED  0 TOPICS"));
    }

    /* center: message feed */
    CLAY({ .id = CLAY_ID("topics_feed"),
           .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0) },
                       .layoutDirection = CLAY_TOP_TO_BOTTOM, .padding = CLAY_PADDING_ALL(UISC(12)) },
           .backgroundColor = P->bg }) {
        ui_section_label(P, CLAY_STRING("MESSAGE FEED"));
        ui_placeholder(P, CLAY_STRING("select a topic"));
    }

    /* right drawer (consumes width only when open) */
    if (app->drawer_open) {
        CLAY({ .id = CLAY_ID("topics_drawer"),
               .layout = { .sizing = { .width = CLAY_SIZING_FIXED(UISC(332)), .height = CLAY_SIZING_GROW(0) },
                           .layoutDirection = CLAY_TOP_TO_BOTTOM },
               .backgroundColor = P->panel,
               .border = { .width = { (uint16_t)1, 0, 0, 0, 0 }, .color = P->border } }) {
            topics_drawer_header(app, P);
            ui_placeholder(P, app->drawer_mode == DRW_INSPECT ? CLAY_STRING("inspect")
                                                              : CLAY_STRING("publish"));
        }
    }
}

#endif /* UI_TAB_TOPICS_H */
