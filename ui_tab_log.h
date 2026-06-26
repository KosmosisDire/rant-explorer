/* Log tab: one full-width diagnostic stream. Skeleton renders the header strip
   and an empty table. */
#ifndef UI_TAB_LOG_H
#define UI_TAB_LOG_H

static void log_tab(AppState *app, const Palette *P){
    (void)app;
    CLAY({ .id = CLAY_ID("log"),
           .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0) },
                       .layoutDirection = CLAY_TOP_TO_BOTTOM, .padding = CLAY_PADDING_ALL(UISC(12)) },
           .backgroundColor = P->bg }) {
        ui_section_label(P, CLAY_STRING("ALL SOURCES"));
        ui_placeholder(P, CLAY_STRING("log"));
    }
}

#endif /* UI_TAB_LOG_H */
