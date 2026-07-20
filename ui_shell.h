/* The app shell: a 48px top bar (logo, tabs, theme toggle) above a body that
   dispatches to the active tab. ui_frame() builds the whole Clay tree and is
   called between Clay_BeginLayout and Clay_EndLayout. Requires the tab headers
   first. */
#ifndef UI_SHELL_H
#define UI_SHELL_H

static void ui_logo(const Palette *P){
    CLAY({ .layout = { .childGap = (uint16_t)UISC(8), .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                       .padding = { .left = (uint16_t)UISC(2), .right = (uint16_t)UISC(8) } } }) {
        ui_icon(ICON_LOGO, 22, UI_NONE);   /* Logo.svg, full color (drawn untinted) */
        CLAY_TEXT(CLAY_STRING("DART"), CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_BOLD, FS_VALUE),
                                                          .textColor = P->text, .wrapMode = CLAY_TEXT_WRAP_NONE }));
    }
}

/* one top-bar tab: icon + name + faint count, accent underline + color when active.
   Active state is shown by color + underline, not weight (matches the mockup). */
static void ui_tab(AppState *app, const Palette *P, IconId icon, Clay_String name, int count, Tab tab){
    int active = app->tab == tab;
    CLAY({ .layout = { .sizing = { .height = CLAY_SIZING_FIXED(UISC(48)) },
                       .padding = { .left = (uint16_t)UISC(13), .right = (uint16_t)UISC(13) },
                       .childGap = (uint16_t)UISC(7),
                       .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } },
           .border = { .width = { 0, 0, 0, active ? (uint16_t)UISC(2) : 0, 0 }, .color = P->accent } }) {
        if (Clay_Hovered() && g_pointer_pressed) app->tab = tab;
        ui_icon(icon, 15, active ? P->text : P->dim);
        CLAY_TEXT(name, CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_SEMI, FS_BODY),
                                           .textColor = active ? P->text : P->dim,
                                           .wrapMode = CLAY_TEXT_WRAP_NONE }));
        CLAY_TEXT(ui_fmt("%d", count), CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_CAPTION),
                                                          .textColor = P->faint, .wrapMode = CLAY_TEXT_WRAP_NONE }));
    }
}

/* the theme toggle: an icon-only button (no text, no border) whose icon shows the
   CURRENT mode -- moon while dark, sun while light. */
static void ui_theme_toggle(AppState *app, const Palette *P){
    if (ui_icon_button(P, app->theme_dark ? ICON_MOON : ICON_SUN, 17, 30, P->dim, P->text))
        app->theme_dark = !app->theme_dark;
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
        ui_tab(app, P, ICON_BOX,    CLAY_STRING("Nodes"),  D ? D->n_nodes  : 0, TAB_NODES);
        ui_tab(app, P, ICON_RADIO,  CLAY_STRING("Topics"), D ? D->n_topics : 0, TAB_TOPICS);
        ui_tab(app, P, ICON_LOGS,   CLAY_STRING("Log"),    D ? D->n_logs   : 0, TAB_LOG);
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}   /* spacer */
        ui_theme_toggle(app, P);
    }
}

/* build the whole UI tree for this frame */
static void ui_frame(AppState *app){
    const Palette *P = app->theme_dark ? &UI_DARK : &UI_LIGHT;
    ui_scrollbars_pre();   /* drag/page any scrollbar, consuming the press before rows see it */
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
