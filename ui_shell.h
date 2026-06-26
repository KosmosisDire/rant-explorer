/* The app shell: a 48px top bar (logo, tabs, theme toggle) above a body that
   dispatches to the active tab. ui_frame() builds the whole Clay tree and is
   called between Clay_BeginLayout and Clay_EndLayout. Requires the tab headers
   first. */
#ifndef UI_SHELL_H
#define UI_SHELL_H

static void ui_logo(const Palette *P){
    CLAY({ .layout = { .childGap = (uint16_t)UISC(9), .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                       .padding = { .left = (uint16_t)UISC(2), .right = (uint16_t)UISC(8) } } }) {
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_FIXED(UISC(13)), .height = CLAY_SIZING_FIXED(UISC(13)) } },
               .backgroundColor = P->accent, .cornerRadius = CLAY_CORNER_RADIUS(UISC(3)) }) {}
        CLAY_TEXT(CLAY_STRING("DART"), CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_BOLD, FS_VALUE),
                                                          .textColor = P->text, .wrapMode = CLAY_TEXT_WRAP_NONE }));
    }
}

/* one top-bar tab: name + faint count, accent underline + text color when active.
   Active state is shown by color + underline, not weight (matches the mockup). */
static void ui_tab(AppState *app, const Palette *P, Clay_String name, int count, Tab tab){
    int active = app->tab == tab;
    CLAY({ .layout = { .sizing = { .height = CLAY_SIZING_FIXED(UISC(48)) },
                       .padding = { .left = (uint16_t)UISC(13), .right = (uint16_t)UISC(13) },
                       .childGap = (uint16_t)UISC(6),
                       .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } },
           .border = { .width = { 0, 0, 0, active ? (uint16_t)UISC(2) : 0, 0 }, .color = P->accent } }) {
        if (Clay_Hovered() && g_pointer_pressed) app->tab = tab;
        CLAY_TEXT(name, CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_SEMI, FS_BODY),
                                           .textColor = active ? P->text : P->dim,
                                           .wrapMode = CLAY_TEXT_WRAP_NONE }));
        CLAY_TEXT(ui_fmt("%d", count), CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_CAPTION),
                                                          .textColor = P->faint, .wrapMode = CLAY_TEXT_WRAP_NONE }));
    }
}

static void ui_topbar(AppState *app, const Palette *P){
    const Dataset *D = app->data;
    CLAY({ .id = CLAY_ID("topbar"),
           .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(UISC(48)) },
                       .padding = { .left = (uint16_t)UISC(16), .right = (uint16_t)UISC(16) },
                       .childGap = (uint16_t)UISC(2), .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
           .backgroundColor = P->panel,
           .border = { .width = { 0, 0, 0, (uint16_t)1, 0 }, .color = P->border } }) {
        ui_logo(P);
        ui_tab(app, P, CLAY_STRING("Nodes"),  D ? D->n_nodes  : 0, TAB_NODES);
        ui_tab(app, P, CLAY_STRING("Topics"), D ? D->n_topics : 0, TAB_TOPICS);
        ui_tab(app, P, CLAY_STRING("Log"),    D ? D->n_logs   : 0, TAB_LOG);
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}   /* spacer */
        if (ui_pill(P, app->theme_dark ? CLAY_STRING("Light") : CLAY_STRING("Dark"),
                    FAM_SANS, WT_REG, FS_SMALL, P->dim, P->panel, P->border2, UISC(28)))
            app->theme_dark = !app->theme_dark;
    }
}

/* build the whole UI tree for this frame */
static void ui_frame(AppState *app){
    const Palette *P = app->theme_dark ? &UI_DARK : &UI_LIGHT;
    CLAY({ .id = CLAY_ID("root"),
           .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0) },
                       .layoutDirection = CLAY_TOP_TO_BOTTOM },
           .backgroundColor = P->bg }) {
        ui_topbar(app, P);
        CLAY({ .id = CLAY_ID("body"),
               .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0) } } }) {
            switch (app->tab) {
                case TAB_NODES:  nodes_tab(app, P);  break;
                case TAB_TOPICS: topics_tab(app, P); break;
                case TAB_LOG:    log_tab(app, P);    break;
            }
        }
    }
}

#endif /* UI_SHELL_H */
