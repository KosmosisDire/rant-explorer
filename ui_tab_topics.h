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
static int         tt_send_pending;   /* a send is parked on the forming match (spinner) */
static uint32_t    tt_msgs, tt_drops;
static CapSchema   tt_feed_schema[2];   /* the topic's schemas, for picking the feed's column
                                           templates: [0] the primary one (a FUNCTION's request),
                                           [1] a function's RESPONSE (empty for every other kind) */

/* entity glyph for a topic's kind: the capture layer folds pattern channels through the
   canonical reflection walk, so a function or variable arrives as ONE entity and this
   is a straight kind -> icon map. Returns 1 if it drew. Colored per family for a quick scan. */
static int tt_kind_icon(const Palette *P, int kind){
    IconId id; Clay_Color c;
    switch (kind){
        case CAP_KIND_TOPIC:    id = ICON_RSS;      c = P->dim;    break;
        case CAP_KIND_FUNCTION: id = ICON_FUNCTION; c = P->accent; break;
        case CAP_KIND_VARIABLE: id = ICON_VARIABLE; c = P->amber;  break;
        case CAP_KIND_TASK:     id = ICON_TASK;     c = P->green;  break;
        default: return 0;
    }
    ui_icon(id, 14, c);
    return 1;
}
static const char *tt_kind_label(const Topic *t){
    switch (t->kind){
        case CAP_KIND_FUNCTION: return t->incomplete ? "function (half advertised)" : "function";
        case CAP_KIND_VARIABLE: return t->incomplete ? "variable (half advertised)"
                                     : (t->writable ? "variable" : "variable (read-only)");
        case CAP_KIND_TASK:     return t->incomplete ? "task (half advertised)" : "task";
        default: return "topic";
    }
}
/* the send verb the composer/form buttons carry: sending IS calling / setting for a
   pattern entity (the capture layer routes it) */
static const char *tt_send_verb(const Topic *t){
    switch (t->kind){
        case CAP_KIND_FUNCTION: return "Call";
        case CAP_KIND_TASK:     return "Call";
        case CAP_KIND_VARIABLE: return "Set";
        default: return "Send";
    }
}
/* a function/task entity: the feed is a call log (request + reply groups, directed rows) */
static int tt_call_kind(int kind){ return kind == CAP_KIND_FUNCTION || kind == CAP_KIND_TASK; }

/* a freshly received message blinks the status light full green for this long (seconds);
   between messages it settles back to the dim subscribed green. */
#define TT_FLASH_S 0.12

/* 1 if a subscribed topic received a message within the flash window (a live count-up, so
   this dims frame by frame between messages and reads as a blink per sample). */
static int tt_recent(const Topic *t){
    return t && t->last_age_s >= 0.0 && t->last_age_s < TT_FLASH_S;
}

/* topic status light: hollow grey not subscribed, red dropping/erroring, and when subscribed
   a DIM green that blinks full green on each freshly received message (flash). */
static void tt_status_dot(const Palette *P, int sub_state, int flash){
    if (sub_state == 0){ ui_dot(UISC(7), UI_NONE, P->faint); return; }
    if (sub_state == 2){ ui_dot(UISC(7), P->red, UI_NONE); return; }
    if (flash) ui_dot(UISC(7), P->green, UI_NONE);          /* a message just arrived */
    else { Clay_Color g = P->green; g.a = 90; ui_dot(UISC(7), g, UI_NONE); }   /* subscribed, idle */
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

/* the topic filter box: a bare text box in the search row. Click to focus, a
   click anywhere else or Enter unfocuses, Escape or the X clears. The tree
   hides rows whose topic doesn't match (path or endpoint node name). */
static void ui_filter_box(AppState *app, const Palette *P, Clay_String hint){
    int focused = ui_tb_focused(&app->tb_filter), act;
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(UISC(30)) },
                       .padding = { .left = UISCI(9), .right = UISCI(9) }, .childGap = UISCI(7),
                       .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
           .backgroundColor = P->panel2, .cornerRadius = CLAY_CORNER_RADIUS(UISC(5)),
           .border = { .width = CLAY_BORDER_OUTSIDE(1), .color = focused ? P->accent : P->border } }) {
        ui_icon(ICON_SEARCH, 14, P->faint);
        act = ui_textbox(P, CLAY_ID("topic_filter_tb"), &app->tb_filter,
                         app->topic_filter, (int)sizeof app->topic_filter, &app->topic_filter_len,
                         &(UiTextBoxOpts){ .bare = 1, .fill_w = 1, .placeholder = hint,
                                           .fam = FAM_MONO, .wt = WT_REG, .sz = FS_SMALL });
        if (ui_tb_focused(&app->tb_filter))
            app->adding_topic = 0;               /* the two inputs never hold focus together */
        if (act & UI_TB_SUBMIT) ui_tb_blur(&app->tb_filter);
        if (act & UI_TB_CANCEL){
            app->topic_filter_len = 0; app->topic_filter[0] = '\0';
            ui_tb_reset(&app->tb_filter); ui_tb_blur(&app->tb_filter);
        }
        if (app->topic_filter_len){
            CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_FIXED(UISC(16)), .height = CLAY_SIZING_GROW(0) },
                               .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } } }) {
                if (Clay_Hovered() && g_pointer_pressed){
                    app->topic_filter_len = 0; app->topic_filter[0] = '\0';
                    ui_tb_reset(&app->tb_filter);
                    g_pointer_pressed = false;   /* consumed: don't also focus the box */
                }
                ui_icon(ICON_X, 12, Clay_Hovered() ? P->text : P->faint);
            }
        }
        if (Clay_Hovered() && g_pointer_pressed){   /* click on the row chrome (icon, padding) */
            ui_tb_focus(&app->tb_filter);
            app->tb_filter.caret = app->tb_filter.anchor = app->topic_filter_len;
            app->adding_topic = 0;
            g_pointer_pressed = false;
        }
    }
}

static Clay_String tt_rel_word(int reliable){
    return reliable ? CLAY_STRING("RELIABLE") : CLAY_STRING("BEST_EFFORT");
}

/* ------- the topic-tree category filter (the funnel button by the filter box) */

static const char tt_filter_menu_tok;   /* &this = the category-filter menu's owner token */

/* the category-filter checklist: grouped Kind / Reliability / State checkboxes (a disabled
   row heads each group), plus a "Clear filters" row when anything is set. Toggling a box
   keeps the menu open (ui_menu's checklist behaviour); the tree rebuild picks up the change
   next frame. Drawn each frame; idle unless open. */
static void tt_filter_menu(AppState *app, const Palette *P){
    UiMenuItem items[16];
    unsigned   bit[16];              /* the bit each row toggles; 0 = a non-toggle row (header / clear) */
    int n = 0, hit;
    if (!ui_menu_is_open(&tt_filter_menu_tok)) return;
    /* a group header: a dimmed, non-clickable label */
    #define TT_FM_HDR(s) do{ items[n] = (UiMenuItem){ .label = CLAY_STRING(s), .enabled = 0 }; \
                             bit[n] = 0; n++; }while(0)
    /* a checkbox row bound to category bit b */
    #define TT_FM_CHK(s, b) do{ items[n] = (UiMenuItem){ .label = CLAY_STRING(s), .enabled = 1, \
                                  .check = (app->topic_cats & (b)) ? UI_MENU_ON : UI_MENU_OFF }; \
                                bit[n] = (b); n++; }while(0)
    TT_FM_HDR("Kind");
    TT_FM_CHK("Topics",      TT_CAT_TOPIC);
    TT_FM_CHK("Functions",   TT_CAT_FUNCTION);
    TT_FM_CHK("Variables",   TT_CAT_VARIABLE);
    TT_FM_CHK("Tasks",       TT_CAT_TASK);
    TT_FM_HDR("Reliability");
    TT_FM_CHK("Reliable",    TT_CAT_RELIABLE);
    TT_FM_CHK("Best-effort", TT_CAT_BEST_EFF);
    TT_FM_HDR("State");
    TT_FM_CHK("Subscribed",  TT_CAT_SUBSCRIBED);
    TT_FM_CHK("Publishing",  TT_CAT_ACTIVE);
    if (app->topic_cats){           /* a plain (menu-closing) row to reset everything */
        items[n] = (UiMenuItem){ .label = CLAY_STRING("Clear filters"), .enabled = 1 };
        bit[n] = 0; n++;
    }
    #undef TT_FM_HDR
    #undef TT_FM_CHK
    hit = ui_menu(P, &tt_filter_menu_tok, items, n, 190);
    if (hit < 0) return;
    if (bit[hit]) app->topic_cats ^= bit[hit];   /* toggle the checkbox (menu stays open) */
    else          app->topic_cats = 0;           /* the "Clear filters" row (menu closes) */
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

static Clay_String tt_fit(const char *s, int budget);   /* defined with the feed cells below */

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

/* ------- the tree's right-click subscribe menu (a lone topic, or a branch's whole subtree) */

/* a real DATA topic: functions have no passive subscribe (replies are directed), so they are
   never a subscribe target here nor counted in a branch's "Subscribe All". */
static int tt_subscribable(const Topic *t){ return t->kind != CAP_KIND_FUNCTION; }

/* pull one path segment (split on '/' OR '.', either separator) starting at p */
static const char *tt_seg(const char *p, char *out, int cap){
    int n = 0;
    while (*p == '/' || *p == '.') p++;
    while (*p && *p != '/' && *p != '.'){ if (n < cap - 1) out[n++] = *p; p++; }
    out[n] = '\0';
    return p;
}
/* 1 if `topic` is a strict descendant of `branch` in segment terms; separator-agnostic, so a
   "a.b" topic matches under the tree's "a/b" branch path (the tree splits on both). */
static int tt_under(const char *topic, const char *branch){
    char bs[CAP_TOPIC_CAP], ts[CAP_TOPIC_CAP];
    const char *bp = branch, *tp = topic;
    for (;;){
        bp = tt_seg(bp, bs, sizeof bs);
        if (!bs[0]) break;                       /* branch fully consumed */
        tp = tt_seg(tp, ts, sizeof ts);
        if (!ts[0] || strcmp(bs, ts)) return 0;  /* diverged, or branch is longer */
    }
    tp = tt_seg(tp, ts, sizeof ts);              /* a descendant has at least one more segment */
    return ts[0] != '\0';
}
static int tt_find_topic(const Dataset *D, const char *path){
    int i;
    for (i = 0; i < D->n_topics; i++) if (!strcmp(D->topics[i].path, path)) return i;
    return -1;
}
static int tt_count_descendants(const Dataset *D, const char *branch){
    int i, n = 0;
    for (i = 0; i < D->n_topics; i++)
        if (tt_subscribable(&D->topics[i]) && tt_under(D->topics[i].path, branch)) n++;
    return n;
}
/* (un)subscribe every subscribable topic in a branch's subtree: the branch's own topic (if it
   has one) plus all descendants. Subscribe joins at each topic's recommended reliability. */
static void tt_subtree_apply(AppState *app, const char *self, const char *branch, int subscribe){
    const Dataset *D = app->data;
    int i, si;
    if (!app->cap) return;
    if (self[0] && (si = tt_find_topic(D, self)) >= 0){
        if (subscribe) cap_subscribe(app->cap, self, D->topics[si].reliable_recommend > 0);
        else           cap_unsubscribe(app->cap, self);
    }
    for (i = 0; i < D->n_topics; i++){
        const Topic *t = &D->topics[i];
        if (!tt_subscribable(t) || !tt_under(t->path, branch)) continue;
        if (subscribe) cap_subscribe(app->cap, t->path, t->reliable_recommend > 0);
        else           cap_unsubscribe(app->cap, t->path);
    }
}

/* live tally of a subtree (the branch's own topic + descendants): how many subscribable topics
   it holds, and how many of those the explorer is currently subscribed to */
static void tt_subtree_counts(const Dataset *D, const char *self, const char *branch,
                              int *total, int *subscribed){
    int i, si;
    *total = *subscribed = 0;
    if (self[0] && (si = tt_find_topic(D, self)) >= 0){
        (*total)++;
        if (D->topics[si].sub_state) (*subscribed)++;
    }
    for (i = 0; i < D->n_topics; i++){
        const Topic *t = &D->topics[i];
        if (!tt_subscribable(t) || !tt_under(t->path, branch)) continue;
        (*total)++;
        if (t->sub_state) (*subscribed)++;
    }
}

static const char tt_tree_menu_tok;   /* &this = the tree menu's owner token */
static char tt_menu_branch[96];        /* the right-clicked node's tree path (subtree scan key) */
static char tt_menu_self[96];          /* its own topic path, or "" if a pure namespace / function */
static int  tt_menu_desc_count;        /* subscribable descendant topics (excludes the self topic) */

/* the tree's per-row menu: "Subscribe"/"Unsubscribe" for the row's own topic, and (for a branch)
   "Subscribe All (N)" / "Unsubscribe All (N)" over its whole subtree. Each item is shown only
   when it would act on something, so it is never a no-op. Drawn each frame; idle unless open. */
static void tt_tree_menu(AppState *app, const Palette *P){
    const Dataset *D = app->data;
    UiMenuItem items[3];
    char       act[3];               /* 's' self toggle, 'a' subscribe subtree, 'u' unsubscribe subtree */
    int n = 0, hit, self_idx, subscribed, total = 0, sub_cnt = 0;
    if (!ui_menu_is_open(&tt_tree_menu_tok)) return;
    self_idx   = tt_menu_self[0] ? tt_find_topic(D, tt_menu_self) : -1;
    subscribed = self_idx >= 0 && D->topics[self_idx].sub_state != 0;
    if (tt_menu_desc_count > 0) tt_subtree_counts(D, tt_menu_self, tt_menu_branch, &total, &sub_cnt);
    if (tt_menu_self[0]){
        items[n] = (UiMenuItem){ .label = subscribed ? CLAY_STRING("Unsubscribe")
                                                      : CLAY_STRING("Subscribe"), .enabled = 1 };
        act[n++] = 's';
    }
    if (tt_menu_desc_count > 0 && sub_cnt < total){    /* something left to join */
        items[n] = (UiMenuItem){ .label = ui_fmt("Subscribe All (%d)", total), .enabled = 1 };
        act[n++] = 'a';
    }
    if (tt_menu_desc_count > 0 && sub_cnt > 0){        /* something to drop */
        items[n] = (UiMenuItem){ .label = ui_fmt("Unsubscribe All (%d)", total), .enabled = 1 };
        act[n++] = 'u';
    }
    if (n == 0){ ui_menu_close(); return; }
    hit = ui_menu(P, &tt_tree_menu_tok, items, n, 200);
    if (hit < 0 || !app->cap) return;
    if (act[hit] == 's'){
        if (subscribed) cap_unsubscribe(app->cap, tt_menu_self);
        else if (self_idx >= 0) cap_subscribe(app->cap, tt_menu_self,
                                              D->topics[self_idx].reliable_recommend > 0);
    } else {
        tt_subtree_apply(app, tt_menu_self, tt_menu_branch, act[hit] == 'a');
    }
}

/* name_col = the reserved left-region width in px (caret zone + gap + name text, == the
   panel's name_max); cw = the mono-small char advance. The name is content-sized with no
   horizontal clip (a per-row clip would blow Clay's ~10 scroll-container slots, so we
   truncate like the feed cells do), so it MUST be trimmed to what fits, else a name wider
   than the clamped budget grows the GROW row past the fixed-width panel and spills the
   stat columns + scrollbar off the sidebar's right edge. */
static void topic_tree_row(AppState *app, const Palette *P, const TreeRow *row, int idx,
                           float name_col, float cw, float gap){
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
                if (Clay_Hovered() && g_pointer_pressed){ app_toggle_expanded(app, row->path); g_pointer_pressed = false; }
                ui_icon(row->open ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, 12, P->dim);
            }
        }
        /* topic glyph: a plain topic shows the hash (a named channel); a function or
           variable shows its type icon instead (tt_kind_icon maps the kind). A namespace
           branch has no topic and shows nothing here. */
        if (row->has_topic){
            CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_FIXED(UISC(18)), .height = CLAY_SIZING_GROW(0) },
                               .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } } }) {
                tt_kind_icon(P, row->topic->kind);
            }
        }
        /* name: a branch (has children) gets a trailing '/' so a namespace reads as a
           path segment; dim when it's a pure namespace, normal when it's a topic */
        CLAY({ .id = CLAY_IDI("tree_name", (uint32_t)idx),
               .layout = { .sizing = { .height = CLAY_SIZING_GROW(0) },
                           .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
            /* px left for the text = the name region minus this row's caret zone, the gap,
               and (for a topic) the kind glyph. Truncate to that many mono chars so the
               row can never grow wider than the panel (see the header comment above). */
            float text_px = name_col - UISC(10 + row->depth * 15 + 24) - gap
                          - (row->has_topic ? UISC(18) + gap : 0.0f);
            int budget = (cw > 0.0f && text_px > cw) ? (int)(text_px / cw) : 1;
            CLAY_TEXT(tt_fit(ui_fmt("%s%s", row->name, row->is_branch ? "/" : "").chars, budget),
                      CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_SMALL),
                                         .textColor = row->has_topic ? P->text : P->dim,
                                         .wrapMode = CLAY_TEXT_WRAP_NONE }));
        }
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}   /* spacer: right-anchor the stat columns */
        /* status dot: live subscription state (green subscribed, red dropping, grey not) */
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_FIXED(UISC(TT_COL_DOT)) },
                           .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } } }) {
            if (row->has_topic) tt_status_dot(P, row->topic->sub_state, tt_recent(row->topic));
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
        /* right-click opens the subscribe menu: "Subscribe" for the row's own topic, and/or
           "Subscribe All (N)" when the subtree holds subscribable descendants. Nothing to
           offer (a lone function, an empty namespace) means no menu. */
        if (Clay_Hovered() && g_right_pressed){
            int has_self = row->has_topic && tt_subscribable(row->topic);
            int desc     = tt_count_descendants(D, row->path);
            if (has_self || desc > 0){
                snprintf(tt_menu_branch, sizeof tt_menu_branch, "%s", row->path);
                snprintf(tt_menu_self, sizeof tt_menu_self, "%s", has_self ? row->topic->path : "");
                tt_menu_desc_count = desc;
                ui_menu_open(&tt_tree_menu_tok, g_pointer_x, g_pointer_y);
            }
            g_right_pressed = false;
        }
        /* whole-row click selects the topic (the caret already consumed its own click);
           a pure namespace has nothing to select, so a click toggles its collapse */
        if (Clay_Hovered() && g_pointer_pressed){
            if (row->has_topic){
                app_select_topic(app, D, (int)(row->topic - D->topics));
                app->drawer_open = 1;
                app->has_inspect_msg = 0;  /* clicking a topic returns to inspecting the topic */
                app->adding_topic = 0;     /* selecting a topic exits the new-topic field */
            } else if (row->is_branch){
                app_toggle_expanded(app, row->path);
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
    ui_tb_reset(&app->tb_new_topic);
    ui_tb_blur(&app->tb_new_topic);
}

/* the inline "new topic name" field, shown under the filter while adding_topic.
   Enter or Add commits, Escape closes. */
static void tt_new_topic_input(AppState *app, const Palette *P){
    int can, act;
    if (!app->adding_topic){ ui_tb_blur(&app->tb_new_topic); return; }   /* closed elsewhere */
    can = app->new_topic_len > 0;
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .childGap = UISCI(6),
                       .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
        act = ui_textbox(P, CLAY_ID("new_topic_tb"), &app->tb_new_topic,
                         app->new_topic, (int)sizeof app->new_topic, &app->new_topic_len,
                         &(UiTextBoxOpts){ .fill_w = 1, .h_min = 30, .pad_x = 9, .radius = 5,
                                           .fam = FAM_MONO, .wt = WT_REG, .sz = FS_SMALL,
                                           .bg = P->panel2, .border = P->accent, .border_focus = P->accent });
        if (act & UI_TB_SUBMIT) tt_commit_new_topic(app);
        if (act & UI_TB_CANCEL){
            app->adding_topic = 0; app->new_topic_len = 0; app->new_topic[0] = '\0';
            ui_tb_reset(&app->tb_new_topic); ui_tb_blur(&app->tb_new_topic);
        }
        if (app->adding_topic &&
            ui_pill(P, CLAY_STRING("Add"), FAM_SANS, WT_SEMI, FS_SMALL,
                    can ? P->accent : P->faint, can ? P->accent_bg : P->panel2, UI_NONE, UISC(30)) && can)
            tt_commit_new_topic(app);
    }
}

/* how many of the longest labels to actually TTF-measure when sizing the name column */
#define TT_NAME_CAND 24

static void topics_tree(AppState *app, const Palette *P){
    const Dataset *D = app->data;
    int n = ui_tree_build(app), i;
    /* virtualization window: only rows in (or a few rows around) the viewport become Clay
       elements. Scroll data reflects the previous frame's layout (scrollPosition->y <= 0,
       0 = top); on the very first frame it is absent, so fall back to a generous top slice. */
    Clay_ScrollContainerData sd =
        Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("topics_tree_scroll")));
    float row_h  = UISC(29);
    int   margin = 4, first = 0, vis = 200, last;
    if (sd.found && sd.scrollPosition){
        float scrolled = -sd.scrollPosition->y;
        if (scrolled < 0.0f) scrolled = 0.0f;
        first = (int)(scrolled / row_h) - margin;
        vis   = (int)(sd.scrollContainerDimensions.height / row_h) + 1 + 2 * margin;
    }
    if (first < 0) first = 0;
    if (first > n) first = n;
    last = first + vis;
    if (last > n) last = n;
    /* fit the panel to its content: widest name column (indent + measured text) plus the fixed
       stat columns, clamped so one long name can't blow it out. Measure across the WHOLE tree,
       every segment in ut_pool (rows off-screen and rows inside collapsed branches included), so
       the panel width stays put as you scroll or expand/collapse. tt_name_w (TTF, uncached) is far
       too slow to call for all thousands of nodes, so rank cheaply by label length + indent and
       measure only the few longest candidates (the width clamps at 300 px, so an exact winner is
       not needed). */
    float gap        = UISC(TT_COL_GAP);
    float data_block = UISC(TT_COL_DOT) + UISC(TT_COL_RATE) + UISC(TT_COL_JITTER) + UISC(TT_COL_LAST)
                      + UISC(TT_COL_QOS) + gap * 5;
    float name_max   = UISC(70);
    float panel_w;
    {
        int cand[TT_NAME_CAND], key[TT_NAME_CAND], nc = 0, j, mi;
        for (i = 1; i < ut_n; i++){                       /* skip node 0, the implicit root */
            int k = (int)strlen(ut_pool[i].name) + ut_pool[i].depth * 2;   /* proxy: chars + indent */
            if (nc < TT_NAME_CAND){ cand[nc] = i; key[nc] = k; nc++; continue; }
            for (mi = 0, j = 1; j < nc; j++) if (key[j] < key[mi]) mi = j; /* evict the smallest */
            if (k > key[mi]){ cand[mi] = i; key[mi] = k; }
        }
        for (j = 0; j < nc; j++){
            const UtNode *u = &ut_pool[cand[j]];
            char  lbl[CAP_TOPIC_CAP + 2];
            float w;
            snprintf(lbl, sizeof lbl, "%s%s", u->name, u->n_children > 0 ? "/" : "");
            w = UISC(10 + u->depth * 15 + 24) + gap + tt_name_w(lbl);
            /* a topic row prefixes a kind glyph (hash for a plain topic, the type icon for a
               function or variable), so reserve the same width the row subtracts from its
               text budget, else its name gets trimmed. A namespace branch has none. */
            if (u->topic >= 0) w += UISC(18) + gap;
            if (w > name_max) name_max = w;
        }
    }
    if (name_max > UISC(300)) name_max = UISC(300);
    panel_w = name_max + gap + data_block + UISC(10) + UISC(16);   /* right pad + breathing room */
    if (panel_w < UISC(300)) panel_w = UISC(300);                  /* keep filter + add usable */
    float name_cw = tt_name_w("0000000000") / 10.0f;   /* mono-small char advance, for the name trim */
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
                ui_filter_box(app, P, CLAY_STRING("Filter topics..."));
                /* category filter: a funnel button that toggles the checklist menu; accented
                   while any category filter is active so it reads as "on" */
                if (ui_icon_button(P, ICON_FILTER, 15, 30,
                                   (app->topic_cats || ui_menu_is_open(&tt_filter_menu_tok)) ? P->accent : P->dim,
                                   P->text)){
                    if (ui_menu_is_open(&tt_filter_menu_tok)) ui_menu_close();
                    else ui_menu_open(&tt_filter_menu_tok, g_pointer_x, g_pointer_y);
                }
                if (ui_icon_button(P, ICON_PLUS, 16, 30, app->adding_topic ? P->accent : P->dim, P->text)){
                    app->adding_topic = !app->adding_topic;
                    app->new_topic_len = 0; app->new_topic[0] = '\0';
                    ui_tb_reset(&app->tb_new_topic);
                    if (app->adding_topic) ui_tb_focus(&app->tb_new_topic);
                    else                   ui_tb_blur(&app->tb_new_topic);
                }
            }
            tt_new_topic_input(app, P);
        }
        if (n > 0) tt_tree_header(P);
        CLAY({ .id = CLAY_ID("topics_tree_scroll"),
               .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0) },
                           .layoutDirection = CLAY_TOP_TO_BOTTOM },
               .clip = { .vertical = true, .childOffset = ui_scroll_offset(CLAY_ID("topics_tree_scroll")) } }) {
            if (n == 0)
                ui_placeholder(P, (app->topic_filter_len || app->topic_cats)
                                      ? CLAY_STRING("no topics match")
                                      : CLAY_STRING("no topics discovered"));
            else {
                /* virtualized: emit only rows [first,last); fixed-height spacers stand in
                   for the rest, so the total content height and the scrollbar are unchanged.
                   Row ids stay keyed to the global index i, so they are stable across frames. */
                if (first > 0)
                    CLAY({ .id = CLAY_ID("topics_tree_vtop"),
                           .layout = { .sizing = { .width  = CLAY_SIZING_GROW(0),
                                                   .height = CLAY_SIZING_FIXED((float)first * row_h) } } }) {}
                for (i = first; i < last; i++) topic_tree_row(app, P, &ut_rows[i], i, name_max, name_cw, gap);
                if (last < n)
                    CLAY({ .id = CLAY_ID("topics_tree_vbot"),
                           .layout = { .sizing = { .width  = CLAY_SIZING_GROW(0),
                                                   .height = CLAY_SIZING_FIXED((float)(n - last) * row_h) } } }) {}
            }
        }
        ui_scrollbar(P, CLAY_ID("topics_tree_scroll"));
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .padding = CLAY_PADDING_ALL(UISC(12)) },
               .border = { .width = { 0, 0, UISCI(1), 0, 0 }, .color = P->border } }) {
            ui_section_label(P, (app->topic_filter_len || app->topic_cats)
                ? ui_fmt("%d / %d TOPICS", ut_n_matched, D ? D->n_topics : 0)
                : ui_fmt("%d TOPICS", D ? D->n_topics : 0));
        }
    }
}

/* ============================================================ center: feed (table)

   The feed is a TABLE: columns are the message's fields (flattened, so a nested member reads
   pos.x), rows are samples (newest at the bottom). The header is sticky: it is a sibling
   ABOVE the scroll body sharing the same column widths, so the columns line up while the body
   scrolls. The first TT_TABLE_COLS fields show by default; right-click the header for a
   checklist of every column, the fixed time/sender ones included (field picks are re-seeded
   per topic, the time/sender toggles persist). A schema-less topic falls
   back to a single "payload" column. The shown field columns stretch to fill the table width
   (measured from the previous frame's laid-out body). Clicking a row copies that sample into
   the inspector (the full decode). Cell text is truncated to its (now dynamic) column width: a
   per-cell clip would overflow Clay's 10-slot scroll-container array, and the full value is
   always one click away in the inspector. Any header overrun on a narrow window is hidden by
   the (later-drawn, opaque) drawer or clipped at the window edge.

   TWO SCHEMAS, ONE TABLE. A FUNCTION feed interleaves our calls (the request schema) with the
   replies (the response schema), and a field index means something different in each, so one
   column template would print reply values under request column names. The columns are instead
   the UNION of the two field sets, each column TAGGED with its group (0 = primary/request,
   1 = response) and named with a req./rsp. prefix; a row fills only its own group's cells and
   leaves the other group blank, with a divider line at the boundary. Rows stay one per wire
   message in arrival order, so chronology is exactly the feed's. Every other kind has one
   group and renders as before. A row that carries no decode at all (a "call failed: TIMEOUT"
   entry) shows its text across the field area instead of a line of dashes. */

#define TT_TABLE_COLS 6    /* fields shown by default (right-click the header to change) */
#define TT_GROUP_COLS 3    /* ...per group when a feed has two (a function's req + rsp) */
#define TT_TIME_W   105    /* time column width, unscaled px (fits "12:34:56.789") */
#define TT_FROM_W   110    /* sender column width, unscaled px ("> " call / "< " reply + name) */
#define TT_FIELD_W   94    /* field column width, unscaled px (fallback before layout is known) */
#define TT_CELL_PADL  8    /* cell left padding, unscaled px */
#define TT_DRAWER_W 332    /* right sidebar width, unscaled px (must match topics_drawer) */
#define TT_FEED_PAD  16    /* topics_feed content padding, unscaled px */

#define TT_MAX_COLS (CAP_MSG_FIELDS * 2)   /* both groups' fields (see the section comment) */

/* a flattened column -> its fields[] index, within its group */
typedef struct { char name[80]; int fidx; int grp; } TblCol;
static TblCol tt_cols[TT_MAX_COLS];   /* every column (drives the right-click menu) */
static TblCol tt_vis[TT_MAX_COLS];    /* the visible subset, in column order (drives the table) */
static int    tt_seeded_cols;         /* columns the visible set has been seeded against */

/* append group `grp`'s columns (from a template decoded message) to cols[] at `at`, returning
   the new count: each non-struct field becomes a column named `prefix` + its dotted path
   (pos.x), keyed to its fields[] index (stable across samples of one schema, since every
   message enumerates the schema in the same depth-first order). A struct field contributes
   only its name as a path prefix. `prefix` is NULL on a single-group feed. */
static int tt_build_columns(const CapFeedItem *m, TblCol *cols, int at, int maxcols,
                            const char *prefix, int grp){
    static char stack[CAP_MSG_FIELDS][CAP_TOPIC_CAP];   /* ancestor field names, by depth */
    int i, n = at;
    for (i = 0; i < m->n_fields; i++){
        const CapMsgField *f = &m->fields[i];
        int d = f->depth < CAP_MSG_FIELDS ? f->depth : CAP_MSG_FIELDS - 1;
        snprintf(stack[d], sizeof stack[d], "%s", f->name);
        if (!strcmp(f->value, "{...}")) continue;       /* struct: a path prefix, not a column */
        if (n < maxcols){
            char *dst = cols[n].name; int cap = (int)sizeof cols[n].name, pos, k;
            pos = snprintf(dst, (size_t)cap, "%s", prefix ? prefix : "");
            if (pos < 0 || pos >= cap) pos = cap - 1;
            for (k = 0; k <= d; k++){
                int w = snprintf(dst + pos, (size_t)(cap - pos), k ? ".%s" : "%s", stack[k]);
                if (w < 0 || pos + w >= cap){ pos = cap - 1; break; }
                pos += w;
            }
            cols[n].fidx = i;
            cols[n].grp  = grp;
            n++;
        }
    }
    return n;
}

/* which column group a sample fills. Its own decode decides it (the root type + field count
   the writer declared, matched against the two advertised schemas); a sample that matches
   neither -- an undecoded outcome line, or a subset publisher -- falls back to the direction,
   which on a function feed is exact: our echoed call is `mine`, a reply never is. */
static int tt_msg_group(const CapFeedItem *m, int n_grp){
    int g;
    if (n_grp < 2) return 0;
    if (m->decoded)
        for (g = 1; g >= 0; g--)
            if (tt_feed_schema[g].inlined
                && m->total_fields == tt_feed_schema[g].total_fields
                && !strcmp(m->type_name, tt_feed_schema[g].type_name)) return g;
    return m->mine ? 0 : 1;
}

/* the feed sample whose decode drives group `g`'s columns: one matching that group's advertised
   schema by preference (a sender publishing a SUBSET of it must not narrow the columns), else
   the newest decoded sample on that side. -1 = nothing decoded to build the group from (its
   columns appear once the first such message lands). */
static int tt_template(int g, int n_grp){
    const CapSchema *sc = &tt_feed_schema[g];
    int j;
    if (sc->inlined)
        for (j = tt_feed_n - 1; j >= 0; j--)
            if (tt_feed[j].decoded
                && tt_feed[j].total_fields == sc->total_fields
                && !strcmp(tt_feed[j].type_name, sc->type_name)) return j;
    for (j = tt_feed_n - 1; j >= 0; j--)
        if (tt_feed[j].decoded && tt_msg_group(&tt_feed[j], n_grp) == g) return j;
    return -1;
}

/* a string truncated to `budget` characters, always copied into the frame pool (so callers
   may pass a local buffer). Keeps a fixed-width cell's text from overrunning into its neighbour. */
static Clay_String tt_fit(const char *s, int budget){
    int len = (int)strlen(s);
    if (budget < 1) budget = 1;
    if (len > budget) len = budget;
    return ui_fmt("%.*s", len, s);
}

/* one fixed-width table cell (mono small, left aligned, truncated to fit). `sep` draws the
   group divider down its left edge (the request | response boundary). */
static void tt_cell(const Palette *P, float w, const char *text, int budget, Clay_Color col,
                    int sep){
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_FIXED(w), .height = CLAY_SIZING_GROW(0) },
                       .padding = { .left = UISCI(TT_CELL_PADL), .right = UISCI(2) },
                       .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
           .border = { .width = { sep ? UISCI(1) : 0, 0, 0, 0, 0 }, .color = P->border2 } }) {
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

/* the sticky header row: the fixed time/sender columns (each hideable from the header
   menu) then a header cell per shown field, the group boundary marked by a divider.
   `field_w` is the stretched column width so the fields fill the table. `raw` (a schema-less
   topic) shows a single payload column instead.
   Right-clicking the row opens the column checklist menu at the cursor. */
static void tt_table_header(AppState *app, const Palette *P, const TblCol *cols, int n_show,
                            float field_w, int field_budget, int time_budget, int from_budget,
                            int raw){
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(UISC(26)) } },
           .backgroundColor = P->panel2,
           .border = { .width = { 0, 0, 0, UISCI(1), 0 }, .color = P->border2 } }) {
        int c;
        if (Clay_Hovered() && g_right_pressed){
            ui_menu_open(tt_cols, g_pointer_x, g_pointer_y);
            g_right_pressed = false;
        }
        if (app->col_show_time) tt_cell(P, UISC(TT_TIME_W), "written", time_budget, P->faint, 0);
        if (app->col_show_from) tt_cell(P, UISC(TT_FROM_W), "from", from_budget, P->faint, 0);
        if (raw) tt_cell_grow(P, "payload", P->faint);
        else for (c = 0; c < n_show; c++)
            tt_cell(P, field_w, cols[c].name, field_budget, P->faint,
                    c > 0 && cols[c].grp != cols[c - 1].grp);
    }
}

/* one sample row: the shown fixed columns then a cell per shown field (its decoded value,
   or "-" when a message lacks it), field cells stretched to `field_w`. Cells of the OTHER
   group (on a two-group function feed) stay blank: this row is a call or a reply, never both.
   `raw`, and any row with no decode at all, shows its payload/outcome text across the field
   area instead. Clicking copies the sample into the inspector; the inspected sample is
   highlighted. */
static void tt_table_row(AppState *app, const Palette *P, const char *topic, const CapFeedItem *m,
                         int idx, const TblCol *cols, int n_show, float field_w,
                         int field_budget, int time_budget, int from_budget, int kind, int raw,
                         int n_grp){
    int  selected = app->has_inspect_msg && m->uid == app->inspect_msg.uid
                    && !strcmp(topic, app->inspect_msg_topic);
    char tbuf[16], fbuf[CAP_NAME_CAP + 12];
    int  c;
    {   /* the WRITER's clock, local time of day: see cap_stamp on what it means when a row
           reads far from now (a replayed message, or a publisher whose host clock is off) */
        Clay_String cs = nf_clock_ms(m->wall_us);
        snprintf(tbuf, sizeof tbuf, "%.*s", (int)cs.length, cs.chars);
    }
    /* the sender, direction-marked on a call log ("> " = our call, "< " = the reply);
       a FORCED variable value carries its pin marker with the writer */
    snprintf(fbuf, sizeof fbuf, "%s%s%s",
             tt_call_kind(kind) ? (m->mine ? "> " : "< ") : "",
             m->sender,
             m->forced ? " [F]" : "");
    CLAY({ .id = CLAY_IDI("feed_row", (uint32_t)idx),
           .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(UISC(24)) } },
           .backgroundColor = selected ? P->accent_bg : (Clay_Hovered() ? P->panel : UI_NONE),
           .border = { .width = { 0, 0, 0, UISCI(1), 0 }, .color = P->border } }) {
        if (Clay_Hovered() && g_pointer_pressed) app_inspect_msg(app, topic, m);
        if (app->col_show_time) tt_cell(P, UISC(TT_TIME_W), tbuf, time_budget, P->faint, 0);
        if (app->col_show_from) tt_cell(P, UISC(TT_FROM_W), fbuf, from_budget,
                                        m->forced ? P->amber : m->mine ? P->accent : P->dim, 0);
        if (m->call_status){
            /* a failed call: the response message leads (never omitted), the payload's
               preview follows when the provider attached structured failure data */
            char pvbuf[CAP_MSG_PREVIEW + 1], failbuf[sizeof m->call_msg + CAP_MSG_PREVIEW + 32];
            int  fn = snprintf(failbuf, sizeof failbuf, "call failed: %s",
                               m->call_msg[0] ? m->call_msg : "?");
            if (m->len && fn > 0 && fn < (int)sizeof failbuf){
                tt_sanitize(pvbuf, (int)sizeof pvbuf, m->preview, m->preview_len);
                snprintf(failbuf + fn, sizeof failbuf - (size_t)fn, "  |  %s", pvbuf);
            }
            tt_cell_grow(P, failbuf, P->amber);
        } else if (raw || !m->decoded){
            char rawbuf[CAP_MSG_PREVIEW + 1];
            const char *pv = m->preview;
            if (!m->decoded){ tt_sanitize(rawbuf, (int)sizeof rawbuf, m->preview, m->preview_len); pv = rawbuf; }
            if (m->call_msg[0]){   /* an OK reply carrying debug text: show it beside the payload */
                char okbuf[CAP_MSG_PREVIEW + sizeof m->call_msg + 8];
                snprintf(okbuf, sizeof okbuf, "%s  --  %s", pv, m->call_msg);
                tt_cell_grow(P, okbuf, P->text);
            } else
                tt_cell_grow(P, pv, P->text);
        } else {
            int grp = tt_msg_group(m, n_grp);
            for (c = 0; c < n_show; c++){
                int sep = c > 0 && cols[c].grp != cols[c - 1].grp;
                const char *v = cols[c].grp != grp ? ""
                              : cols[c].fidx < m->n_fields ? m->fields[cols[c].fidx].value : "-";
                tt_cell(P, field_w, v, field_budget, P->text, sep);
            }
        }
    }
}

/* the header's right-click column menu: the generic ui_menu in checklist form, owned by
   tt_cols (one feed table exists at a time). The fixed time/sender columns lead, then every
   field; a click toggles visibility and the menu stays open for the next toggle. */
static void tt_col_menu(AppState *app, const Palette *P, const TblCol *cols, int n_cols){
    UiMenuItem items[TT_MAX_COLS + 2];
    int c, n = 0, hit;
    if (!ui_menu_is_open(tt_cols)) return;
    items[n++] = (UiMenuItem){ .label = CLAY_STRING("written"), .enabled = 1,
                               .check = app->col_show_time ? UI_MENU_ON : UI_MENU_OFF };
    items[n++] = (UiMenuItem){ .label = CLAY_STRING("from"), .enabled = 1,
                               .check = app->col_show_from ? UI_MENU_ON : UI_MENU_OFF };
    for (c = 0; c < n_cols; c++)
        items[n++] = (UiMenuItem){ .label = ui_str(cols[c].name), .enabled = 1,
                                   .check = app_col_visible(app, cols[c].name) ? UI_MENU_ON
                                                                               : UI_MENU_OFF };
    hit = ui_menu(P, tt_cols, items, n, 210);
    if (hit == 0)      app->col_show_time = !app->col_show_time;
    else if (hit == 1) app->col_show_from = !app->col_show_from;
    else if (hit >= 2) app_col_toggle(app, cols[hit - 2].name);
}

/* publish the composed message to `topic` and clear the box */
static void tt_do_send(AppState *app, const char *topic){
    if (app->compose_len <= 0 || !app->cap || !topic) return;
    cap_publish(app->cap, topic, app->compose, (size_t)app->compose_len);
    app->compose_len = 0;
    app->compose[0]  = '\0';
    ui_tb_reset(&app->tb_compose);
}

/* ------------------------------------------------- structured publish form (typed topics) */

/* the prefilled default for a form field, by its kind */
static const char *tt_form_default(const CapSchemaField *f){
    switch (f->kind){
        case CAP_K_BOOL:                 return "false";
        case CAP_K_ENUM:                 /* the first option name (the dropdown's initial pick) */
            return f->n_variants ? f->variants[0] : "0";
        case CAP_K_STRUCT:               /* no setter yet: stays default */
        case CAP_K_STR:                  /* empty string(s) */
        case CAP_K_ARR:                  /* array: empty = all zero */
        case CAP_K_VSTR:                 /* variable string/array: empty */
        case CAP_K_VARR:
        case CAP_K_MAP:  return "";      /* map: read-only, stays empty */
        default:         return "0";     /* numeric scalar */
    }
}

/* overwrite a form field's value (bool toggle, quick fills, defaults) */
static void tt_form_set_val(AppState *app, int i, const char *v){
    snprintf(app->form_val[i], UI_FORM_VAL, "%s", v);
    app->form_len[i] = (int)strlen(app->form_val[i]);
    ui_tb_reset(&app->tb_form[i]);   /* the buffer changed under the box */
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
    if (f->kind == CAP_K_STR || f->kind == CAP_K_VSTR) return CLAY_STRING("text");
    if (f->kind == CAP_K_ARR || f->kind == CAP_K_VARR) return CLAY_STRING("comma-separated");
    return CLAY_STRING("0");
}

/* one segment of the boolean toggle: click sets the field to `seg` ("true"/"false") */
static void tt_form_bool_seg(AppState *app, const Palette *P, int i, const char *seg, int active){
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0) },
                       .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } },
           .backgroundColor = active ? P->accent_bg : UI_NONE,
           .cornerRadius = CLAY_CORNER_RADIUS(UISC(3)) }) {
        if (Clay_Hovered() && g_pointer_pressed){
            tt_form_set_val(app, i, seg);
            app->form_focus = i; app->adding_topic = 0;
            ui_tb_blur_all();   /* keyboard "focus" is the bool row now, no text box */
        }
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
        ui_tb_reset(&app->tb_form[i]);
    }
    app->form_focus = 0;
    if (g_tb_focus == NULL && n > 0 && sc->fields[0].kind != CAP_K_BOOL &&
        sc->fields[0].kind != CAP_K_ENUM &&
        sc->fields[0].kind != CAP_K_STRUCT && sc->fields[0].kind != CAP_K_MAP){
        ui_tb_focus(&app->tb_form[0]);           /* type straight into the first field... */
        app->tb_form[0].anchor = 0;              /* ...replacing its prefilled default */
        app->tb_form[0].caret  = app->form_len[0];
    }
}

static void tt_form_send(AppState *app, const Topic *t, int n){
    const char *vals[UI_FORM_MAX]; int i;
    if (!app->cap) return;
    for (i = 0; i < n; i++) vals[i] = app->form_val[i];
    cap_publish_form(app->cap, t->path, vals, n);   /* the values stay for the next send */
}
/* variable debug override: pin the form's value until Unforce (op bits on the set channel) */
static void tt_form_force(AppState *app, const Topic *t, int n){
    const char *vals[UI_FORM_MAX]; int i;
    if (!app->cap) return;
    for (i = 0; i < n; i++) vals[i] = app->form_val[i];
    cap_variable_force_form(app->cap, t->path, vals, n);
}

/* one field: name | type-aware input | type, nested members indented. A bool gets a
   toggle, a string/array gets a validated text box with an "n/cap" counter, a numeric
   scalar a validated text box; the box + type turn red when the typed value won't parse
   (`valid` comes from cap_form_validate). A struct row is a read-only group header
   (members are the inputs). Returns the text box's action bits (Enter, Tab, ...). */
static int tt_form_field_row(AppState *app, const Palette *P, const CapSchemaField *f, int i, int valid){
    int is_struct = (f->kind == CAP_K_STRUCT || f->kind == CAP_K_MAP);   /* read-only rows */
    int is_bool   = (f->kind == CAP_K_BOOL);
    int is_enum   = (f->kind == CAP_K_ENUM);
    int has_val   = app->form_val[i][0] != '\0';
    int bad       = !is_struct && !is_bool && !is_enum && has_val && !valid;   /* typed, but won't parse */
    int show_count = (f->kind == CAP_K_STR || f->kind == CAP_K_ARR
                   || f->kind == CAP_K_VSTR || f->kind == CAP_K_VARR);
    int act = 0;
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
            tt_form_bool(app, P, i, app->form_focus == i && g_tb_focus == NULL);
        } else if (is_enum){
            /* the enum's options -> a dropdown; the current pick is whichever variant name
               the field's value holds (form_val is a variant name, set here or by default) */
            const char *opts[CAP_ENUM_VARIANTS];
            int k, sel = -1, picked;
            for (k = 0; k < f->n_variants; k++){
                opts[k] = f->variants[k];
                if (!strcmp(app->form_val[i], f->variants[k])) sel = k;
            }
            picked = ui_dropdown(P, &app->form_val[i], CLAY_IDI("form_dd", (uint32_t)i),
                                 opts, f->n_variants, sel, 0);
            if (picked >= 0){
                tt_form_set_val(app, i, f->variants[picked]);
                app->form_focus = i; app->adding_topic = 0;
                ui_tb_blur_all();               /* keyboard "focus" is this enum row now, no text box */
            }
        } else if (!is_struct){
            act = ui_textbox(P, CLAY_IDI("form_tb", (uint32_t)i), &app->tb_form[i],
                             app->form_val[i], UI_FORM_VAL, &app->form_len[i],
                             &(UiTextBoxOpts){ .fill_w = 1, .h_min = 24, .pad_x = 8, .radius = 4,
                                               .placeholder = tt_form_hint(f),
                                               .fam = FAM_MONO, .wt = WT_REG, .sz = FS_SMALL,
                                               .bg = P->panel2,
                                               .border = bad ? P->red : P->border,
                                               .border_focus = bad ? P->red : P->accent });
            if (ui_tb_focused(&app->tb_form[i])){ app->form_focus = i; app->adding_topic = 0; }
            if (act & UI_TB_CANCEL){                     /* Escape clears the field */
                app->form_val[i][0] = '\0'; app->form_len[i] = 0;
                ui_tb_reset(&app->tb_form[i]);
            }
        } else {
            CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
        }
        if (show_count){    /* live shape cue: bytes/cap for a string, elements/count for an array
                               (a variable field has no bound: just the live count) */
            int is_str = (f->kind == CAP_K_STR || f->kind == CAP_K_VSTR);
            int cur = is_str ? (int)strlen(app->form_val[i]) : tt_count_elems(f, app->form_val[i]);
            int cap = f->kind == CAP_K_STR ? (int)f->str_cap : f->kind == CAP_K_ARR ? (int)f->count : 0;
            int over = cap && cur > cap;
            CLAY_TEXT(cap ? ui_fmt("%d/%d", cur, cap) : ui_fmt("%d", cur),
                      CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_CAPTION),
                                          .textColor = (bad || over) ? P->red : P->faint, .wrapMode = CLAY_TEXT_WRAP_NONE }));
        }
        CLAY_TEXT(ui_str(f->type), CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_CAPTION),
                                                      .textColor = bad ? P->red : is_struct ? P->dim : P->accent,
                                                      .wrapMode = CLAY_TEXT_WRAP_NONE }));
    }
    return act;
}

/* the structured composer: the message type name, one input per schema field (nested members
   indented) prefilled with defaults, then a Send button under them. Enter or Send publishes
   (empty fields keep the canonical default zero). */
static void topics_form(AppState *app, const Palette *P, const Topic *t, const CapSchema *sc){
    int i, n = sc->n_fields < UI_FORM_MAX ? sc->n_fields : UI_FORM_MAX;
    int send = 0, tab = 0, tab_from = -1;
    unsigned char valid[UI_FORM_MAX];
    if (app->form_topic != app->sel_topic){       /* switched topic: fresh defaults */
        app->form_topic = app->sel_topic;
        tt_form_reset(app, sc, n);
    }
    app->form_n = n;
    {   const char *vals[UI_FORM_MAX];            /* live per-field validity (one schema parse) */
        for (i = 0; i < n; i++){ vals[i] = app->form_val[i]; valid[i] = 1; }
        if (app->cap) cap_form_validate(app->cap, t->path, vals, n, valid);
    }
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
        for (i = 0; i < n; i++){
            int a = tt_form_field_row(app, P, &sc->fields[i], i, valid[i]);
            if (a & UI_TB_SUBMIT) send = 1;
            if (a & (UI_TB_TAB | UI_TB_BACKTAB)){ tab = (a & UI_TB_TAB) ? 1 : -1; tab_from = i; }
        }
        /* a focused bool/enum row has no text box, so its keys stay in the frame queue:
           Space toggles a bool, Enter sends, Tab cycles on */
        if (g_tb_focus == NULL && app->form_focus >= 0 && app->form_focus < n &&
            (sc->fields[app->form_focus].kind == CAP_K_BOOL
             || sc->fields[app->form_focus].kind == CAP_K_ENUM)){
            if (sc->fields[app->form_focus].kind == CAP_K_BOOL && ui_tb_take_key(SDLK_SPACE, 0))
                tt_form_set_val(app, app->form_focus,
                                strcmp(app->form_val[app->form_focus], "true") ? "true" : "false");
            if (ui_tb_take_key(SDLK_RETURN, 0) || ui_tb_take_key(SDLK_KP_ENTER, 0)) send = 1;
            if (ui_tb_take_key(SDLK_TAB, SDL_KMOD_SHIFT)){ tab = -1; tab_from = app->form_focus; }
            else if (ui_tb_take_key(SDLK_TAB, 0)){ tab = 1; tab_from = app->form_focus; }
        }
        if (tab && n > 0){                        /* cycle to the next editable/bool field */
            int j = tab_from, k;
            for (k = 0; k < n; k++){
                j = (j + tab + n) % n;
                if (sc->fields[j].kind != CAP_K_STRUCT && sc->fields[j].kind != CAP_K_MAP) break;
            }
            app->form_focus = j;
            if (sc->fields[j].kind == CAP_K_BOOL || sc->fields[j].kind == CAP_K_ENUM) ui_tb_blur_all();
            else {
                ui_tb_focus(&app->tb_form[j]);
                app->tb_form[j].anchor = 0;       /* select the value, so typing replaces it */
                app->tb_form[j].caret  = app->form_len[j];
                app->tb_form[j].ensure = 1;
            }
        }
        if (send) tt_form_send(app, t, n);
        /* Send: its own row under the fields (right-aligned), not boxed in a card. A
           writable, forceable variable also gets the debug override pair: Force pins the
           form's value at the owner (writes absorb until Unforce releases it). */
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .padding = { .top = UISCI(4) },
                           .childGap = UISCI(8) } }) {
            CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
            /* the owner must also permit force (allow_force, advertised in the announce next
               to writable): a writable-but-not-forceable variable silently absorbs force ops. */
            if (t->kind == CAP_KIND_VARIABLE && t->writable && t->forceable){
                if (ui_pill(P, CLAY_STRING("Force"), FAM_SANS, WT_SEMI, FS_SMALL,
                            P->amber, P->panel2, P->border2, UISC(34)))
                    tt_form_force(app, t, n);
                if (ui_pill(P, CLAY_STRING("Unforce"), FAM_SANS, WT_SEMI, FS_SMALL,
                            P->dim, P->panel2, P->border2, UISC(34)) && app->cap)
                    cap_variable_unforce(app->cap, t->path);
            }
            if (ui_pill(P, ui_str(tt_send_verb(t)), FAM_SANS, WT_SEMI, FS_SMALL,
                        P->accent, P->accent_bg, P->border2, UISC(34)))
                tt_form_send(app, t, n);
        }
    }
}

/* The message composer, hosted in the Publish sidebar tab: a wrapping multi-line text box
   that grows in height with the text (then scrolls) plus a Send button. Enter sends,
   shift+Enter inserts a newline. Focused by default so you can just type. */
static void topics_composer(AppState *app, const Palette *P, const Topic *t){
    static CapSchema tt_form_schema;   /* the selected topic's advertised schema, per frame */
    int can, act;
    if (app->cap && cap_topic_schema(app->cap, t->path, &tt_form_schema)
        && tt_form_schema.inlined && tt_form_schema.n_fields > 0){
        topics_form(app, P, t, &tt_form_schema);   /* typed topic: the structured form */
        return;
    }
    app->form_focus = -1; app->form_n = 0;         /* free-text composer owns input */
    if (app->compose_topic != app->sel_topic){      /* switched topic: drop the stale draft */
        app->compose_topic = app->sel_topic;
        app->compose_len = 0; app->compose[0] = '\0';
        ui_tb_reset(&app->tb_compose);
    }
    /* focused by default (so you can just type) whenever no other box holds the focus */
    if (g_tb_focus == NULL && !app->adding_topic) ui_tb_focus(&app->tb_compose);
    can = app->compose_len > 0 && app->cap != NULL;

    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .childGap = UISCI(10),
                       .childAlignment = { .y = CLAY_ALIGN_Y_BOTTOM } } }) {
        act = ui_textbox(P, CLAY_ID("compose_box"), &app->tb_compose,
                         app->compose, UI_COMPOSE_MAX, &app->compose_len,
                         &(UiTextBoxOpts){ .fill_w = 1, .multiline = 1, .wrap = 1, .enter_submits = 1,
                                           .h_min = 38, .h_max = 150, .pad_x = 11, .pad_y = 10, .radius = 6,
                                           .placeholder = CLAY_STRING("Type a message, Enter to send"),
                                           .fam = FAM_MONO, .wt = WT_REG, .sz = FS_SMALL,
                                           .bg = P->panel2, .border = P->border, .border_focus = P->accent });
        if (ui_tb_focused(&app->tb_compose)) app->adding_topic = 0;   /* a click here exits add mode */
        if ((act & UI_TB_SUBMIT) && can) tt_do_send(app, t->path);
        if (act & UI_TB_CANCEL){
            app->compose_len = 0; app->compose[0] = '\0';
            ui_tb_reset(&app->tb_compose);
        }
        if (ui_pill(P, ui_str(tt_send_verb(t)), FAM_SANS, WT_SEMI, FS_SMALL,
                    can ? P->accent : P->faint, can ? P->accent_bg : P->panel2, P->border2, UISC(38)) && can)
            tt_do_send(app, t->path);
    }
}

/* =================================== center: task panel (our live call + observed runs) */

static CapTaskCall tt_task_call;                 /* the selected task's own-call state, per frame */
static CapTaskRun  tt_task_runs[CAP_TASK_RUNS];  /* observed runs off the @prg tap, per frame */

/* DartCallStatus display words (net_capture hands the numeric status; no DART types here) */
static const char *tt_call_status_word(int status){
    switch (status){
        case 0: return "OK";
        case 1: return "APP_ERROR";
        case 2: return "NO_HANDLER";
        case 3: return "TIMEOUT";
        case 4: return "PEER_LOST";
        case 5: return "CANCELLED";
        default: return "?";
    }
}

/* a small status chip (caption mono on a tinted pill) */
static void tt_task_chip(const Palette *P, Clay_String txt, Clay_Color fg, Clay_Color bg){
    CLAY({ .layout = { .padding = { .left = UISCI(8), .right = UISCI(8),
                                    .top = UISCI(3), .bottom = UISCI(3) } },
           .backgroundColor = bg, .cornerRadius = CLAY_CORNER_RADIUS(UISC(4)) }) {
        CLAY_TEXT(txt, CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_SEMI, FS_CAPTION),
                                          .textColor = fg, .wrapMode = CLAY_TEXT_WRAP_NONE }));
    }
}

/* a cancel pill, greyed (and inert) when the definition declared no_cancel or nothing runs */
static int tt_cancel_pill(const Palette *P, int enabled){
    return ui_pill(P, CLAY_STRING("Cancel"), FAM_SANS, WT_SEMI, FS_SMALL,
                   enabled ? P->amber : P->faint, P->panel2, P->border2, UISC(24)) && enabled;
}

/* one field of the latest progress update (name then value; nested members indented) */
static void tt_task_prg_field(const Palette *P, const CapMsgField *f, int idx){
    CLAY({ .id = CLAY_IDI("task_prg_field", (uint32_t)idx),
           .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(UISC(20)) },
                       .padding = { .left = UISCI(10 + f->depth * 12), .right = UISCI(10) },
                       .childGap = UISCI(10), .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_FIXED(UISC(110 - f->depth * 12)) } } }) {
            CLAY_TEXT(ui_str(f->name), CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_CAPTION),
                                                          .textColor = P->faint, .wrapMode = CLAY_TEXT_WRAP_NONE }));
        }
        CLAY_TEXT(ui_str(f->value), CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_SMALL),
                                                       .textColor = P->text, .wrapMode = CLAY_TEXT_WRAP_NONE }));
    }
}

/* one observed run: caller + call id, its newest progress line, update count, age, cancel */
static void tt_task_run_row(AppState *app, const Palette *P, const Topic *t,
                            const CapTaskRun *r, int idx){
    CLAY({ .id = CLAY_IDI("task_run", (uint32_t)idx),
           .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(UISC(28)) },
                       .padding = { .left = UISCI(10), .right = UISCI(4) }, .childGap = UISCI(10),
                       .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
           .backgroundColor = P->panel2, .cornerRadius = CLAY_CORNER_RADIUS(UISC(5)),
           .border = { .width = CLAY_BORDER_OUTSIDE(1), .color = P->border } }) {
        ui_dot(UISC(6), P->green, UI_NONE);
        CLAY_TEXT(ui_fmt("%s #%u", r->caller, r->call_id),
                  CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_SMALL),
                                     .textColor = P->text, .wrapMode = CLAY_TEXT_WRAP_NONE }));
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) },
                           .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
            CLAY_TEXT(tt_fit(r->preview, 60), CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_CAPTION),
                                                                 .textColor = P->dim, .wrapMode = CLAY_TEXT_WRAP_NONE }));
        }
        CLAY_TEXT(ui_fmt("%u upd", r->updates),
                  CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_CAPTION),
                                     .textColor = P->faint, .wrapMode = CLAY_TEXT_WRAP_NONE }));
        CLAY_TEXT(nf_age(r->age_s), CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_CAPTION),
                                                       .textColor = P->faint, .wrapMode = CLAY_TEXT_WRAP_NONE }));
        if (tt_cancel_pill(P, t->cancellable) && app->cap)
            cap_task_cancel_run(app->cap, t->path, r->provider_id, r->caller_lo, r->call_id);
    }
}

/* the task panel between the control row and the call log: our own call's live state
   (phase, decoded latest progress, terminal outcome, cancel) then every run observed on
   the broadcast @prg tap, each with its own cancel. Progress semantics are schema-defined,
   so updates render as decoded fields, never an invented percentage bar. */
static void topics_task_panel(AppState *app, const Palette *P, const Topic *t){
    int have  = app->cap ? cap_task_call_state(app->cap, t->path, &tt_task_call) : 0;
    int n_run = app->cap ? cap_task_runs(app->cap, t->path, tt_task_runs, CAP_TASK_RUNS) : 0;
    int i;
    if (have && tt_task_call.phase != CAP_TCALL_IDLE){
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .layoutDirection = CLAY_TOP_TO_BOTTOM,
                           .padding = CLAY_PADDING_ALL(UISC(8)), .childGap = UISCI(4) },
               .backgroundColor = P->panel, .cornerRadius = CLAY_CORNER_RADIUS(UISC(6)),
               .border = { .width = CLAY_BORDER_OUTSIDE(1), .color = P->border } }) {
            const CapTaskCall *c = &tt_task_call;
            CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .childGap = UISCI(10),
                               .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
                CLAY_TEXT(ui_fmt("your call #%u", c->call_id),
                          CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_SEMI, FS_SMALL),
                                             .textColor = P->text, .wrapMode = CLAY_TEXT_WRAP_NONE }));
                if (c->phase == CAP_TCALL_SENT)
                    tt_task_chip(P, CLAY_STRING("CALLING..."), P->amber, P->amber_bg);
                else if (c->phase == CAP_TCALL_RUNNING)
                    tt_task_chip(P, CLAY_STRING("RUNNING"), P->green, P->green_bg);
                else if (c->call_status == 0)
                    tt_task_chip(P, CLAY_STRING("OK"), P->green, P->green_bg);
                else
                    tt_task_chip(P, ui_str(tt_call_status_word(c->call_status)), P->red, P->red_bg);
                if (c->progress_count)
                    CLAY_TEXT(ui_fmt("%u update%s", c->progress_count, c->progress_count == 1 ? "" : "s"),
                              CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_CAPTION),
                                                 .textColor = P->dim, .wrapMode = CLAY_TEXT_WRAP_NONE }));
                CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
                if (tt_cancel_pill(P, t->cancellable
                                   && (c->phase == CAP_TCALL_SENT || c->phase == CAP_TCALL_RUNNING))
                    && app->cap)
                    cap_task_cancel_call(app->cap, t->path);
            }
            if (c->phase == CAP_TCALL_DONE && c->call_msg[0])   /* the terminal message */
                CLAY_TEXT(ui_fmt("%s", c->call_msg),
                          CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_CAPTION),
                                             .textColor = c->call_status ? P->amber : P->dim }));
            if (c->has_progress){                               /* the latest update, as fields */
                if (c->latest.decoded)
                    for (i = 0; i < c->latest.n_fields; i++)
                        tt_task_prg_field(P, &c->latest.fields[i], i);
                else {
                    char raw[CAP_MSG_PREVIEW + 1];
                    tt_sanitize(raw, (int)sizeof raw, c->latest.preview, c->latest.preview_len);
                    CLAY_TEXT(ui_fmt("%s", raw), CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_SMALL),
                                                                    .textColor = P->text }));
                }
            }
        }
    }
    /* every run the tap observes (the explorer's own call excluded: the card above is it) */
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .childGap = UISCI(8),
                       .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
        ui_section_label(P, ui_fmt("LIVE RUNS  %d", n_run));
        if (t->exclusive || t->multi || !t->cancellable)
            CLAY_TEXT(ui_fmt("%s%s%s", !t->cancellable ? "no cancel  " : "",
                             t->exclusive ? "exclusive  " : "", t->multi ? "multi" : ""),
                      CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_CAPTION),
                                         .textColor = P->faint, .wrapMode = CLAY_TEXT_WRAP_NONE }));
    }
    if (n_run == 0)
        CLAY_TEXT(tt_subscribed ? CLAY_STRING("no runs observed (progress shows here as it happens)")
                                : CLAY_STRING("subscribe to observe live runs"),
                  CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_CAPTION), .textColor = P->faint }));
    for (i = 0; i < n_run; i++) tt_task_run_row(app, P, t, &tt_task_runs[i], i);
}

static void topics_feed(AppState *app, const Palette *P){
    const Dataset *D = app->data;
    const Topic *t = (D && D->n_topics && app->sel_topic >= 0 && app->sel_topic < D->n_topics)
                     ? &D->topics[app->sel_topic] : NULL;
    int feed_state, i;

    /* refetch the selected topic's live feed for this frame (before drawing the controls) */
    tt_feed_n = 0; tt_subscribed = tt_sub_error = tt_sub_reliable = 0; tt_msgs = tt_drops = 0;
    tt_send_pending = 0;
    if (t && app->cap){
        tt_feed_n = cap_topic_feed(app->cap, t->path, tt_feed, CAP_FEED_MAX,
                                   &tt_subscribed, &tt_sub_error, &tt_sub_reliable, &tt_msgs, &tt_drops);
        tt_send_pending = cap_topic_send_pending(app->cap, t->path);
    }
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
            /* control row: subscribe toggle + live subscription status. A FUNCTION has no
               subscribe: replies are DIRECTED to their caller, so there is nothing to
               passively receive; the feed is this explorer's own call log, opened by the
               first Call. */
            CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .childGap = UISCI(12),
                               .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
                if (t->kind == CAP_KIND_FUNCTION){
                    tt_status_dot(P, tt_feed_n > 0 ? (tt_sub_error ? 2 : 1) : 0, tt_recent(t));
                    CLAY_TEXT(tt_feed_n > 0 ? ui_fmt("call log  \xC2\xB7  %u replies", tt_msgs)
                                            : CLAY_STRING("call log (replies are directed: only your own calls appear)"),
                              CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_CAPTION),
                                                 .textColor = P->dim, .wrapMode = CLAY_TEXT_WRAP_NONE }));
                } else {
                if (!tt_subscribed){
                    if (ui_pill(P, CLAY_STRING("Subscribe"), FAM_SANS, WT_SEMI, FS_SMALL,
                                P->accent, P->accent_bg, UI_NONE, UISC(28)) && app->cap)
                        cap_subscribe(app->cap, t->path, t->reliable_recommend > 0);
                } else {
                    if (ui_pill(P, CLAY_STRING("Unsubscribe"), FAM_SANS, WT_SEMI, FS_SMALL,
                                P->dim, P->panel2, P->border2, UISC(28)) && app->cap)
                        cap_unsubscribe(app->cap, t->path);
                }
                tt_status_dot(P, feed_state, tt_recent(t));
                CLAY_TEXT(tt_subscribed ? ui_fmt("subscribed %s  \xC2\xB7  %u msgs",
                                                 tt_sub_reliable ? "reliable" : "best-effort", tt_msgs)
                                        : CLAY_STRING("not subscribed"),
                          CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_CAPTION),
                                             .textColor = feed_state == 2 ? P->red : P->dim,
                                             .wrapMode = CLAY_TEXT_WRAP_NONE }));
                }
                if (tt_drops > 0)
                    CLAY_TEXT(ui_fmt("%u dropped", tt_drops),
                              CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_CAPTION), .textColor = P->red,
                                                 .wrapMode = CLAY_TEXT_WRAP_NONE }));
                /* variable: the newest value's FORCED flag, live from the value prefix */
                if (t->kind == CAP_KIND_VARIABLE && tt_feed_n > 0 && tt_feed[tt_feed_n - 1].forced)
                    CLAY_TEXT(CLAY_STRING("FORCED"),
                              CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_SEMI, FS_CAPTION),
                                                 .textColor = P->amber, .wrapMode = CLAY_TEXT_WRAP_NONE }));
                /* a send parked on the forming match (cap_pend_poll flushes it) */
                if (tt_send_pending)
                    CLAY_TEXT(CLAY_STRING("SENDING..."),
                              CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_SEMI, FS_CAPTION),
                                                 .textColor = P->amber, .wrapMode = CLAY_TEXT_WRAP_NONE }));
                /* Clear (right edge): drop this topic's stored messages and every count
                   derived from them. The subscription stays live, so the feed refills from
                   the next message. Only offered while there is something to clear. */
                if (tt_feed_n > 0 || tt_msgs > 0 || tt_drops > 0){
                    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
                    if (ui_pill(P, CLAY_STRING("Clear"), FAM_SANS, WT_SEMI, FS_SMALL,
                                P->dim, P->panel2, P->border2, UISC(28)) && app->cap){
                        cap_topic_clear(app->cap, t->path);
                        if (app->has_inspect_msg && !strcmp(app->inspect_msg_topic, t->path))
                            app->has_inspect_msg = 0;    /* the pinned sample was one of them */
                        /* this frame already copied the feed out: drop it here too, so the
                           table empties on the click rather than one frame later */
                        tt_feed_n = 0; tt_msgs = tt_drops = 0; tt_sub_error = 0;
                    }
                }
            }
            if (t->kind == CAP_KIND_TASK) topics_task_panel(app, P, t);
            if (tt_feed_n == 0){
                ui_placeholder(P, tt_call_kind(t->kind)
                                    ? CLAY_STRING("compose a call in the Publish tab to see requests and replies")
                                    : tt_subscribed ? CLAY_STRING("waiting for messages...")
                                                    : CLAY_STRING("subscribe to see live messages"));
            } else {
                /* columns come from a decoded sample of the topic's advertised schema (the
                   widest compatible one, exactly what the inspector shows via cap_topic_schema),
                   not merely the newest decoded sample: a subscriber publishing a SUBSET of the
                   schema must not narrow the available columns to its fewer fields. A FUNCTION
                   has TWO schemas and both become column groups (see the section comment); every
                   other kind has one. A schema-less feed shows one payload column. A group's
                   fields[] index maps 1:1 across every sample of that group's schema, so the
                   columns drive every row. */
                int   n_cols = 0, c, n_vis = 0, raw, n_grp = 1, tmpl;
                int   field_budget, time_budget, from_budget;
                float mono_adv = tt_name_w("00000000") / 8.0f;
                float table_w = 0.0f, field_w;
                if (mono_adv <= 0.5f) mono_adv = UISC(7);
                time_budget = (int)((UISC(TT_TIME_W) - UISC(TT_CELL_PADL + 2)) / mono_adv);
                if (time_budget < 1) time_budget = 1;

                /* the group schemas (both accessors zero *out when the topic advertises none).
                   Two groups only where the response shape genuinely DIFFERS: a function whose
                   reply repeats the request type is one set of columns, not two identical ones. */
                if (!app->cap || !cap_topic_schema(app->cap, t->path, &tt_feed_schema[0]))
                    memset(&tt_feed_schema[0], 0, sizeof tt_feed_schema[0]);
                if (!app->cap || !tt_call_kind(t->kind)
                    || !cap_topic_rsp_schema(app->cap, t->path, &tt_feed_schema[1])
                    || tt_feed_schema[1].hash == tt_feed_schema[0].hash)
                    memset(&tt_feed_schema[1], 0, sizeof tt_feed_schema[1]);
                else n_grp = 2;

                tmpl = tt_template(0, n_grp);
                if (tmpl >= 0) n_cols = tt_build_columns(&tt_feed[tmpl], tt_cols, 0, TT_MAX_COLS,
                                                         n_grp > 1 ? "req." : NULL, 0);
                if (n_grp > 1 && (tmpl = tt_template(1, n_grp)) >= 0)
                    n_cols = tt_build_columns(&tt_feed[tmpl], tt_cols, n_cols, TT_MAX_COLS,
                                              "rsp.", 1);

                /* seed the visible set when the topic changes; thereafter the right-click menu
                   owns it. Columns that appear LATER are seeded too (a function's response
                   columns only exist once the first reply decodes, and must not be born hidden),
                   which leaves the user's existing toggles alone. */
                if (app->col_vis_topic != app->sel_topic){
                    app->col_vis_topic = app->sel_topic;
                    if (ui_menu_is_open(tt_cols)) ui_menu_close();
                    app->n_col_vis  = 0;
                    tt_seeded_cols  = 0;
                }
                if (n_cols < tt_seeded_cols) tt_seeded_cols = n_cols;   /* the schema shrank */
                if (n_cols > tt_seeded_cols){
                    int per_grp = n_grp > 1 ? TT_GROUP_COLS : TT_TABLE_COLS, shown[2];
                    shown[0] = shown[1] = 0;
                    for (c = 0; c < n_cols; c++){
                        int g = tt_cols[c].grp;
                        if (c < tt_seeded_cols){
                            if (app_col_visible(app, tt_cols[c].name)) shown[g]++;
                        } else if (shown[g] < per_grp){
                            app_col_toggle(app, tt_cols[c].name);
                            shown[g]++;
                        }
                    }
                    tt_seeded_cols = n_cols;
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
                {   float fixed_w = (app->col_show_time ? UISC(TT_TIME_W) : 0.0f)
                                  + (app->col_show_from ? UISC(TT_FROM_W) : 0.0f);
                    field_w = (n_vis > 0 && table_w > fixed_w)
                              ? floorf((table_w - fixed_w) / (float)n_vis)
                              : UISC(TT_FIELD_W);
                }
                if (field_w < 1.0f) field_w = 1.0f;
                field_budget = (int)((field_w - UISC(TT_CELL_PADL + 2)) / mono_adv);
                if (field_budget < 1) field_budget = 1;
                from_budget = (int)((UISC(TT_FROM_W) - UISC(TT_CELL_PADL + 2)) / mono_adv);
                if (from_budget < 1) from_budget = 1;

                CLAY({ .id = CLAY_ID("topics_table"),
                       .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0) },
                                   .layoutDirection = CLAY_TOP_TO_BOTTOM },
                       .border = { .width = CLAY_BORDER_OUTSIDE(1), .color = P->border } }) {
                    tt_table_header(app, P, tt_vis, n_vis, field_w, field_budget, time_budget,
                                    from_budget, raw);
                    /* scrolling body, newest at the bottom; topics_feed_autoscroll keeps it pinned */
                    CLAY({ .id = CLAY_ID("topics_feed_scroll"),
                           .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0) },
                                       .layoutDirection = CLAY_TOP_TO_BOTTOM },
                           .clip = { .vertical = true, .childOffset = ui_scroll_offset(CLAY_ID("topics_feed_scroll")) } }) {
                        for (i = 0; i < tt_feed_n; i++)
                            tt_table_row(app, P, t->path, &tt_feed[i], i, tt_vis, n_vis, field_w,
                                         field_budget, time_budget, from_budget, t->kind, raw,
                                         n_grp);
                    }
                    ui_scrollbar(P, CLAY_ID("topics_feed_scroll"));
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
        if (Clay_Hovered() && g_pointer_pressed){ app->tab = TAB_NODES; app_select_node(app, app->data, nidx); }
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

/* the selected topic's advertised message schema (from any endpoint: pub or sub). The query
   prefilters each peer's interest by topic hash, so it is cheap even at many-thousand-topic
   scale and runs live every frame (no cache, no staleness). */
static CapSchema tt_schema[3];          /* [0] request/primary, [1] response, [2] a task's
                                           progress: Clay renders the frame's text AFTER the
                                           sections fill, so each needs its own strings */
static char      tt_dsl[8192];          /* scratch for the DSL handed to the clipboard */
static int       tt_dsl_copied_topic = -1;   /* which topic's Copy DSL was last clicked */
static uint32_t  tt_dsl_copied_ms;      /* when, for the brief "Copied" flash */
static void topic_schema_section(AppState *app, const Palette *P, const Topic *t,
                                 Clay_String label, int which){
    CapSchema *sc = &tt_schema[which];
    int have, i, copied;
    have = app->cap ? (which == 1 ? cap_topic_rsp_schema(app->cap, t->path, sc)
                     : which == 2 ? cap_topic_prg_schema(app->cap, t->path, sc)
                                  : cap_topic_schema(app->cap, t->path, sc)) : 0;
    copied = tt_dsl_copied_topic == app->sel_topic * 3 + which
          && (uint32_t)(g_now_ms - tt_dsl_copied_ms) < 1500u;
    /* header row: the label, and (inlined) a Copy DSL button that puts this section's
       schema (request, response or progress) as compile-ready DSL onto the clipboard */
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(UISC(22)) },
                       .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
        ui_section_label(P, label);
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
        if (have && sc->inlined && ui_copy_pill(P, copied)
            && cap_topic_schema_dsl(app->cap, t->path, which, tt_dsl, sizeof tt_dsl) > 0){
            SDL_SetClipboardText(tt_dsl);
            tt_dsl_copied_topic = app->sel_topic * 3 + which;
            tt_dsl_copied_ms    = g_now_ms;
        }
    }
    if (!have){
        CLAY_TEXT(which == 1 ? CLAY_STRING("none advertised (empty acknowledgement)")
                : which == 2 ? CLAY_STRING("none advertised (raw progress bytes)")
                             : CLAY_STRING("none advertised (raw bytes)"),
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
                    CLAY_TEXT(sc->inlined ? ui_str(sc->type_name) : CLAY_STRING("(hash only)"),
                              CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_SEMI, FS_SMALL),
                                                 .textColor = P->text, .wrapMode = CLAY_TEXT_WRAP_NONE }));
                    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
                    if (sc->inlined)
                        CLAY_TEXT(ui_fmt("%u B/msg", sc->msg_size),
                                  CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_CAPTION),
                                                     .textColor = P->dim, .wrapMode = CLAY_TEXT_WRAP_NONE }));
                }
                CLAY_TEXT(ui_fmt("id %08x%08x", (unsigned)(sc->hash >> 32), (unsigned)sc->hash),
                          CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_CAPTION),
                                             .textColor = P->faint, .wrapMode = CLAY_TEXT_WRAP_NONE }));
            }
            if (sc->inlined)   /* id space split: the sections coexist in one layout */
                for (i = 0; i < sc->n_fields; i++)
                    topic_schema_field_row(P, &sc->fields[i],
                                           i + which * CAP_SCHEMA_FIELDS);
            if (sc->inlined && sc->total_fields > sc->n_fields)
                CLAY_TEXT(ui_fmt("  +%d more fields", sc->total_fields - sc->n_fields),
                          CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_CAPTION), .textColor = P->faint }));
            if (!sc->inlined)
                CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .padding = CLAY_PADDING_ALL(UISC(9)) } }) {
                    CLAY_TEXT(CLAY_STRING("schema advertised by hash only (too large to inline)"),
                              CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_CAPTION), .textColor = P->faint }));
                }
        }
        if (sc->hash_conflict)
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
           .clip = { .vertical = true, .childOffset = ui_scroll_offset(CLAY_ID("topics_inspect_scroll")) } }) {
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .childGap = UISCI(6),
                           .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
            tt_kind_icon(P, t->kind);   /* hash for a plain topic, the type icon for an entity */
            CLAY_TEXT(ui_str(t->path), CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_BODY), .textColor = P->text }));
        }
        if (t->kind)   /* patterns layer: name the entity + channel role this topic plays */
            CLAY_TEXT(ui_str(tt_kind_label(t)),
                      CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_CAPTION), .textColor = P->dim }));
        if (t->kind == CAP_KIND_TASK){   /* the definition's declared attrs, as badges */
            CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .childGap = UISCI(6) } }) {
                if (t->cancellable) tt_task_chip(P, CLAY_STRING("cancellable"), P->green, P->green_bg);
                else                tt_task_chip(P, CLAY_STRING("no cancel"),   P->amber, P->amber_bg);
                if (t->exclusive)   tt_task_chip(P, CLAY_STRING("exclusive"),   P->dim,   P->panel2);
                if (t->multi)       tt_task_chip(P, CLAY_STRING("multi"),       P->dim,   P->panel2);
            }
        }

        ui_section_label(P, CLAY_STRING("QUALITY OF SERVICE"));
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .childGap = UISCI(8) } }) {
            topic_qos_cell(P, CLAY_STRING("Reliability"), tt_rel_word(t->reliable), ui_qos_color(P, t->reliable));
        }

        if (t->kind == CAP_KIND_FUNCTION){
            /* a function has two shapes: what a call sends and what the reply carries */
            topic_schema_section(app, P, t, CLAY_STRING("REQUEST SCHEMA"), 0);
            topic_schema_section(app, P, t, CLAY_STRING("RESPONSE SCHEMA"), 1);
        } else if (t->kind == CAP_KIND_TASK){
            /* a task adds the broadcast progress shape between request and response */
            topic_schema_section(app, P, t, CLAY_STRING("REQUEST SCHEMA"), 0);
            topic_schema_section(app, P, t, CLAY_STRING("PROGRESS SCHEMA"), 2);
            topic_schema_section(app, P, t, CLAY_STRING("RESPONSE SCHEMA"), 1);
        } else {
            topic_schema_section(app, P, t, CLAY_STRING("SCHEMA"), 0);
        }

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
    ui_scrollbar(P, CLAY_ID("topics_inspect_scroll"));
}

/* the Publish sidebar tab: the message composer (free-text, or the structured form for a
   typed topic) that used to live pinned under the feed, now hosted in the drawer */
static void topics_publish(AppState *app, const Palette *P, const Topic *t){
    CLAY({ .id = CLAY_ID("topics_publish_scroll"),
           .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0) },
                       .layoutDirection = CLAY_TOP_TO_BOTTOM, .padding = CLAY_PADDING_ALL(UISC(14)),
                       .childGap = UISCI(11) },
           .clip = { .vertical = true, .childOffset = ui_scroll_offset(CLAY_ID("topics_publish_scroll")) } }) {
        CLAY_TEXT(ui_str(t->path), CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_BODY), .textColor = P->text }));
        ui_section_label(P, tt_call_kind(t->kind)          ? CLAY_STRING("COMPOSE CALL")
                          : t->kind == CAP_KIND_VARIABLE ? CLAY_STRING("SET VALUE")
                          :                                CLAY_STRING("COMPOSE MESSAGE"));
        topics_composer(app, P, t);
    }
    ui_scrollbar(P, CLAY_ID("topics_publish_scroll"));
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
           .clip = { .vertical = true, .childOffset = ui_scroll_offset(CLAY_ID("topics_msg_scroll")) } }) {
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
            tt_msg_meta_row(P, m->stamped ? CLAY_STRING("Written") : CLAY_STRING("Arrived"),
                            nf_clock_ms(m->wall_us), P->dim);
            tt_msg_meta_row(P, CLAY_STRING("Size"), ui_fmt("%u B", m->len), P->dim);
            tt_msg_meta_row(P, CLAY_STRING("Type"),
                            m->decoded && m->type_name[0] ? ui_str(m->type_name) : CLAY_STRING("raw bytes"),
                            m->decoded && m->type_name[0] ? P->accent : P->dim);
            if (m->call_status)   /* a failed call: its status + response message */
                tt_msg_meta_row(P, CLAY_STRING("Failed"),
                                ui_fmt("%s", m->call_msg[0] ? m->call_msg : "?"), P->amber);
            else if (m->call_msg[0])   /* an OK reply carrying debug text */
                tt_msg_meta_row(P, CLAY_STRING("Message"), ui_fmt("%s", m->call_msg), P->text);
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
    ui_scrollbar(P, CLAY_ID("topics_msg_scroll"));
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
    tt_tree_menu(app, P);     /* the tree's right-click subscribe menu (floats above everything) */
    tt_filter_menu(app, P);   /* the category-filter checklist (floats above everything) */
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
