/* Topics tab: topic tree (264) + center feed (grow) + right sidebar (332, only when open)
   that tabs between Inspect and Publish. Wired to live discovery: the tree, each topic's
   reliability, and its publisher/subscriber node lists are real (from the announce interest
   blobs). The center FEED is live too: the explorer subscribes to the selected topic on
   demand (cap_subscribe) and shows messages as they arrive, newest at the bottom, auto-scrolling
   while the user is parked there. A topic's status light is its subscription state: green
   subscribed, red dropping/erroring, hollow grey not subscribed. The feed is a TABLE: columns
   are the message's (flattened) fields, rows are samples, with a sticky header; clicking a row
   copies that sample into the Inspect tab, which shows its full decoded field hierarchy from a
   durable copy that survives the feed ring recycling. The Publish tab hosts the message
   composer (free-text, or a structured form on a typed topic). Durability/history/deadline QoS
   ride the data plane out of band and are never observable here, so the Inspect tab just
   doesn't show them.
   Requires ui_tree.h, ui_widgets.h, ui_model.h, ui_app.h, net_capture.h. */
#ifndef UI_TAB_TOPICS_H
#define UI_TAB_TOPICS_H

/* the selected topic's live feed, refetched from the capture each frame */
static CapFeedItem tt_feed[CAP_FEED_MAX];
static int         tt_feed_n, tt_subscribed, tt_sub_error, tt_sub_reliable;
static uint32_t    tt_msgs, tt_drops;

/* topic status light: hollow grey not subscribed, green subscribed, red dropping/erroring */
static void tt_status_dot(const Palette *P, int sub_state){
    if (sub_state == 0) ui_dot(UISC(7), UI_NONE, P->faint);
    else                ui_dot(UISC(7), sub_state == 2 ? P->red : P->green, UI_NONE);
}

/* copy a payload preview as printable ASCII (non-printable bytes become '.') into dst */
static void tt_sanitize(char *dst, int cap, const char *src, int n){
    int i, j = 0;
    for (i = 0; i < n && j < cap - 1; i++){
        unsigned char c = (unsigned char)src[i];
        dst[j++] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
    }
    dst[j] = '\0';
}

static void ui_filter_box(const Palette *P, Clay_String hint){
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(UISC(30)) },
                       .padding = { .left = UISCI(9), .right = UISCI(9) }, .childGap = UISCI(7),
                       .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
           .backgroundColor = P->panel2, .cornerRadius = CLAY_CORNER_RADIUS(UISC(5)),
           .border = { .width = CLAY_BORDER_OUTSIDE(1), .color = P->border } }) {
        ui_icon(ICON_SEARCH, 14, P->faint);
        CLAY_TEXT(hint, CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_SMALL), .textColor = P->faint,
                                           .wrapMode = CLAY_TEXT_WRAP_NONE }));
    }
}

static Clay_String tt_rel_word(int reliable){
    return reliable ? CLAY_STRING("RELIABLE") : CLAY_STRING("BEST_EFFORT");
}

/* ============================================================= left: topic tree */

/* the tree is a table: the name column (indented) plus fixed stat columns. These are the
   stat-column widths (unscaled px); the name column measures to fit its content. */
#define TT_COL_DOT    14
#define TT_COL_RATE   58
#define TT_COL_JITTER 64
#define TT_COL_LAST   66
#define TT_COL_QOS    42
#define TT_COL_GAP    6

/* mono-small text width in physical px, for fitting the name column to its content */
static float tt_name_w(const char *s){
    TTF_Font *f = g_fonts[ui_font_id(FAM_MONO, WT_REG, FS_SMALL)];
    int w = 0, h = 0;
    if (f && s && *s) TTF_GetStringSize(f, s, strlen(s), &w, &h);
    return (float)w;
}

/* publish rate for the tree's Rate column; dash-ish handling is done by the caller */
static Clay_String tt_rate(double hz){
    if (hz <= 0.0)   return ui_str(ND_DASH);
    if (hz < 10.0)   return ui_fmt("%.1f Hz", hz);
    if (hz < 1000.0) return ui_fmt("%.0f Hz", hz);
    return ui_fmt("%.1fk Hz", hz / 1000.0);
}
/* p90 jitter for the tree's Jitter column; ms is < 0 for "not enough samples yet" */
static Clay_String tt_jitter(double ms){
    if (ms < 0.0)    return ui_str(ND_DASH);
    if (ms < 10.0)   return ui_fmt("%.2f ms", ms);
    if (ms < 1000.0) return ui_fmt("%.1f ms", ms);
    return ui_fmt("%.2f s", ms / 1000.0);
}
static Clay_String tt_qos_short(int reliable){ return reliable ? CLAY_STRING("REL") : CLAY_STRING("BE"); }

/* one fixed-width stat cell (caption mono, left-aligned) shared by the rows and header */
static void tt_stat_cell(const Palette *P, float w, Clay_String value, Clay_Color col){
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_FIXED(w), .height = CLAY_SIZING_GROW(0) },
                       .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
        CLAY_TEXT(value, CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_CAPTION),
                                            .textColor = col, .wrapMode = CLAY_TEXT_WRAP_NONE }));
    }
}

/* the table header: "TOPIC" then the stat columns, laid out to match the rows so the
   fixed columns (right-anchored by a grow spacer) line up under their labels */
static void tt_tree_header(const Palette *P){
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(UISC(24)) },
                       .padding = { .left = UISCI(12), .right = UISCI(10) }, .childGap = UISCI(TT_COL_GAP),
                       .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
           .border = { .width = { 0, 0, UISCI(1), 0, 0 }, .color = P->border } }) {
        CLAY_TEXT(CLAY_STRING("TOPIC"), CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_CAPTION),
                                          .textColor = P->faint, .letterSpacing = 1, .wrapMode = CLAY_TEXT_WRAP_NONE }));
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}   /* spacer */
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_FIXED(UISC(TT_COL_DOT)) } } }) {}
        tt_stat_cell(P, UISC(TT_COL_RATE),   CLAY_STRING("RATE"),   P->faint);
        tt_stat_cell(P, UISC(TT_COL_JITTER), CLAY_STRING("JITTER"), P->faint);
        tt_stat_cell(P, UISC(TT_COL_LAST),   CLAY_STRING("LAST"),   P->faint);
        tt_stat_cell(P, UISC(TT_COL_QOS),    CLAY_STRING("QOS"),    P->faint);
    }
}

static void topic_tree_row(AppState *app, const Palette *P, const TreeRow *row, int idx){
    const Dataset *D = app->data;
    int sel  = row->has_topic && app->sel_topic >= 0 && app->sel_topic < D->n_topics
               && row->topic == &D->topics[app->sel_topic];
    int live = row->has_topic && row->topic->sub_state != 0;         /* rate/last need the data plane */
    int qos  = row->has_topic && row->topic->has_qos;                /* reliability rides discovery */
    CLAY({ .id = CLAY_IDI("tree_row", (uint32_t)idx),
           .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(UISC(29)) },
                       .padding = { .right = UISCI(10) }, .childGap = UISCI(TT_COL_GAP),
                       .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
           .backgroundColor = sel ? P->accent_bg : UI_NONE,
           .border = { .width = { sel ? UISCI(2) : 0, 0, 0, 0, 0 }, .color = P->accent } }) {
        /* caret zone: spans the indent + caret. A click here toggles collapse and is
           consumed, so it doesn't also select the row below. The indent is left padding,
           so the chevron stays centered after it. */
        CLAY({ .id = CLAY_IDI("tree_caret", (uint32_t)idx),
               .layout = { .sizing = { .width = CLAY_SIZING_FIXED(UISC(10 + row->depth * 15 + 24)), .height = CLAY_SIZING_GROW(0) },
                           .padding = { .left = UISCI(10 + row->depth * 15) },
                           .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } } }) {
            if (row->is_branch){
                if (Clay_Hovered() && g_pointer_pressed){ app_toggle_collapsed(app, row->path); g_pointer_pressed = false; }
                ui_icon(row->open ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, 12, P->dim);
            }
        }
        /* name: a branch (has children) gets a trailing '/' so a namespace reads as a
           path segment; dim when it's a pure namespace, normal when it's a topic */
        CLAY({ .id = CLAY_IDI("tree_name", (uint32_t)idx),
               .layout = { .sizing = { .height = CLAY_SIZING_GROW(0) },
                           .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
            CLAY_TEXT(ui_fmt("%s%s", row->name, row->is_branch ? "/" : ""),
                      CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_SMALL),
                                         .textColor = row->has_topic ? P->text : P->dim,
                                         .wrapMode = CLAY_TEXT_WRAP_NONE }));
        }
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}   /* spacer: right-anchor the stat columns */
        /* status dot: live subscription state (green subscribed, red dropping, grey not) */
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_FIXED(UISC(TT_COL_DOT)) },
                           .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } } }) {
            if (row->has_topic) tt_status_dot(P, row->topic->sub_state);
        }
        /* Rate / Jitter / Last: ride the data plane, so real only while subscribed (dash
           otherwise); QoS rides discovery, so it shows whenever the topic has any endpoint. */
        tt_stat_cell(P, UISC(TT_COL_RATE), live ? tt_rate(row->topic->rate_hz) : ui_str(ND_DASH),
                     live && row->topic->rate_hz > 0.0 ? P->dim : P->faint);
        tt_stat_cell(P, UISC(TT_COL_JITTER), live ? tt_jitter(row->topic->jitter_p90_ms) : ui_str(ND_DASH),
                     live && row->topic->jitter_p90_ms >= 0.0 ? P->dim : P->faint);
        tt_stat_cell(P, UISC(TT_COL_LAST), live ? nf_age(row->topic->last_age_s) : ui_str(ND_DASH),
                     live && row->topic->last_age_s >= 0.0 ? P->dim : P->faint);
        tt_stat_cell(P, UISC(TT_COL_QOS),  qos ? tt_qos_short(row->topic->reliable) : ui_str(ND_DASH),
                     qos ? ui_qos_color(P, row->topic->reliable) : P->faint);
        /* whole-row click selects the topic (the caret already consumed its own click);
           a pure namespace has nothing to select, so a click toggles its collapse */
        if (Clay_Hovered() && g_pointer_pressed){
            if (row->has_topic){
                app->sel_topic = (int)(row->topic - D->topics);
                app->drawer_open = 1;
                app->has_inspect_msg = 0;  /* clicking a topic returns to inspecting the topic */
                app->adding_topic = 0;     /* selecting a topic exits the new-topic field */
            } else if (row->is_branch){
                app_toggle_collapsed(app, row->path);
            }
        }
    }
}

/* commit the typed new-topic name: register it (so it shows in the tree) and queue it for
   selection once it appears, then leave add mode. */
static void tt_commit_new_topic(AppState *app){
    if (app->new_topic_len > 0){
        ui_data_add_user_topic(app->new_topic);
        if (app->cap) cap_declare_publish(app->cap, app->new_topic);   /* advertise pub interest now */
        snprintf(app->select_topic, sizeof app->select_topic, "%s", app->new_topic);
    }
    app->adding_topic  = 0;
    app->new_topic_len = 0;
    app->new_topic[0]  = '\0';
}

/* the inline "new topic name" field, shown under the filter while adding_topic. Text is
   captured into app->new_topic by the event loop; Enter (app->new_topic_commit) or Add commits. */
static void tt_new_topic_input(AppState *app, const Palette *P){
    int can;
    if (app->new_topic_commit){ app->new_topic_commit = 0; tt_commit_new_topic(app); }
    if (!app->adding_topic) return;
    can = app->new_topic_len > 0;
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .childGap = UISCI(6),
                       .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(UISC(30)) },
                           .padding = { .left = UISCI(9), .right = UISCI(9) },
                           .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
               .backgroundColor = P->panel2, .cornerRadius = CLAY_CORNER_RADIUS(UISC(5)),
               .border = { .width = CLAY_BORDER_OUTSIDE(1), .color = P->accent } }) {
            /* always focused while shown: a blinking caret (mono "|"/" ", stable width), no placeholder */
            CLAY_TEXT(ui_fmt("%s%s", app->new_topic, g_caret_on ? "|" : " "),
                      CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_SMALL), .textColor = P->text,
                                         .wrapMode = CLAY_TEXT_WRAP_NONE }));
        }
        if (ui_pill(P, CLAY_STRING("Add"), FAM_SANS, WT_SEMI, FS_SMALL,
                    can ? P->accent : P->faint, can ? P->accent_bg : P->panel2, UI_NONE, UISC(30)) && can)
            tt_commit_new_topic(app);
    }
}

static void topics_tree(AppState *app, const Palette *P){
    const Dataset *D = app->data;
    int n = ui_tree_build(app), i;
    /* fit the panel to its content: widest name column (indent + measured text) plus the
       fixed stat columns, clamped so one long name can't blow it out and the header
       controls stay usable */
    float gap        = UISC(TT_COL_GAP);
    float data_block = UISC(TT_COL_DOT) + UISC(TT_COL_RATE) + UISC(TT_COL_JITTER) + UISC(TT_COL_LAST)
                      + UISC(TT_COL_QOS) + gap * 5;
    float name_max   = UISC(70);
    float panel_w;
    for (i = 0; i < n; i++){
        const TreeRow *r = &ut_rows[i];
        char  lbl[CAP_TOPIC_CAP + 2];
        float w;
        snprintf(lbl, sizeof lbl, "%s%s", r->name, r->is_branch ? "/" : "");
        w = UISC(10 + r->depth * 15 + 24) + gap + tt_name_w(lbl);
        if (w > name_max) name_max = w;
    }
    if (name_max > UISC(300)) name_max = UISC(300);
    panel_w = name_max + gap + data_block + UISC(10) + UISC(16);   /* right pad + breathing room */
    if (panel_w < UISC(300)) panel_w = UISC(300);                  /* keep filter + add usable */
    CLAY({ .id = CLAY_ID("topics_tree"),
           .layout = { .sizing = { .width = CLAY_SIZING_FIXED(panel_w), .height = CLAY_SIZING_GROW(0) },
                       .layoutDirection = CLAY_TOP_TO_BOTTOM },
           .backgroundColor = P->panel,
           .border = { .width = { 0, UISCI(1), 0, 0, 0 }, .color = P->border } }) {
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .padding = CLAY_PADDING_ALL(UISC(12)),
                           .layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = UISCI(8) } }) {
            /* filter + add-topic button */
            CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .childGap = UISCI(8),
                               .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
                ui_filter_box(P, CLAY_STRING("Filter topics..."));
                if (ui_icon_button(P, ICON_PLUS, 16, 30, app->adding_topic ? P->accent : P->dim, P->text)){
                    app->adding_topic = !app->adding_topic;
                    app->new_topic_len = 0; app->new_topic[0] = '\0';
                }
            }
            tt_new_topic_input(app, P);
        }
        if (n > 0) tt_tree_header(P);
        CLAY({ .id = CLAY_ID("topics_tree_scroll"),
               .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0) },
                           .layoutDirection = CLAY_TOP_TO_BOTTOM },
               .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() } }) {
            if (n == 0)
                ui_placeholder(P, CLAY_STRING("no topics discovered"));
            for (i = 0; i < n; i++) topic_tree_row(app, P, &ut_rows[i], i);
        }
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .padding = CLAY_PADDING_ALL(UISC(12)) },
               .border = { .width = { 0, 0, UISCI(1), 0, 0 }, .color = P->border } }) {
            ui_section_label(P, ui_fmt("%d TOPICS", D ? D->n_topics : 0));
        }
    }
}

/* ============================================================ center: feed (table)

   The feed is a TABLE: columns are the message's fields (flattened, so a nested member reads
   pos.x), rows are samples (newest at the bottom). The header is sticky: it is a sibling
   ABOVE the scroll body sharing the same column widths, so the columns line up while the body
   scrolls. The first TT_TABLE_COLS fields show by default; right-click the header to pick which
   fields are shown (with none selected only the time column shows). A schema-less topic falls
   back to a single "payload" column. The shown field columns stretch to fill the table width
   (measured from the previous frame's laid-out body). Clicking a row copies that sample into
   the inspector (the full decode). Cell text is truncated to its (now dynamic) column width: a
   per-cell clip would overflow Clay's 10-slot scroll-container array, and the full value is
   always one click away in the inspector. Any header overrun on a narrow window is hidden by
   the (later-drawn, opaque) drawer or clipped at the window edge. */

#define TT_TABLE_COLS 6    /* fields shown by default (right-click the header to change) */
#define TT_TIME_W    60    /* time column width, unscaled px */
#define TT_FIELD_W   94    /* field column width, unscaled px (fallback before layout is known) */
#define TT_CELL_PADL  8    /* cell left padding, unscaled px */
#define TT_DRAWER_W 332    /* right sidebar width, unscaled px (must match topics_drawer) */
#define TT_FEED_PAD  16    /* topics_feed content padding, unscaled px */

typedef struct { char name[80]; int fidx; } TblCol;   /* a flattened column -> its fields[] index */
static TblCol tt_cols[CAP_MSG_FIELDS];   /* every column (drives the right-click menu) */
static TblCol tt_vis[CAP_MSG_FIELDS];    /* the visible subset, in column order (drives the table) */

/* build the flattened column list from a template decoded message: each non-struct field
   becomes a column named by its dotted path (pos.x), keyed to its fields[] index (stable
   across samples of one schema, since every message enumerates the schema in the same
   depth-first order). A struct field contributes only its name as a path prefix. */
static int tt_build_columns(const CapFeedItem *m, TblCol *cols, int maxcols){
    static char stack[CAP_MSG_FIELDS][CAP_TOPIC_CAP];   /* ancestor field names, by depth */
    int i, n = 0;
    for (i = 0; i < m->n_fields; i++){
        const CapMsgField *f = &m->fields[i];
        int d = f->depth < CAP_MSG_FIELDS ? f->depth : CAP_MSG_FIELDS - 1;
        snprintf(stack[d], sizeof stack[d], "%s", f->name);
        if (!strcmp(f->value, "{...}")) continue;       /* struct: a path prefix, not a column */
        if (n < maxcols){
            char *dst = cols[n].name; int cap = (int)sizeof cols[n].name, at = 0, k;
            for (k = 0; k <= d; k++){
                int w = snprintf(dst + at, (size_t)(cap - at), k ? ".%s" : "%s", stack[k]);
                if (w < 0 || at + w >= cap){ at = cap - 1; break; }
                at += w;
            }
            cols[n].fidx = i;
            n++;
        }
    }
    return n;
}

/* a string truncated to `budget` characters, always copied into the frame pool (so callers
   may pass a local buffer). Keeps a fixed-width cell's text from overrunning into its neighbour. */
static Clay_String tt_fit(const char *s, int budget){
    int len = (int)strlen(s);
    if (budget < 1) budget = 1;
    if (len > budget) len = budget;
    return ui_fmt("%.*s", len, s);
}

/* one fixed-width table cell (mono small, left aligned, truncated to fit) */
static void tt_cell(const Palette *P, float w, const char *text, int budget, Clay_Color col){
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_FIXED(w), .height = CLAY_SIZING_GROW(0) },
                       .padding = { .left = UISCI(TT_CELL_PADL), .right = UISCI(2) },
                       .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
        CLAY_TEXT(tt_fit(text, budget), CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_SMALL),
                                                          .textColor = col, .wrapMode = CLAY_TEXT_WRAP_NONE }));
    }
}

/* the schema-less fallback's single grow-width payload cell (overrun is clipped by the feed
   scissor / hidden by the drawer, so it needs no per-cell clip) */
static void tt_cell_grow(const Palette *P, const char *text, Clay_Color col){
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0) },
                       .padding = { .left = UISCI(TT_CELL_PADL), .right = UISCI(2) },
                       .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
        CLAY_TEXT(ui_fmt("%s", text), CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_SMALL),
                                                        .textColor = col, .wrapMode = CLAY_TEXT_WRAP_NONE }));
    }
}

/* the sticky header row: a fixed time column then a header cell per shown field. `field_w`
   is the stretched column width so the fields fill the table. `raw` (a schema-less topic)
   shows a single payload column instead; with a schema but no fields selected only the time
   column shows. Right-clicking the row opens the column-selection menu at the cursor. */
static void tt_table_header(AppState *app, const Palette *P, const TblCol *cols, int n_show,
                            float field_w, int field_budget, int time_budget, int raw){
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(UISC(26)) } },
           .backgroundColor = P->panel2,
           .border = { .width = { 0, 0, 0, UISCI(1), 0 }, .color = P->border2 } }) {
        int c;
        if (Clay_Hovered() && g_right_pressed){
            app->col_menu_open = 1;
            app->col_menu_x = g_pointer_x; app->col_menu_y = g_pointer_y;
        }
        tt_cell(P, UISC(TT_TIME_W), "t (s)", time_budget, P->faint);
        if (raw) tt_cell_grow(P, "payload", P->faint);
        else for (c = 0; c < n_show; c++) tt_cell(P, field_w, cols[c].name, field_budget, P->faint);
    }
}

/* one sample row: the time column then a cell per shown field (its decoded value, or "-"
   when a message lacks it), field cells stretched to `field_w`. `raw` shows the payload
   instead. Clicking copies the sample into the inspector; the inspected sample is highlighted. */
static void tt_table_row(AppState *app, const Palette *P, const char *topic, const CapFeedItem *m,
                         int idx, const TblCol *cols, int n_show, float field_w,
                         int field_budget, int time_budget, int raw){
    int  selected = app->has_inspect_msg && m->uid == app->inspect_msg.uid
                    && !strcmp(topic, app->inspect_msg_topic);
    char tbuf[16];
    int  c;
    snprintf(tbuf, sizeof tbuf, "%.2f", m->t_s);
    CLAY({ .id = CLAY_IDI("feed_row", (uint32_t)idx),
           .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(UISC(24)) } },
           .backgroundColor = selected ? P->accent_bg : (Clay_Hovered() ? P->panel : UI_NONE),
           .border = { .width = { 0, 0, 0, UISCI(1), 0 }, .color = P->border } }) {
        if (Clay_Hovered() && g_pointer_pressed) app_inspect_msg(app, topic, m);
        tt_cell(P, UISC(TT_TIME_W), tbuf, time_budget, P->faint);
        if (raw){
            char rawbuf[CAP_MSG_PREVIEW + 1];
            const char *pv = m->preview;
            if (!m->decoded){ tt_sanitize(rawbuf, (int)sizeof rawbuf, m->preview, m->preview_len); pv = rawbuf; }
            tt_cell_grow(P, pv, P->text);
        } else {
            for (c = 0; c < n_show; c++){
                const char *v = (m->decoded && cols[c].fidx < m->n_fields) ? m->fields[cols[c].fidx].value : "-";
                tt_cell(P, field_w, v, field_budget, P->text);
            }
        }
    }
}

/* one column-menu row: a checkbox + the field's flattened name; click toggles its visibility */
static void tt_col_menu_item(AppState *app, const Palette *P, const TblCol *col, int idx){
    int on = app_col_visible(app, col->name);
    CLAY({ .id = CLAY_IDI("col_menu_item", (uint32_t)idx),
           .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(UISC(26)) },
                       .padding = { .left = UISCI(7), .right = UISCI(9) }, .childGap = UISCI(8),
                       .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
           .backgroundColor = Clay_Hovered() ? P->panel2 : UI_NONE,
           .cornerRadius = CLAY_CORNER_RADIUS(UISC(4)) }) {
        if (Clay_Hovered() && g_pointer_pressed) app_col_toggle(app, col->name);
        /* checkbox: accent box + check when shown, empty bordered box when hidden */
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_FIXED(UISC(15)), .height = CLAY_SIZING_FIXED(UISC(15)) },
                           .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } },
               .backgroundColor = on ? P->accent_bg : UI_NONE,
               .cornerRadius = CLAY_CORNER_RADIUS(UISC(3)),
               .border = { .width = CLAY_BORDER_OUTSIDE(1), .color = on ? P->accent : P->border2 } }) {
            if (on) ui_icon(ICON_CHECK, 11, P->accent);
        }
        CLAY_TEXT(ui_str(col->name), CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_SMALL),
                                                        .textColor = on ? P->text : P->dim,
                                                        .wrapMode = CLAY_TEXT_WRAP_NONE }));
    }
}

/* the header's right-click column menu: a floating checklist of every field over a full-screen
   backdrop that dismisses it on an outside click. Anchored at the cursor, clamped to the window. */
static void tt_col_menu(AppState *app, const Palette *P, const TblCol *cols, int n_cols){
    float menu_w = UISC(210);
    float menu_h = UISC(30) + UISC(26) * (float)(n_cols > 0 ? n_cols : 1) + UISC(10);
    float x, y;
    int c;
    if (!app->col_menu_open) return;
    /* backdrop under the menu: an outside click (either button) closes it */
    CLAY({ .id = CLAY_ID("col_menu_backdrop"),
           .floating = { .attachTo = CLAY_ATTACH_TO_ROOT, .zIndex = 200 },
           .layout = { .sizing = { .width = CLAY_SIZING_FIXED(g_view_w), .height = CLAY_SIZING_FIXED(g_view_h) } } }) {
        if (Clay_Hovered() && (g_pointer_pressed || g_right_pressed)) app->col_menu_open = 0;
    }
    x = app->col_menu_x; y = app->col_menu_y;
    if (x + menu_w > g_view_w) x = g_view_w - menu_w;
    if (y + menu_h > g_view_h) y = g_view_h - menu_h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    CLAY({ .id = CLAY_ID("col_menu"),
           .floating = { .attachTo = CLAY_ATTACH_TO_ROOT, .offset = { x, y }, .zIndex = 201 },
           .layout = { .sizing = { .width = CLAY_SIZING_FIXED(menu_w) }, .layoutDirection = CLAY_TOP_TO_BOTTOM,
                       .padding = CLAY_PADDING_ALL(UISC(5)), .childGap = UISCI(1) },
           .backgroundColor = P->panel3, .cornerRadius = CLAY_CORNER_RADIUS(UISC(7)),
           .border = { .width = CLAY_BORDER_OUTSIDE(1), .color = P->border2 } }) {
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) },
                           .padding = { .left = UISCI(7), .top = UISCI(4), .bottom = UISCI(5) } } }) {
            ui_section_label(P, CLAY_STRING("COLUMNS"));
        }
        if (n_cols == 0)
            CLAY({ .layout = { .padding = { .left = UISCI(7), .bottom = UISCI(4) } } }) {
                CLAY_TEXT(CLAY_STRING("no fields"), CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_CAPTION),
                                                                      .textColor = P->faint }));
            }
        for (c = 0; c < n_cols; c++) tt_col_menu_item(app, P, &cols[c], c);
    }
}

/* publish the composed message to `topic` and clear the box */
static void tt_do_send(AppState *app, const char *topic){
    if (app->compose_len <= 0 || !app->cap || !topic) return;
    cap_publish(app->cap, topic, app->compose, (size_t)app->compose_len);
    app->compose_len = 0;
    app->compose[0]  = '\0';
}

/* ------------------------------------------------- structured publish form (typed topics) */

/* the prefilled default for a form field, by its kind */
static const char *tt_form_default(const CapSchemaField *f){
    switch (f->kind){
        case CAP_K_BOOL:                 return "false";
        case CAP_K_STRUCT:               /* no setter yet: stays default */
        case CAP_K_STR:                  /* empty string(s) */
        case CAP_K_ARR:  return "";      /* array: empty = all zero */
        default:         return "0";     /* numeric scalar */
    }
}

/* overwrite a form field's value (bool toggle, quick fills) */
static void tt_form_set_val(AppState *app, int i, const char *v){
    snprintf(app->form_val[i], UI_FORM_VAL, "%s", v);
    app->form_len[i] = (int)strlen(app->form_val[i]);
}

/* live element count of an array value, for the "n/count" hint. Whitespace/comma runs for
   numeric elements; comma-separated segments for strings (which may contain spaces). This
   is only a display cue; cap_form_validate is the authoritative accept test. */
static int tt_count_elems(const CapSchemaField *f, const char *s){
    int n = 0;
    if (f->elem == CAP_K_STR){
        const char *p = s;
        while (*p){
            const char *q = p; while (*q && *q != ',') q++;
            n++;
            if (!*q) break;
            p = q + 1;
        }
    } else {
        int in = 0;
        for (; *s; s++){
            int sep = (*s == ' ' || *s == '\t' || *s == ',');
            if (!sep && !in){ n++; in = 1; } else if (sep) in = 0;
        }
    }
    return n;
}

/* a faint placeholder for an empty, unfocused text field, hinting the expected shape */
static Clay_String tt_form_hint(const CapSchemaField *f){
    if (f->kind == CAP_K_STR) return CLAY_STRING("text");
    if (f->kind == CAP_K_ARR) return CLAY_STRING("comma-separated");
    return CLAY_STRING("0");
}

/* one segment of the boolean toggle: click sets the field to `seg` ("true"/"false") */
static void tt_form_bool_seg(AppState *app, const Palette *P, int i, const char *seg, int active){
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0) },
                       .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } },
           .backgroundColor = active ? P->accent_bg : UI_NONE,
           .cornerRadius = CLAY_CORNER_RADIUS(UISC(3)) }) {
        if (Clay_Hovered() && g_pointer_pressed){ tt_form_set_val(app, i, seg); app->form_focus = i; app->adding_topic = 0; }
        CLAY_TEXT(ui_str(seg), CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_SMALL),
                                                  .textColor = active ? P->accent : P->dim,
                                                  .wrapMode = CLAY_TEXT_WRAP_NONE }));
    }
}

/* a boolean field's smart input: a two-segment [ false | true ] control filling the value
   column, the current value highlighted. Click a segment to set it (no typing needed). */
static void tt_form_bool(AppState *app, const Palette *P, int i, int focused){
    int on = !strcmp(app->form_val[i], "true");
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(UISC(24)) },
                       .padding = CLAY_PADDING_ALL(UISC(2)), .childGap = UISCI(2) },
           .backgroundColor = P->panel2, .cornerRadius = CLAY_CORNER_RADIUS(UISC(4)),
           .border = { .width = CLAY_BORDER_OUTSIDE(1), .color = focused ? P->accent : P->border } }) {
        tt_form_bool_seg(app, P, i, "false", !on);
        tt_form_bool_seg(app, P, i, "true",   on);
    }
}

static void tt_form_reset(AppState *app, const CapSchema *sc, int n){
    int i;
    for (i = 0; i < n; i++){
        snprintf(app->form_val[i], UI_FORM_VAL, "%s", tt_form_default(&sc->fields[i]));
        app->form_len[i] = (int)strlen(app->form_val[i]);
    }
    app->form_focus = 0;
}

static void tt_form_send(AppState *app, const Topic *t, int n){
    const char *vals[UI_FORM_MAX]; int i;
    if (!app->cap) return;
    for (i = 0; i < n; i++) vals[i] = app->form_val[i];
    cap_publish_form(app->cap, t->path, vals, n);   /* the values stay for the next send */
}

/* one field: name | type-aware input | type, nested members indented. A bool gets a
   toggle, a string/array gets a validated box with an "n/cap" counter, a numeric scalar a
   validated box; the input box + type turn red when the typed value won't parse (`valid`
   comes from cap_form_validate). A struct row is a read-only group header (members are the
   inputs). Clicking a text field focuses it. */
static void tt_form_field_row(AppState *app, const Palette *P, const CapSchemaField *f, int i, int valid){
    int is_struct = (f->kind == CAP_K_STRUCT);
    int is_bool   = (f->kind == CAP_K_BOOL);
    int focused   = !is_struct && app->form_focus == i && !app->adding_topic;
    int has_val   = app->form_val[i][0] != '\0';
    int bad       = !is_struct && !is_bool && has_val && !valid;   /* typed, but won't parse */
    int show_count = (f->kind == CAP_K_STR || f->kind == CAP_K_ARR);
    CLAY({ .id = CLAY_IDI("form_field", (uint32_t)i),
           .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(UISC(26)) },
                       .padding = { .left = UISCI(f->depth * 14) },
                       .childGap = UISCI(8), .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_FIXED(UISC(110 - f->depth * 14)), .height = CLAY_SIZING_GROW(0) },
                           .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
            CLAY_TEXT(ui_str(f->name), CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_CAPTION),
                                                          .textColor = P->dim, .wrapMode = CLAY_TEXT_WRAP_NONE }));
        }
        if (is_bool){
            tt_form_bool(app, P, i, focused);
        } else if (!is_struct){
            CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(UISC(24)) },
                               .padding = { .left = UISCI(8), .right = UISCI(8) },
                               .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
                   .backgroundColor = P->panel2,
                   .cornerRadius = CLAY_CORNER_RADIUS(UISC(4)),
                   .border = { .width = CLAY_BORDER_OUTSIDE(1),
                               .color = bad ? P->red : focused ? P->accent : P->border } }) {
                if (Clay_Hovered() && g_pointer_pressed){ app->form_focus = i; app->adding_topic = 0; }
                if (!has_val && !focused)
                    CLAY_TEXT(tt_form_hint(f), CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_SMALL),
                                                                 .textColor = P->faint, .wrapMode = CLAY_TEXT_WRAP_NONE }));
                else
                    CLAY_TEXT(ui_fmt("%s%s", app->form_val[i], focused ? (g_caret_on ? "|" : " ") : ""),
                              CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_SMALL),
                                                 .textColor = P->text, .wrapMode = CLAY_TEXT_WRAP_NONE }));
            }
        } else {
            CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
        }
        if (show_count){    /* live shape cue: bytes/cap for a string, elements/count for an array */
            int cur = f->kind == CAP_K_STR ? (int)strlen(app->form_val[i]) : tt_count_elems(f, app->form_val[i]);
            int cap = f->kind == CAP_K_STR ? (int)f->str_cap : (int)f->count;
            int over = cur > cap;
            CLAY_TEXT(ui_fmt("%d/%d", cur, cap), CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_CAPTION),
                                          .textColor = (bad || over) ? P->red : P->faint, .wrapMode = CLAY_TEXT_WRAP_NONE }));
        }
        CLAY_TEXT(ui_str(f->type), CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_CAPTION),
                                                      .textColor = bad ? P->red : is_struct ? P->dim : P->accent,
                                                      .wrapMode = CLAY_TEXT_WRAP_NONE }));
    }
}

/* the structured composer: the message type name, one input per schema field (nested members
   indented) prefilled with defaults, then a Send button under them. Enter or Send publishes
   (empty fields keep the canonical default zero). */
static void topics_form(AppState *app, const Palette *P, const Topic *t, const CapSchema *sc){
    int i, n = sc->n_fields < UI_FORM_MAX ? sc->n_fields : UI_FORM_MAX;
    int send = app->form_send;
    unsigned char valid[UI_FORM_MAX];
    app->form_send = 0; app->compose_send = 0;   /* the form owns Enter on this topic */
    if (app->form_topic != app->sel_topic){       /* switched topic: fresh defaults */
        app->form_topic = app->sel_topic;
        tt_form_reset(app, sc, n);
    }
    app->form_n = n;                              /* input routes to the form fields */
    for (i = 0; i < n; i++) app->form_kind[i] = sc->fields[i].kind;   /* event loop: bool = toggle keys */
    {   const char *vals[UI_FORM_MAX];            /* live per-field validity (one schema parse) */
        for (i = 0; i < n; i++){ vals[i] = app->form_val[i]; valid[i] = 1; }
        if (app->cap) cap_form_validate(app->cap, t->path, vals, n, valid);
    }
    if (send) tt_form_send(app, t, n);
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .layoutDirection = CLAY_TOP_TO_BOTTOM,
                       .childGap = UISCI(6) } }) {
        /* header: message type name + input hint */
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .childGap = UISCI(8),
                           .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
            CLAY_TEXT(ui_str(sc->type_name), CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_SEMI, FS_SMALL),
                                                               .textColor = P->text, .wrapMode = CLAY_TEXT_WRAP_NONE }));
            CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
            CLAY_TEXT(CLAY_STRING("Tab: next \xC2\xB7 Enter: send"),
                      CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_CAPTION), .textColor = P->faint,
                                         .wrapMode = CLAY_TEXT_WRAP_NONE }));
        }
        for (i = 0; i < n; i++) tt_form_field_row(app, P, &sc->fields[i], i, valid[i]);
        /* Send: its own row under the fields (right-aligned), not boxed in a card */
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .padding = { .top = UISCI(4) } } }) {
            CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
            if (ui_pill(P, CLAY_STRING("Send"), FAM_SANS, WT_SEMI, FS_SMALL,
                        P->accent, P->accent_bg, P->border2, UISC(34)))
                tt_form_send(app, t, n);
        }
    }
}

/* The message composer, hosted in the Publish sidebar tab: an input box that grows in height
   with the wrapped text plus a Send button. Enter (app->compose_send, set in the event loop)
   sends too. Text is captured into app->compose by the SDL text-input handler while the
   Publish tab is open. Room for QoS/options beside Send comes later. */
static void topics_composer(AppState *app, const Palette *P, const Topic *t){
    static CapSchema tt_form_schema;   /* the selected topic's advertised schema, per frame */
    int can, focused, send;
    if (app->cap && cap_topic_schema(app->cap, t->path, &tt_form_schema)
        && tt_form_schema.inlined && tt_form_schema.n_fields > 0){
        topics_form(app, P, t, &tt_form_schema);   /* typed topic: the structured form */
        return;
    }
    app->form_focus = -1; app->form_n = 0;         /* free-text composer owns input */
    send = app->compose_send;
    app->compose_send = 0;
    if (app->compose_topic != app->sel_topic){      /* switched topic: drop the stale draft */
        app->compose_topic = app->sel_topic;
        app->compose_len = 0; app->compose[0] = '\0';
    }
    /* the composer is focused by default (so you can just type); the new-topic field steals
       focus while it is open. The blinking caret marks where text goes. */
    focused = !app->adding_topic;
    can = app->compose_len > 0 && app->cap != NULL;
    if (send) tt_do_send(app, t->path);

    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .childGap = UISCI(10),
                       .childAlignment = { .y = CLAY_ALIGN_Y_BOTTOM } } }) {
        /* input box: FIT height grows with the text, min one line, capped before it scrolls */
        CLAY({ .id = CLAY_ID("compose_box"),
               .layout = { .sizing = { .width = CLAY_SIZING_GROW(0),
                                       .height = CLAY_SIZING_FIT(UISC(38), UISC(150)) },
                           .padding = { .left = UISCI(11), .right = UISCI(11),
                                        .top = UISCI(10), .bottom = UISCI(10) },
                           .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
               .backgroundColor = P->panel2, .cornerRadius = CLAY_CORNER_RADIUS(UISC(6)),
               .border = { .width = CLAY_BORDER_OUTSIDE(1), .color = focused ? P->accent : P->border } }) {
            if (Clay_Hovered() && g_pointer_pressed) app->adding_topic = 0;   /* click returns focus here */
            if (app->compose_len == 0 && !focused)
                CLAY_TEXT(CLAY_STRING("Type a message, Enter to send"),
                          CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_SMALL), .textColor = P->faint,
                                             .wrapMode = CLAY_TEXT_WRAP_NONE }));
            else
                /* mono caret: "|" on / " " off keeps the same advance + line height every frame,
                   so the blink never resizes the box (a space and a bar are one cell each) */
                CLAY_TEXT(ui_fmt("%s%s", app->compose, !focused ? "" : (g_caret_on ? "|" : " ")),
                          CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_SMALL), .textColor = P->text,
                                             .wrapMode = CLAY_TEXT_WRAP_WORDS }));
        }
        if (ui_pill(P, CLAY_STRING("Send"), FAM_SANS, WT_SEMI, FS_SMALL,
                    can ? P->accent : P->faint, can ? P->accent_bg : P->panel2, P->border2, UISC(38)) && can)
            tt_do_send(app, t->path);
    }
}

static void topics_feed(AppState *app, const Palette *P){
    const Dataset *D = app->data;
    const Topic *t = (D && D->n_topics && app->sel_topic >= 0 && app->sel_topic < D->n_topics)
                     ? &D->topics[app->sel_topic] : NULL;
    int feed_state, i;

    /* refetch the selected topic's live feed for this frame (before drawing the controls) */
    tt_feed_n = 0; tt_subscribed = tt_sub_error = tt_sub_reliable = 0; tt_msgs = tt_drops = 0;
    if (t && app->cap)
        tt_feed_n = cap_topic_feed(app->cap, t->path, tt_feed, CAP_FEED_MAX,
                                   &tt_subscribed, &tt_sub_error, &tt_sub_reliable, &tt_msgs, &tt_drops);
    feed_state = tt_subscribed ? (tt_sub_error ? 2 : 1) : 0;

    CLAY({ .id = CLAY_ID("topics_feed"),
           .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0) },
                       .layoutDirection = CLAY_TOP_TO_BOTTOM, .padding = CLAY_PADDING_ALL(UISC(16)),
                       .childGap = UISCI(12) },
           .backgroundColor = P->bg }) {
        if (!t){
            ui_placeholder(P, CLAY_STRING("select a topic"));
        } else {
            /* header row: path + Details (only when the drawer is closed) */
            CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) },
                               .childGap = UISCI(10), .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
                CLAY_TEXT(ui_str(t->path), CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_TITLE),
                                                             .textColor = P->text, .wrapMode = CLAY_TEXT_WRAP_NONE }));
                CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
                if (!app->drawer_open &&
                    ui_pill(P, CLAY_STRING("Details"), FAM_SANS, WT_SEMI, FS_SMALL,
                            P->dim, P->panel2, P->border2, UISC(28))){
                    app->drawer_open = 1; app->drawer_tab = DRAWER_INSPECT;
                }
            }
            /* meta row: reliability + endpoint counts (all real) */
            CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .childGap = UISCI(16),
                               .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
                CLAY_TEXT(tt_rel_word(t->reliable),
                          CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_CAPTION),
                                             .textColor = ui_qos_color(P, t->reliable), .wrapMode = CLAY_TEXT_WRAP_NONE }));
                CLAY_TEXT(ui_fmt("%d publishers", t->n_pubs + t->self_pub),
                          CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_CAPTION), .textColor = P->dim, .wrapMode = CLAY_TEXT_WRAP_NONE }));
                CLAY_TEXT(ui_fmt("%d subscribers", t->n_subs + t->self_sub),
                          CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_CAPTION), .textColor = P->dim, .wrapMode = CLAY_TEXT_WRAP_NONE }));
            }
            /* control row: subscribe toggle + live subscription status */
            CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .childGap = UISCI(12),
                               .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
                if (!tt_subscribed){
                    if (ui_pill(P, CLAY_STRING("Subscribe"), FAM_SANS, WT_SEMI, FS_SMALL,
                                P->accent, P->accent_bg, UI_NONE, UISC(28)) && app->cap)
                        cap_subscribe(app->cap, t->path, t->reliable_recommend > 0);
                } else {
                    if (ui_pill(P, CLAY_STRING("Unsubscribe"), FAM_SANS, WT_SEMI, FS_SMALL,
                                P->dim, P->panel2, P->border2, UISC(28)) && app->cap)
                        cap_unsubscribe(app->cap, t->path);
                }
                tt_status_dot(P, feed_state);
                CLAY_TEXT(tt_subscribed ? ui_fmt("subscribed %s  \xC2\xB7  %u msgs",
                                                 tt_sub_reliable ? "reliable" : "best-effort", tt_msgs)
                                        : CLAY_STRING("not subscribed"),
                          CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_CAPTION),
                                             .textColor = feed_state == 2 ? P->red : P->dim,
                                             .wrapMode = CLAY_TEXT_WRAP_NONE }));
                if (tt_drops > 0)
                    CLAY_TEXT(ui_fmt("%u dropped", tt_drops),
                              CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_CAPTION), .textColor = P->red,
                                                 .wrapMode = CLAY_TEXT_WRAP_NONE }));
            }
            if (tt_feed_n == 0){
                ui_placeholder(P, tt_subscribed ? CLAY_STRING("waiting for messages...")
                                                : CLAY_STRING("subscribe to see live messages"));
            } else {
                /* columns come from the newest decoded sample (its fields[] index maps 1:1 to
                   every sample of this schema); a schema-less feed shows one payload column */
                int   n_cols = 0, tmpl = -1, j, c, n_vis = 0, raw;
                int   field_budget, time_budget;
                float mono_adv = tt_name_w("00000000") / 8.0f;
                float table_w = 0.0f, field_w;
                if (mono_adv <= 0.5f) mono_adv = UISC(7);
                time_budget = (int)((UISC(TT_TIME_W) - UISC(TT_CELL_PADL + 2)) / mono_adv);
                if (time_budget < 1) time_budget = 1;
                for (j = tt_feed_n - 1; j >= 0; j--) if (tt_feed[j].decoded){ tmpl = j; break; }
                if (tmpl >= 0) n_cols = tt_build_columns(&tt_feed[tmpl], tt_cols, CAP_MSG_FIELDS);

                /* seed the visible set to the first TT_TABLE_COLS fields when the topic changes;
                   thereafter the right-click menu owns it */
                if (app->col_vis_topic != app->sel_topic){
                    app->col_vis_topic = app->sel_topic;
                    app->col_menu_open = 0;
                    app->n_col_vis = 0;
                    for (c = 0; c < n_cols && c < TT_TABLE_COLS; c++) app_col_toggle(app, tt_cols[c].name);
                }
                for (c = 0; c < n_cols; c++)
                    if (app_col_visible(app, tt_cols[c].name)) tt_vis[n_vis++] = tt_cols[c];
                raw = (n_cols == 0);   /* schema-less: one payload column. all fields off: just time */

                /* stretch the field columns to fill the AVAILABLE table width, derived from
                   STABLE references (window - tree - drawer - feed padding) rather than the
                   feed's own laid-out width. Fixed-width cells set the feed's minimum width, so
                   measuring the feed would pin it wide and it could never shrink on a resize
                   (pushing the fixed drawer off-screen) -- a feedback loop this sidesteps. The
                   tree is a fixed-width panel, so its measured width is unaffected by overflow. */
                {   Clay_ElementData td = Clay_GetElementData(Clay_GetElementId(CLAY_STRING("topics_tree")));
                    float drawer_w = app->drawer_open ? UISC(TT_DRAWER_W) : 0.0f;
                    if (td.found)
                        table_w = g_view_w - td.boundingBox.width - drawer_w - UISC(TT_FEED_PAD) * 2.0f - UISC(2);
                }
                field_w = (n_vis > 0 && table_w > UISC(TT_TIME_W))
                          ? floorf((table_w - UISC(TT_TIME_W)) / (float)n_vis) : UISC(TT_FIELD_W);
                if (field_w < 1.0f) field_w = 1.0f;
                field_budget = (int)((field_w - UISC(TT_CELL_PADL + 2)) / mono_adv);
                if (field_budget < 1) field_budget = 1;

                CLAY({ .id = CLAY_ID("topics_table"),
                       .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0) },
                                   .layoutDirection = CLAY_TOP_TO_BOTTOM },
                       .border = { .width = CLAY_BORDER_OUTSIDE(1), .color = P->border } }) {
                    tt_table_header(app, P, tt_vis, n_vis, field_w, field_budget, time_budget, raw);
                    /* scrolling body, newest at the bottom; topics_feed_autoscroll keeps it pinned */
                    CLAY({ .id = CLAY_ID("topics_feed_scroll"),
                           .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0) },
                                       .layoutDirection = CLAY_TOP_TO_BOTTOM },
                           .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() } }) {
                        for (i = 0; i < tt_feed_n; i++)
                            tt_table_row(app, P, t->path, &tt_feed[i], i, tt_vis, n_vis, field_w,
                                         field_budget, time_budget, raw);
                    }
                }
                tt_col_menu(app, P, tt_cols, n_cols);   /* the header's right-click column picker */
            }
        }
    }
}

/* ================================================================ right: drawer */

/* one right-sidebar tab: accent text on an accent-tinted pill when active, dim otherwise */
static void topics_drawer_tab(AppState *app, const Palette *P, Clay_String label, DrawerTab tab){
    int active = app->drawer_tab == tab;
    CLAY({ .layout = { .sizing = { .height = CLAY_SIZING_FIXED(UISC(28)) },
                       .padding = { .left = UISCI(11), .right = UISCI(11) },
                       .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } },
           .backgroundColor = active ? P->accent_bg : UI_NONE,
           .cornerRadius = CLAY_CORNER_RADIUS(UISC(5)) }) {
        if (Clay_Hovered() && g_pointer_pressed) app->drawer_tab = tab;   /* the message view keeps
            its selection across tab switches; the "Topic" back control clears it */
        CLAY_TEXT(label, CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_SEMI, FS_SMALL),
                                            .textColor = active ? P->accent : P->dim,
                                            .wrapMode = CLAY_TEXT_WRAP_NONE }));
    }
}

static void topics_drawer_header(AppState *app, const Palette *P){
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(UISC(40)) },
                       .padding = { .left = UISCI(8), .right = UISCI(6) }, .childGap = UISCI(3),
                       .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
           .border = { .width = { 0, 0, 0, UISCI(1), 0 }, .color = P->border } }) {
        topics_drawer_tab(app, P, CLAY_STRING("Inspect"), DRAWER_INSPECT);
        topics_drawer_tab(app, P, CLAY_STRING("Publish"), DRAWER_PUBLISH);
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
        if (ui_icon_button(P, ICON_X, 14, 26, P->dim, P->text))
            app->drawer_open = 0;
    }
}

static void topic_qos_cell(const Palette *P, Clay_String label, Clay_String value, Clay_Color vcol){
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(UISC(50)) },
                       .layoutDirection = CLAY_TOP_TO_BOTTOM, .padding = CLAY_PADDING_ALL(UISC(9)),
                       .childGap = UISCI(6) },
           .backgroundColor = P->panel2, .cornerRadius = CLAY_CORNER_RADIUS(UISC(6)),
           .border = { .width = CLAY_BORDER_OUTSIDE(1), .color = P->border } }) {
        CLAY_TEXT(label, CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_CAPTION),
                                            .textColor = P->faint, .wrapMode = CLAY_TEXT_WRAP_NONE }));
        CLAY_TEXT(value, CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_SMALL),
                                            .textColor = vcol, .wrapMode = CLAY_TEXT_WRAP_NONE }));
    }
}

/* a publisher/subscriber row: node dot + name + locator; click jumps to the node */
static void topic_node_chip(AppState *app, const Palette *P, int nidx, const char *idtag, int idx){
    const Node *nd = &app->data->nodes[nidx];
    CLAY({ .id = CLAY_SIDI(ui_str(idtag), (uint32_t)idx),
           .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(UISC(30)) },
                       .padding = { .left = UISCI(10), .right = UISCI(10) },
                       .childGap = UISCI(9), .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
           .backgroundColor = P->panel2, .cornerRadius = CLAY_CORNER_RADIUS(UISC(5)),
           .border = { .width = CLAY_BORDER_OUTSIDE(1), .color = P->border } }) {
        if (Clay_Hovered() && g_pointer_pressed){ app->tab = TAB_NODES; app->sel_node = nidx; }
        ui_dot(UISC(7), ui_state_color(P, nd->state), UI_NONE);
        CLAY_TEXT(ui_str(nd->name), CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_SMALL),
                                                       .textColor = P->text, .wrapMode = CLAY_TEXT_WRAP_NONE }));
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
        CLAY_TEXT(ui_str(nd->ip), CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_CAPTION),
                                                     .textColor = P->faint, .wrapMode = CLAY_TEXT_WRAP_NONE }));
    }
}

/* the explorer's own endpoint on a topic (not a discovered peer): accent dot + "you" + the
   reliability we offer/request, so a topic we publish to lists us as a publisher */
static void topic_self_chip(const Palette *P, int reliable){
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(UISC(30)) },
                       .padding = { .left = UISCI(10), .right = UISCI(10) },
                       .childGap = UISCI(9), .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
           .backgroundColor = P->panel2, .cornerRadius = CLAY_CORNER_RADIUS(UISC(5)),
           .border = { .width = CLAY_BORDER_OUTSIDE(1), .color = P->border } }) {
        ui_dot(UISC(7), P->accent, UI_NONE);
        CLAY_TEXT(CLAY_STRING("you"), CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_SMALL),
                                                         .textColor = P->text, .wrapMode = CLAY_TEXT_WRAP_NONE }));
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
        CLAY_TEXT(tt_rel_word(reliable), CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_CAPTION),
                                                           .textColor = ui_qos_color(P, reliable), .wrapMode = CLAY_TEXT_WRAP_NONE }));
    }
}

/* one schema field: name | type | @offset (nested members indented by depth) */
static void topic_schema_field_row(const Palette *P, const CapSchemaField *f, int idx){
    CLAY({ .id = CLAY_IDI("schema_field", (uint32_t)idx),
           .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(UISC(24)) },
                       .padding = { .left = UISCI(10 + f->depth * 12), .right = UISCI(10) },
                       .childGap = UISCI(9), .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
           .border = { .width = { 0, 0, 0, UISCI(1), 0 }, .color = P->border } }) {
        CLAY_TEXT(ui_str(f->name), CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_SMALL),
                                                      .textColor = P->text, .wrapMode = CLAY_TEXT_WRAP_NONE }));
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
        CLAY_TEXT(ui_str(f->type), CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_SMALL),
                                                      .textColor = P->accent, .wrapMode = CLAY_TEXT_WRAP_NONE }));
        CLAY_TEXT(ui_fmt("@%u", f->offset), CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_CAPTION),
                                                               .textColor = P->faint, .wrapMode = CLAY_TEXT_WRAP_NONE }));
    }
}

/* the selected topic's advertised message schema (from any endpoint: pub or sub) */
static CapSchema tt_schema;
static void topic_schema_section(AppState *app, const Palette *P, const Topic *t){
    int have = app->cap ? cap_topic_schema(app->cap, t->path, &tt_schema) : 0;
    int i;
    ui_section_label(P, CLAY_STRING("SCHEMA"));
    if (!have){
        CLAY_TEXT(CLAY_STRING("none advertised (raw bytes)"),
                  CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_SMALL), .textColor = P->faint }));
    } else {
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .layoutDirection = CLAY_TOP_TO_BOTTOM },
               .backgroundColor = P->panel2, .cornerRadius = CLAY_CORNER_RADIUS(UISC(6)),
               .border = { .width = CLAY_BORDER_OUTSIDE(1), .color = P->border } }) {
            /* header: root type name + message size, hash underneath */
            CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .layoutDirection = CLAY_TOP_TO_BOTTOM,
                               .padding = CLAY_PADDING_ALL(UISC(9)), .childGap = UISCI(4) },
                   .border = { .width = { 0, 0, 0, UISCI(1), 0 }, .color = P->border } }) {
                CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .childGap = UISCI(8),
                                   .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
                    CLAY_TEXT(tt_schema.inlined ? ui_str(tt_schema.type_name) : CLAY_STRING("(hash only)"),
                              CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_SEMI, FS_SMALL),
                                                 .textColor = P->text, .wrapMode = CLAY_TEXT_WRAP_NONE }));
                    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
                    if (tt_schema.inlined)
                        CLAY_TEXT(ui_fmt("%u B/msg", tt_schema.msg_size),
                                  CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_CAPTION),
                                                     .textColor = P->dim, .wrapMode = CLAY_TEXT_WRAP_NONE }));
                }
                CLAY_TEXT(ui_fmt("id %08x%08x", (unsigned)(tt_schema.hash >> 32), (unsigned)tt_schema.hash),
                          CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_CAPTION),
                                             .textColor = P->faint, .wrapMode = CLAY_TEXT_WRAP_NONE }));
            }
            if (tt_schema.inlined)
                for (i = 0; i < tt_schema.n_fields; i++) topic_schema_field_row(P, &tt_schema.fields[i], i);
            if (tt_schema.inlined && tt_schema.total_fields > tt_schema.n_fields)
                CLAY_TEXT(ui_fmt("  +%d more fields", tt_schema.total_fields - tt_schema.n_fields),
                          CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_CAPTION), .textColor = P->faint }));
            if (!tt_schema.inlined)
                CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .padding = CLAY_PADDING_ALL(UISC(9)) } }) {
                    CLAY_TEXT(CLAY_STRING("schema advertised by hash only (too large to inline)"),
                              CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_CAPTION), .textColor = P->faint }));
                }
        }
        if (tt_schema.hash_conflict)
            CLAY_TEXT(CLAY_STRING("endpoints disagree on this topic's schema"),
                      CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_SEMI, FS_CAPTION), .textColor = P->red }));
    }
}

static void topics_inspect(AppState *app, const Palette *P, const Topic *t){
    int i;
    CLAY({ .id = CLAY_ID("topics_inspect_scroll"),
           .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0) },
                       .layoutDirection = CLAY_TOP_TO_BOTTOM, .padding = CLAY_PADDING_ALL(UISC(14)),
                       .childGap = UISCI(11) },
           .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() } }) {
        CLAY_TEXT(ui_str(t->path), CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_BODY), .textColor = P->text }));

        ui_section_label(P, CLAY_STRING("QUALITY OF SERVICE"));
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .childGap = UISCI(8) } }) {
            topic_qos_cell(P, CLAY_STRING("Reliability"), tt_rel_word(t->reliable), ui_qos_color(P, t->reliable));
        }

        topic_schema_section(app, P, t);

        ui_section_label(P, ui_fmt("PUBLISHERS  %d", t->n_pubs + t->self_pub));
        if (t->n_pubs == 0 && !t->self_pub)
            CLAY_TEXT(CLAY_STRING("none"), CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_SMALL), .textColor = P->faint }));
        if (t->self_pub) topic_self_chip(P, t->self_pub_reliable);
        for (i = 0; i < t->n_pubs; i++) topic_node_chip(app, P, t->pubs[i], "pub_chip", i);

        ui_section_label(P, ui_fmt("SUBSCRIBERS  %d", t->n_subs + t->self_sub));
        if (t->n_subs == 0 && !t->self_sub)
            CLAY_TEXT(CLAY_STRING("none"), CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_SMALL), .textColor = P->faint }));
        if (t->self_sub) topic_self_chip(P, t->self_sub_reliable);
        for (i = 0; i < t->n_subs; i++) topic_node_chip(app, P, t->subs[i], "sub_chip", i);
    }
}

/* the Publish sidebar tab: the message composer (free-text, or the structured form for a
   typed topic) that used to live pinned under the feed, now hosted in the drawer */
static void topics_publish(AppState *app, const Palette *P, const Topic *t){
    CLAY({ .id = CLAY_ID("topics_publish_scroll"),
           .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0) },
                       .layoutDirection = CLAY_TOP_TO_BOTTOM, .padding = CLAY_PADDING_ALL(UISC(14)),
                       .childGap = UISCI(11) },
           .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() } }) {
        CLAY_TEXT(ui_str(t->path), CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_BODY), .textColor = P->text }));
        ui_section_label(P, CLAY_STRING("COMPOSE MESSAGE"));
        topics_composer(app, P, t);
    }
}

/* ====================================================== right: inspect a message */

/* one field of a decoded message: name (indented by depth) then its value. A struct field
   is a group header (its members follow, indented); scalars/arrays show their value, which
   wraps so a long array is shown in full rather than clipped. */
static void tt_msg_field_row(const Palette *P, const CapMsgField *f, int idx){
    int is_struct = !strcmp(f->value, "{...}");   /* the decoder folds a struct's own value */
    CLAY({ .id = CLAY_IDI("msg_field", (uint32_t)idx),
           .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) },
                       .padding = { .left = UISCI(10 + f->depth * 12), .right = UISCI(10),
                                    .top = UISCI(5), .bottom = UISCI(5) }, .childGap = UISCI(10) },
           .border = { .width = { 0, 0, 0, UISCI(1), 0 }, .color = P->border } }) {
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_FIXED(UISC(96 - f->depth * 12)) } } }) {
            CLAY_TEXT(ui_str(f->name), CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_SMALL),
                                                          .textColor = is_struct ? P->dim : P->faint,
                                                          .wrapMode = CLAY_TEXT_WRAP_NONE }));
        }
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {
            if (!is_struct)
                CLAY_TEXT(ui_str(f->value), CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_SMALL),
                                                              .textColor = P->text, .wrapMode = CLAY_TEXT_WRAP_WORDS }));
        }
    }
}

/* one labelled meta line (fixed label column then value) for the message header card */
static void tt_msg_meta_row(const Palette *P, Clay_String label, Clay_String value, Clay_Color vcol){
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(UISC(22)) },
                       .childGap = UISCI(8), .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_FIXED(UISC(52)) } } }) {
            CLAY_TEXT(label, CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_CAPTION),
                                                .textColor = P->faint, .wrapMode = CLAY_TEXT_WRAP_NONE }));
        }
        CLAY_TEXT(value, CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_SMALL),
                                            .textColor = vcol, .wrapMode = CLAY_TEXT_WRAP_NONE }));
    }
}

/* the Inspect sidebar tab showing a message picked from the feed: a back control, the source
   topic, a meta card (from / time / size / type), then the full decoded field hierarchy (or
   the raw payload for a schema-less sender). Reads the durable copy in app->inspect_msg. */
static void topics_msg_inspect(AppState *app, const Palette *P){
    const CapFeedItem *m = &app->inspect_msg;
    int f;
    CLAY({ .id = CLAY_ID("topics_msg_scroll"),
           .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0) },
                       .layoutDirection = CLAY_TOP_TO_BOTTOM, .padding = CLAY_PADDING_ALL(UISC(14)),
                       .childGap = UISCI(11) },
           .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() } }) {
        /* back to the topic overview */
        CLAY({ .id = CLAY_ID("msg_back"),
               .layout = { .padding = { .right = UISCI(6) }, .childGap = UISCI(4),
                           .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
            if (Clay_Hovered() && g_pointer_pressed) app->has_inspect_msg = 0;
            ui_icon(ICON_CHEVRON_LEFT, 14, Clay_Hovered() ? P->text : P->dim);
            CLAY_TEXT(CLAY_STRING("Topic"), CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_SEMI, FS_SMALL),
                                                              .textColor = Clay_Hovered() ? P->text : P->dim,
                                                              .wrapMode = CLAY_TEXT_WRAP_NONE }));
        }
        CLAY_TEXT(ui_str(app->inspect_msg_topic), CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_BODY),
                                                                    .textColor = P->text, .wrapMode = CLAY_TEXT_WRAP_NONE }));

        /* meta card */
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .layoutDirection = CLAY_TOP_TO_BOTTOM,
                           .padding = CLAY_PADDING_ALL(UISC(10)), .childGap = UISCI(2) },
               .backgroundColor = P->panel2, .cornerRadius = CLAY_CORNER_RADIUS(UISC(6)),
               .border = { .width = CLAY_BORDER_OUTSIDE(1), .color = P->border } }) {
            tt_msg_meta_row(P, CLAY_STRING("From"),
                            m->mine ? ui_fmt("%s (you)", m->sender) : ui_str(m->sender),
                            m->mine ? P->accent : P->text);
            tt_msg_meta_row(P, CLAY_STRING("Time"), ui_fmt("%.2f s", m->t_s), P->dim);
            tt_msg_meta_row(P, CLAY_STRING("Size"), ui_fmt("%u B", m->len), P->dim);
            tt_msg_meta_row(P, CLAY_STRING("Type"),
                            m->decoded && m->type_name[0] ? ui_str(m->type_name) : CLAY_STRING("raw bytes"),
                            m->decoded && m->type_name[0] ? P->accent : P->dim);
        }

        if (m->decoded){
            ui_section_label(P, ui_fmt("FIELDS  %d", m->total_fields));
            CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .layoutDirection = CLAY_TOP_TO_BOTTOM },
                   .backgroundColor = P->panel2, .cornerRadius = CLAY_CORNER_RADIUS(UISC(6)),
                   .border = { .width = CLAY_BORDER_OUTSIDE(1), .color = P->border } }) {
                for (f = 0; f < m->n_fields; f++) tt_msg_field_row(P, &m->fields[f], f);
                if (m->total_fields > m->n_fields)
                    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .padding = CLAY_PADDING_ALL(UISC(9)) } }) {
                        CLAY_TEXT(ui_fmt("+%d more fields", m->total_fields - m->n_fields),
                                  CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_CAPTION), .textColor = P->faint }));
                    }
            }
        } else {
            char buf[CAP_MSG_PREVIEW + 1];
            tt_sanitize(buf, (int)sizeof buf, m->preview, m->preview_len);
            ui_section_label(P, CLAY_STRING("PAYLOAD"));
            CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .padding = CLAY_PADDING_ALL(UISC(10)) },
                   .backgroundColor = P->code_bg, .cornerRadius = CLAY_CORNER_RADIUS(UISC(6)),
                   .border = { .width = CLAY_BORDER_OUTSIDE(1), .color = P->border } }) {
                CLAY_TEXT(ui_fmt("%s", buf), CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_SMALL),
                                                               .textColor = P->text, .wrapMode = CLAY_TEXT_WRAP_WORDS }));
            }
            if (m->len > m->preview_len)
                CLAY_TEXT(ui_fmt("+%u more bytes", m->len - m->preview_len),
                          CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_CAPTION), .textColor = P->faint }));
        }
    }
}

static void topics_drawer(AppState *app, const Palette *P){
    const Dataset *D = app->data;
    const Topic *t = (D && D->n_topics && app->sel_topic >= 0 && app->sel_topic < D->n_topics)
                     ? &D->topics[app->sel_topic] : NULL;
    /* an inspected message belongs to one topic; drop it if the selection moved elsewhere so
       the durable copy never shows against the wrong (or no) topic */
    if (app->has_inspect_msg && (!t || strcmp(app->inspect_msg_topic, t->path) != 0))
        app->has_inspect_msg = 0;
    CLAY({ .id = CLAY_ID("topics_drawer"),
           .layout = { .sizing = { .width = CLAY_SIZING_FIXED(UISC(TT_DRAWER_W)), .height = CLAY_SIZING_GROW(0) },
                       .layoutDirection = CLAY_TOP_TO_BOTTOM },
           .backgroundColor = P->panel,
           .border = { .width = { UISCI(1), 0, 0, 0, 0 }, .color = P->border } }) {
        topics_drawer_header(app, P);
        if (app->drawer_tab == DRAWER_INSPECT && app->has_inspect_msg) topics_msg_inspect(app, P);
        else if (!t) ui_placeholder(P, CLAY_STRING("no topic selected"));
        else if (app->drawer_tab == DRAWER_PUBLISH) topics_publish(app, P, t);
        else    topics_inspect(app, P, t);
    }
}

static void topics_tab(AppState *app, const Palette *P){
    topics_tree(app, P);
    topics_feed(app, P);
    if (app->drawer_open) topics_drawer(app, P);
}

/* Keep the feed glued to the newest message while the user is parked at the bottom. Runs
   AFTER Clay_EndLayout (the scroll container's content/size are final then) and writes Clay's
   stored scroll position for the next frame. The user scrolling up unlocks the pin; scrolling
   back to the bottom re-locks it; selecting another topic resets to pinned. Content growing
   (new messages) does not move Clay's stored offset, so it never reads as a user scroll. */
static void topics_feed_autoscroll(AppState *app){
    Clay_ScrollContainerData d;
    float max_scroll, y;
    if (app->tab != TAB_TOPICS) return;          /* the feed container only exists on this tab */
    d = Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("topics_feed_scroll")));
    if (!d.found || !d.scrollPosition) return;
    max_scroll = d.contentDimensions.height - d.scrollContainerDimensions.height;
    if (max_scroll < 0.0f) max_scroll = 0.0f;
    y = d.scrollPosition->y;                      /* <= 0: 0 = top, -max_scroll = bottom */

    if (app->feed_sel_topic != app->sel_topic){   /* new topic: start pinned, no false unlock */
        app->feed_sel_topic = app->sel_topic;
        app->feed_pinned = 1;
        app->feed_prev_scroll_y = y;
    }
    if (app->feed_pinned){
        if (y > app->feed_prev_scroll_y + 1.0f) app->feed_pinned = 0;   /* user pulled up: unlock */
    } else if (-y >= max_scroll - 2.0f){
        app->feed_pinned = 1;                                           /* back at bottom: re-lock */
    }
    if (app->feed_pinned) y = -max_scroll;
    d.scrollPosition->y = y;
    app->feed_prev_scroll_y = y;
}

#endif /* UI_TAB_TOPICS_H */
