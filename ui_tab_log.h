/* Log tab: the observer's event stream, newest first. This is the live discovery
   diagnostic the observer already produces (peer UP/DOWN/REFUSED, decoded meta:
   name, frag, pub/sub interest), read straight from the capture snapshot. A node's
   own transport/qos/shm logs aren't on the wire, so this is the discovery-level
   slice of the handoff's "general diagnostic stream". Requires ui_widgets.h. */
#ifndef UI_TAB_LOG_H
#define UI_TAB_LOG_H

/* severity tint from keywords in the formatted line (the observer logs free text) */
static Clay_Color log_color(const Palette *P, const char *line){
    if (strstr(line, "REFUSED")) return P->red;
    if (strstr(line, "GONE"))    return P->amber;
    if (strstr(line, "DOWN"))    return P->amber;
    if (strstr(line, "UP "))     return P->green;
    return P->dim;
}
/* a wrapped continuation line ("]  <spaces>...") is the decoded-meta detail of the
   event above it; render it de-emphasized and with no dot */
static int log_is_cont(const char *line){
    const char *b = strchr(line, ']');
    return b && b[1] == ' ' && b[2] == ' ';
}

static void log_row(const Palette *P, const char *line, int idx){
    int cont = log_is_cont(line);
    CLAY({ .id = CLAY_IDI("log_row", (uint32_t)idx),
           .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) },
                       .padding = { .left = UISCI(16), .right = UISCI(16), .top = UISCI(5), .bottom = UISCI(5) },
                       .childGap = UISCI(9), .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_FIXED(UISC(8)) },
                           .childAlignment = { .x = CLAY_ALIGN_X_CENTER } } }) {
            if (!cont) ui_dot(UISC(7), log_color(P, line), UI_NONE);
        }
        CLAY_TEXT(ui_str(line), CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_SMALL),
                                                   .textColor = cont ? P->faint : P->text,
                                                   .wrapMode = CLAY_TEXT_WRAP_WORDS }));
    }
}

static void log_tab(AppState *app, const Palette *P){
    const CapSnapshot *s = app->snap;
    int n = s ? s->n_log : 0, i;
    CLAY({ .id = CLAY_ID("log"),
           .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0) },
                       .layoutDirection = CLAY_TOP_TO_BOTTOM },
           .backgroundColor = P->bg }) {
        /* header strip */
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(UISC(40)) },
                           .padding = { .left = UISCI(16), .right = UISCI(16) },
                           .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
               .border = { .width = { 0, 0, 0, UISCI(1), 0 }, .color = P->border } }) {
            ui_section_label(P, CLAY_STRING("OBSERVER EVENT STREAM"));
            CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
            CLAY_TEXT(ui_fmt("%d events  \xC2\xB7  newest first", n),
                      CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_CAPTION), .textColor = P->faint,
                                         .wrapMode = CLAY_TEXT_WRAP_NONE }));
        }
        /* scrolling stream */
        CLAY({ .id = CLAY_ID("log_scroll"),
               .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0) },
                           .layoutDirection = CLAY_TOP_TO_BOTTOM },
               .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() } }) {
            if (n == 0) ui_placeholder(P, CLAY_STRING("waiting for discovery traffic..."));
            for (i = 0; i < n; i++) log_row(P, s->log[i], i);
        }
        ui_scrollbar(P, CLAY_ID("log_scroll"));
    }
}

#endif /* UI_TAB_LOG_H */
