/* Nodes tab: left node list (264) + scrolling detail pane. Skeleton renders the
   two regions; bodies land when the Dataset is populated. */
#ifndef UI_TAB_NODES_H
#define UI_TAB_NODES_H

static void nodes_tab(AppState *app, const Palette *P){
    (void)app;
    /* left: node list */
    CLAY({ .id = CLAY_ID("nodes_list"),
           .layout = { .sizing = { .width = CLAY_SIZING_FIXED(UISC(264)), .height = CLAY_SIZING_GROW(0) },
                       .layoutDirection = CLAY_TOP_TO_BOTTOM, .padding = CLAY_PADDING_ALL(UISC(12)),
                       .childGap = (uint16_t)UISC(6) },
           .backgroundColor = P->panel,
           .border = { .width = { 0, (uint16_t)1, 0, 0, 0 }, .color = P->border } }) {
        ui_section_label(P, CLAY_STRING("NODES"));
        ui_placeholder(P, CLAY_STRING("node list"));
    }
    /* center: detail */
    CLAY({ .id = CLAY_ID("nodes_detail"),
           .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0) } },
           .backgroundColor = P->bg }) {
        ui_placeholder(P, CLAY_STRING("select a node"));
    }
}

#endif /* UI_TAB_NODES_H */
