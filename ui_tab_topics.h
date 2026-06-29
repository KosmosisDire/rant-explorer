/* Topics tab: topic tree (264) + center feed (grow) + inline Inspect/Publish drawer
   (332, only when open). Wired to live discovery: the tree, each topic's reliability,
   and its publisher/subscriber node lists are real (from the announce interest blobs).
   The center FEED is live too: the explorer subscribes to the selected topic on demand
   (cap_subscribe) and shows messages as they arrive, newest at the bottom, auto-scrolling
   while the user is parked there. A topic's status light is its subscription state: green
   subscribed, red dropping/erroring, hollow grey not subscribed. Durability/history/deadline
   QoS ride the data plane out of band, so those stay placeholders; Publish is a read-only note.
   Requires ui_tree.h, ui_widgets.h, ui_model.h, ui_app.h, net_capture.h. */
#ifndef UI_TAB_TOPICS_H
#define UI_TAB_TOPICS_H

/* the selected topic's live feed, refetched from the capture each frame */
static CapFeedItem tt_feed[CAP_FEED_MAX];
static int         tt_feed_n, tt_subscribed, tt_sub_error;
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

static void topic_tree_row(AppState *app, const Palette *P, const TreeRow *row, int idx){
    const Dataset *D = app->data;
    int sel = row->has_topic && app->sel_topic >= 0 && app->sel_topic < D->n_topics
              && row->topic == &D->topics[app->sel_topic];
    CLAY({ .id = CLAY_IDI("tree_row", (uint32_t)idx),
           .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(UISC(29)) },
                       .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
           .backgroundColor = sel ? P->accent_bg : UI_NONE,
           .border = { .width = { sel ? UISCI(2) : 0, 0, 0, 0, 0 }, .color = P->accent } }) {
        /* a pure namespace (collapsible, no topic of its own) has nothing to select,
           so a click anywhere on the row toggles its collapse */
        if (row->is_branch && !row->has_topic && Clay_Hovered() && g_pointer_pressed)
            app_toggle_collapsed(app, row->path);
        /* caret zone: spans the indent + caret so a click anywhere left of the name
           toggles collapse; on a selectable branch the name itself still selects.
           The indent is left padding, so the chevron stays centered after it. */
        CLAY({ .id = CLAY_IDI("tree_caret", (uint32_t)idx),
               .layout = { .sizing = { .width = CLAY_SIZING_FIXED(UISC(10 + row->depth * 15 + 24)), .height = CLAY_SIZING_GROW(0) },
                           .padding = { .left = UISCI(10 + row->depth * 15) },
                           .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } } }) {
            if (row->is_branch){
                if (row->has_topic && Clay_Hovered() && g_pointer_pressed) app_toggle_collapsed(app, row->path);
                ui_icon(row->open ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, 12, P->dim);
            }
        }
        /* name: selectable when it's a topic, dim when a pure namespace */
        CLAY({ .id = CLAY_IDI("tree_name", (uint32_t)idx),
               .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0) },
                           .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
            if (row->has_topic && Clay_Hovered() && g_pointer_pressed){
                app->sel_topic = (int)(row->topic - D->topics);
                app->drawer_open = 1;
            }
            CLAY_TEXT(ui_str(row->name),
                      CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_SMALL),
                                         .textColor = row->has_topic ? P->text : P->dim,
                                         .wrapMode = CLAY_TEXT_WRAP_NONE }));
        }
        /* status dot: live subscription state (green subscribed, red dropping, grey not) */
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_FIXED(UISC(40)) },
                           .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } } }) {
            if (row->has_topic) tt_status_dot(P, row->topic->sub_state);
        }
    }
}

static void topics_tree(AppState *app, const Palette *P){
    const Dataset *D = app->data;
    int n = ui_tree_build(app), i;
    CLAY({ .id = CLAY_ID("topics_tree"),
           .layout = { .sizing = { .width = CLAY_SIZING_FIXED(UISC(264)), .height = CLAY_SIZING_GROW(0) },
                       .layoutDirection = CLAY_TOP_TO_BOTTOM },
           .backgroundColor = P->panel,
           .border = { .width = { 0, UISCI(1), 0, 0, 0 }, .color = P->border } }) {
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .padding = CLAY_PADDING_ALL(UISC(12)) } }) {
            ui_filter_box(P, CLAY_STRING("Filter topics..."));
        }
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
            ui_section_label(P, ui_fmt("%d TOPICS  \xC2\xB7  OBSERVER READ-ONLY", D ? D->n_topics : 0));
        }
    }
}

/* ================================================================= center: feed */

/* one received message: time + sender + size on top, the payload preview below */
static void tt_feed_row(const Palette *P, const CapFeedItem *m, int idx){
    char buf[CAP_MSG_PREVIEW + 1];
    tt_sanitize(buf, (int)sizeof buf, m->preview, m->preview_len);
    CLAY({ .id = CLAY_IDI("feed_row", (uint32_t)idx),
           .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .layoutDirection = CLAY_TOP_TO_BOTTOM,
                       .padding = { .left = UISCI(2), .right = UISCI(2), .top = UISCI(5), .bottom = UISCI(6) },
                       .childGap = UISCI(3) },
           .border = { .width = { 0, 0, 0, UISCI(1), 0 }, .color = P->border } }) {
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .childGap = UISCI(8),
                           .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
            CLAY_TEXT(ui_fmt("%8.2fs", m->t_s),
                      CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_CAPTION), .textColor = P->faint,
                                         .wrapMode = CLAY_TEXT_WRAP_NONE }));
            CLAY_TEXT(ui_str(m->sender),
                      CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_SEMI, FS_CAPTION), .textColor = P->dim,
                                         .wrapMode = CLAY_TEXT_WRAP_NONE }));
            CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
            CLAY_TEXT(ui_fmt("%u B", m->len),
                      CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_CAPTION), .textColor = P->faint,
                                         .wrapMode = CLAY_TEXT_WRAP_NONE }));
        }
        CLAY_TEXT(ui_fmt("%s", buf),
                  CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_SMALL), .textColor = P->text,
                                     .wrapMode = CLAY_TEXT_WRAP_WORDS }));
    }
}

static void topics_feed(AppState *app, const Palette *P){
    const Dataset *D = app->data;
    const Topic *t = (D && D->n_topics && app->sel_topic >= 0 && app->sel_topic < D->n_topics)
                     ? &D->topics[app->sel_topic] : NULL;
    int feed_state, i;

    /* refetch the selected topic's live feed for this frame (before drawing the controls) */
    tt_feed_n = 0; tt_subscribed = tt_sub_error = 0; tt_msgs = tt_drops = 0;
    if (t && app->cap)
        tt_feed_n = cap_topic_feed(app->cap, t->path, tt_feed, CAP_FEED_MAX,
                                   &tt_subscribed, &tt_sub_error, &tt_msgs, &tt_drops);
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
                            P->dim, P->panel2, P->border2, UISC(28)))
                    app->drawer_open = 1;
            }
            /* meta row: reliability + endpoint counts (all real) */
            CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .childGap = UISCI(16),
                               .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
                CLAY_TEXT(tt_rel_word(t->reliable),
                          CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_CAPTION),
                                             .textColor = ui_qos_color(P, t->reliable), .wrapMode = CLAY_TEXT_WRAP_NONE }));
                CLAY_TEXT(ui_fmt("%d publishers", t->n_pubs),
                          CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_CAPTION), .textColor = P->dim, .wrapMode = CLAY_TEXT_WRAP_NONE }));
                CLAY_TEXT(ui_fmt("%d subscribers", t->n_subs),
                          CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_CAPTION), .textColor = P->dim, .wrapMode = CLAY_TEXT_WRAP_NONE }));
            }
            /* control row: subscribe toggle + live subscription status */
            CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .childGap = UISCI(12),
                               .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
                if (!tt_subscribed){
                    if (ui_pill(P, CLAY_STRING("Subscribe"), FAM_SANS, WT_SEMI, FS_SMALL,
                                P->accent, P->accent_bg, UI_NONE, UISC(28)) && app->cap)
                        cap_subscribe(app->cap, t->path, t->reliable);
                } else {
                    if (ui_pill(P, CLAY_STRING("Unsubscribe"), FAM_SANS, WT_SEMI, FS_SMALL,
                                P->dim, P->panel2, P->border2, UISC(28)) && app->cap)
                        cap_unsubscribe(app->cap, t->path);
                }
                tt_status_dot(P, feed_state);
                CLAY_TEXT(tt_subscribed ? ui_fmt("subscribed %s  \xC2\xB7  %u msgs",
                                                 t->reliable ? "reliable" : "best-effort", tt_msgs)
                                        : CLAY_STRING("not subscribed"),
                          CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_CAPTION),
                                             .textColor = feed_state == 2 ? P->red : P->dim,
                                             .wrapMode = CLAY_TEXT_WRAP_NONE }));
                if (tt_drops > 0)
                    CLAY_TEXT(ui_fmt("%u dropped", tt_drops),
                              CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_CAPTION), .textColor = P->red,
                                                 .wrapMode = CLAY_TEXT_WRAP_NONE }));
            }
            ui_section_label(P, CLAY_STRING("MESSAGE FEED"));
            /* scrolling stream, newest at the bottom; topics_feed_autoscroll keeps it pinned */
            CLAY({ .id = CLAY_ID("topics_feed_scroll"),
                   .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0) },
                               .layoutDirection = CLAY_TOP_TO_BOTTOM },
                   .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() } }) {
                if (tt_feed_n == 0)
                    ui_placeholder(P, tt_subscribed ? CLAY_STRING("waiting for messages...")
                                                    : CLAY_STRING("subscribe to see live messages"));
                for (i = 0; i < tt_feed_n; i++) tt_feed_row(P, &tt_feed[i], i);
            }
        }
    }
}

/* ================================================================ right: drawer */

static void topics_drawer_header(AppState *app, const Palette *P){
    int insp = app->drawer_mode == DRW_INSPECT;
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(UISC(40)) },
                       .padding = { .left = UISCI(12), .right = UISCI(6) },
                       .childGap = UISCI(2), .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
           .border = { .width = { 0, 0, 0, UISCI(1), 0 }, .color = P->border } }) {
        if (ui_pill(P, CLAY_STRING("Inspect"), FAM_SANS, WT_SEMI, FS_SMALL,
                    insp ? P->accent : P->dim, insp ? P->accent_bg : P->panel, UI_NONE, UISC(28)))
            app->drawer_mode = DRW_INSPECT;
        if (ui_pill(P, CLAY_STRING("Publish"), FAM_SANS, WT_SEMI, FS_SMALL,
                    !insp ? P->accent : P->dim, !insp ? P->accent_bg : P->panel, UI_NONE, UISC(28)))
            app->drawer_mode = DRW_PUBLISH;
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
        if (ui_icon_button(P, ICON_X, 14, 26, P->dim, P->text))
            app->drawer_open = 0;
    }
}

static void topic_qos_cell(const Palette *P, Clay_String label, Clay_String value, Clay_Color vcol, int placeholder){
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(UISC(50)) },
                       .layoutDirection = CLAY_TOP_TO_BOTTOM, .padding = CLAY_PADDING_ALL(UISC(9)),
                       .childGap = UISCI(6) },
           .backgroundColor = P->panel2, .cornerRadius = CLAY_CORNER_RADIUS(UISC(6)),
           .border = { .width = CLAY_BORDER_OUTSIDE(1), .color = P->border } }) {
        CLAY_TEXT(label, CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_CAPTION),
                                            .textColor = P->faint, .wrapMode = CLAY_TEXT_WRAP_NONE }));
        CLAY_TEXT(value, CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_SMALL),
                                            .textColor = placeholder ? P->faint : vcol, .wrapMode = CLAY_TEXT_WRAP_NONE }));
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
            topic_qos_cell(P, CLAY_STRING("Reliability"), tt_rel_word(t->reliable), ui_qos_color(P, t->reliable), 0);
            topic_qos_cell(P, CLAY_STRING("Durability"), ui_str(ND_DASH), P->text, 1);
        }
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .childGap = UISCI(8) } }) {
            topic_qos_cell(P, CLAY_STRING("History"), ui_str(ND_DASH), P->text, 1);
            topic_qos_cell(P, CLAY_STRING("Deadline"), ui_str(ND_DASH), P->text, 1);
        }

        ui_section_label(P, ui_fmt("PUBLISHERS  %d", t->n_pubs));
        if (t->n_pubs == 0)
            CLAY_TEXT(CLAY_STRING("none"), CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_SMALL), .textColor = P->faint }));
        for (i = 0; i < t->n_pubs; i++) topic_node_chip(app, P, t->pubs[i], "pub_chip", i);

        ui_section_label(P, ui_fmt("SUBSCRIBERS  %d", t->n_subs));
        if (t->n_subs == 0)
            CLAY_TEXT(CLAY_STRING("none"), CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_SMALL), .textColor = P->faint }));
        for (i = 0; i < t->n_subs; i++) topic_node_chip(app, P, t->subs[i], "sub_chip", i);

        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .padding = CLAY_PADDING_ALL(UISC(11)) },
               .backgroundColor = P->panel2, .cornerRadius = CLAY_CORNER_RADIUS(UISC(6)),
               .border = { .width = CLAY_BORDER_OUTSIDE(1), .color = P->border } }) {
            CLAY_TEXT(CLAY_STRING("Durability, history and deadline are per-endpoint QoS carried on the "
                                  "data plane, not advertised in discovery, so they are not observable here. "
                                  "Reliability is (it gates matching)."),
                      CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_CAPTION), .textColor = P->faint }));
        }
    }
}

static void topics_publish(const Palette *P){
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0) },
                       .layoutDirection = CLAY_TOP_TO_BOTTOM, .padding = CLAY_PADDING_ALL(UISC(16)),
                       .childGap = UISCI(12), .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } } }) {
        CLAY_TEXT(CLAY_STRING("Publishing needs a data-plane endpoint. The explorer is a passive discovery "
                              "observer: it joins no topic and sends no data, so the composer is disabled."),
                  CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_SMALL), .textColor = P->faint }));
    }
}

static void topics_drawer(AppState *app, const Palette *P){
    const Dataset *D = app->data;
    const Topic *t = (D && D->n_topics && app->sel_topic >= 0 && app->sel_topic < D->n_topics)
                     ? &D->topics[app->sel_topic] : NULL;
    CLAY({ .id = CLAY_ID("topics_drawer"),
           .layout = { .sizing = { .width = CLAY_SIZING_FIXED(UISC(332)), .height = CLAY_SIZING_GROW(0) },
                       .layoutDirection = CLAY_TOP_TO_BOTTOM },
           .backgroundColor = P->panel,
           .border = { .width = { UISCI(1), 0, 0, 0, 0 }, .color = P->border } }) {
        topics_drawer_header(app, P);
        if (!t)
            ui_placeholder(P, CLAY_STRING("no topic selected"));
        else if (app->drawer_mode == DRW_INSPECT)
            topics_inspect(app, P, t);
        else
            topics_publish(P);
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
