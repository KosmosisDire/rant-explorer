/* Nodes tab: a node list on the left (grouped by machine = advertised IP) and a
   scrolling detail pane. Both render from the live Dataset (fed by net_capture via
   ui_data). The detail pane groups a peer's info by where it comes from: DISCOVERY
   (what the discovery protocol carries: locator, cadence, observer metrics), ANNOUNCE
   METADATA (the transport overlay our node decoded: fragment size + pub/sub interest),
   and NOT OBSERVABLE (a peer's own runtime internals -- CPU, memory, msg counts, real
   uptime, its AckNack counters, the peer table it holds -- which never ride the wire,
   drawn as a dim em dash placeholder). Requires ui_widgets.h, ui_model.h, ui_app.h. */
#ifndef UI_TAB_NODES_H
#define UI_TAB_NODES_H

#define ND_DASH "\xE2\x80\x94"   /* em dash: an unavailable / not-yet-observable value */

/* ---- value formatters (return into the per-frame string pool) ---- */
static Clay_String nf_dur(double s){          /* "9s" / "3m 24s" / "4h 12m" */
    if (s < 0) return ui_str(ND_DASH);
    if (s < 60)   return ui_fmt("%ds", (int)(s + 0.5));
    if (s < 3600) return ui_fmt("%dm %02ds", (int)s / 60, (int)s % 60);
    return ui_fmt("%dh %02dm", (int)s / 3600, ((int)s % 3600) / 60);
}
static Clay_String nf_age(double s){          /* "0.4s ago" / "12s ago" / "3m ago" */
    if (s < 0) return ui_str(ND_DASH);
    if (s < 10)   return ui_fmt("%.1fs ago", s);
    if (s < 60)   return ui_fmt("%ds ago", (int)s);
    if (s < 3600) return ui_fmt("%dm ago", (int)s / 60);
    return ui_fmt("%dh ago", (int)s / 3600);
}
static Clay_String nf_grp(long v){            /* thousands-separated int; <0 -> dash */
    char tmp[24], out[32]; int len, i, j;
    if (v < 0) return ui_str(ND_DASH);
    len = snprintf(tmp, sizeof tmp, "%ld", v);
    for (i = 0, j = 0; i < len && j < (int)sizeof out - 1; i++){
        if (i > 0 && (len - i) % 3 == 0) out[j++] = ',';
        out[j++] = tmp[i];
    }
    out[j] = '\0';
    return ui_fmt("%s", out);
}
static Clay_String nf_bytes(long b){          /* "1408 B" / "182 KB"; <0 -> dash */
    if (b < 0) return ui_str(ND_DASH);
    if (b < 1024)            return ui_fmt("%ld B", b);
    if (b < 1024L * 1024)    return ui_fmt("%.0f KB", b / 1024.0);
    return ui_fmt("%.1f MB", b / (1024.0 * 1024.0));
}
static Clay_String nd_state_word(NodeState s){
    return s == NODE_ALIVE ? CLAY_STRING("ALIVE")
         : s == NODE_JOINING ? CLAY_STRING("JOINING")
         : s == NODE_DROPPED ? CLAY_STRING("DROPPED")
         : CLAY_STRING("GONE");
}

/* ===================================================================== left list */

static void node_group_header(const Palette *P, const Machine *m){
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(UISC(30)) },
                       .padding = { .left = UISCI(12), .right = UISCI(10), .top = UISCI(8) },
                       .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
        ui_section_label(P, ui_str(m->ip[0] ? m->ip : "(unknown host)"));
    }
}

static void node_list_row(AppState *app, const Palette *P, const Node *nd, int idx){
    int sel  = app->sel_node == idx;
    int gone = nd->state == NODE_GONE;
    CLAY({ .id = CLAY_IDI("node_row", (uint32_t)idx),
           .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(UISC(36)) },
                       .padding = { .left = UISCI(12), .right = UISCI(10) },
                       .childGap = UISCI(9), .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
           .backgroundColor = sel ? P->accent_bg : UI_NONE,
           .cornerRadius = CLAY_CORNER_RADIUS(UISC(5)),
           .border = { .width = { sel ? UISCI(2) : 0, 0, 0, 0, 0 }, .color = P->accent } }) {
        if (Clay_Hovered() && g_pointer_pressed) app->sel_node = idx;
        ui_dot(UISC(8), ui_state_color(P, nd->state), UI_NONE);
        CLAY_TEXT(ui_str(nd->name),
                  CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, sel ? WT_SEMI : WT_REG, FS_BODY),
                                     .textColor = gone ? P->faint : P->text, .wrapMode = CLAY_TEXT_WRAP_NONE }));
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}   /* spacer */
        if (nd->state == NODE_JOINING)
            CLAY_TEXT(CLAY_STRING("joining"), CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_SMALL),
                                                                .textColor = P->amber, .wrapMode = CLAY_TEXT_WRAP_NONE }));
        else if (nd->state == NODE_DROPPED)
            CLAY_TEXT(CLAY_STRING("dropped"), CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_SMALL),
                                                                .textColor = P->amber, .wrapMode = CLAY_TEXT_WRAP_NONE }));
        else if (gone)
            CLAY_TEXT(CLAY_STRING("gone"), CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_SMALL),
                                                            .textColor = P->gray, .wrapMode = CLAY_TEXT_WRAP_NONE }));
    }
}

static void nodes_list(AppState *app, const Palette *P){
    const Dataset *D = app->data;
    CLAY({ .id = CLAY_ID("nodes_list"),
           .layout = { .sizing = { .width = CLAY_SIZING_FIXED(UISC(264)), .height = CLAY_SIZING_GROW(0) },
                       .layoutDirection = CLAY_TOP_TO_BOTTOM },
           .backgroundColor = P->panel,
           .border = { .width = { 0, UISCI(1), 0, 0, 0 }, .color = P->border } }) {
        CLAY({ .id = CLAY_ID("nodes_list_scroll"),
               .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0) },
                           .layoutDirection = CLAY_TOP_TO_BOTTOM, .padding = { .bottom = UISCI(12) } },
               .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() } }) {
            if (!D || D->n_nodes == 0){
                ui_placeholder(P, CLAY_STRING("no nodes discovered"));
            } else {
                int mi, ni, i, n_sel, sel[CAP_SNAP_NODES];
                for (mi = 0; mi < D->n_machines; mi++){
                    node_group_header(P, &D->machines[mi]);
                    n_sel = 0;
                    for (ni = 0; ni < D->n_nodes; ni++)
                        if (D->nodes[ni].machine == mi) sel[n_sel++] = ni;
                    /* alphabetize this machine's rows by name (n_sel is tiny: insertion sort) */
                    for (i = 1; i < n_sel; i++){
                        int key = sel[i], j = i - 1;
                        while (j >= 0 && strcmp(D->nodes[sel[j]].name, D->nodes[key].name) > 0){
                            sel[j + 1] = sel[j]; j--;
                        }
                        sel[j + 1] = key;
                    }
                    for (i = 0; i < n_sel; i++) node_list_row(app, P, &D->nodes[sel[i]], sel[i]);
                }
            }
        }
    }
}

/* =================================================================== detail pane */

static void node_stat(const Palette *P, Clay_String label, Clay_String value, int placeholder){
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(UISC(60)) },
                       .layoutDirection = CLAY_TOP_TO_BOTTOM, .padding = CLAY_PADDING_ALL(UISC(11)),
                       .childGap = UISCI(7) },
           .backgroundColor = P->panel2, .cornerRadius = CLAY_CORNER_RADIUS(UISC(6)),
           .border = { .width = CLAY_BORDER_OUTSIDE(1), .color = P->border } }) {
        CLAY_TEXT(label, CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_CAPTION),
                                            .textColor = P->faint, .wrapMode = CLAY_TEXT_WRAP_NONE }));
        CLAY_TEXT(value, CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_VALUE),
                                            .textColor = placeholder ? P->faint : P->text, .wrapMode = CLAY_TEXT_WRAP_NONE }));
    }
}
/* an empty cell so the last stat row keeps the same card width as a full row */
static void node_stat_gap(void){
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
}

static void node_kv_cell(const Palette *P, Clay_String label, Clay_String value, int placeholder, int alert){
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0) },
                       .padding = { .left = UISCI(12), .right = UISCI(12) },
                       .childGap = UISCI(8), .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
        CLAY_TEXT(label, CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_SMALL),
                                            .textColor = P->dim, .wrapMode = CLAY_TEXT_WRAP_NONE }));
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}   /* spacer */
        CLAY_TEXT(value, CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_SMALL),
                          .textColor = placeholder ? P->faint : (alert ? P->red : P->text), .wrapMode = CLAY_TEXT_WRAP_NONE }));
    }
}
static void node_kv_row(const Palette *P,
                        Clay_String l1, Clay_String v1, int ph1, int al1,
                        Clay_String l2, Clay_String v2, int ph2, int al2){
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(UISC(34)) } },
           .border = { .width = { 0, 0, 0, UISCI(1), 0 }, .color = P->border } }) {
        node_kv_cell(P, l1, v1, ph1, al1);
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_FIXED(UISC(1)), .height = CLAY_SIZING_GROW(0) } },
               .backgroundColor = P->border }) {}
        node_kv_cell(P, l2, v2, ph2, al2);
    }
}
/* a single full-width kv row (no divider) for an odd trailing entry */
static void node_kv_single(const Palette *P, Clay_String label, Clay_String value, int placeholder, int alert){
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(UISC(34)) } },
           .border = { .width = { 0, 0, 0, UISCI(1), 0 }, .color = P->border } }) {
        node_kv_cell(P, label, value, placeholder, alert);
    }
}

/* one Publishes/Subscribes entry: reliability dot + topic path + qos word */
static void node_endpoint_row(const Palette *P, Clay_String path, int reliable){
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(UISC(28)) },
                       .padding = { .left = UISCI(10), .right = UISCI(12) },
                       .childGap = UISCI(9), .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
           .backgroundColor = P->panel2, .cornerRadius = CLAY_CORNER_RADIUS(UISC(5)),
           .border = { .width = CLAY_BORDER_OUTSIDE(1), .color = P->border } }) {
        ui_dot(UISC(7), ui_qos_color(P, reliable), UI_NONE);
        CLAY_TEXT(path, CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_SMALL),
                                           .textColor = P->text, .wrapMode = CLAY_TEXT_WRAP_NONE }));
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}   /* spacer */
        CLAY_TEXT(reliable ? CLAY_STRING("reliable") : CLAY_STRING("best-effort"),
                  CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_CAPTION),
                                     .textColor = ui_qos_color(P, reliable), .wrapMode = CLAY_TEXT_WRAP_NONE }));
    }
}

static void node_endpoint_list(const Palette *P, const Dataset *D,
                               const int *topic_idx, const unsigned char *rel, int n){
    int i;
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) },
                       .layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = UISCI(5) } }) {
        if (n == 0){
            CLAY_TEXT(CLAY_STRING("none"), CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_SMALL),
                                                             .textColor = P->faint, .wrapMode = CLAY_TEXT_WRAP_NONE }));
        }
        for (i = 0; i < n; i++){
            const Topic *t = &D->topics[topic_idx[i]];
            node_endpoint_row(P, ui_str(t->path), rel[i]);
        }
    }
}

static void nodes_detail(AppState *app, const Palette *P){
    const Dataset *D = app->data;

    /* NB: never `return` out of a CLAY{} block -- the macro is a for-loop that
       closes the element in its increment clause, so an early return unbalances
       Clay's element stack and crashes EndLayout. Use if/else instead. */
    CLAY({ .id = CLAY_ID("nodes_detail"),
           .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0) } },
           .backgroundColor = P->bg }) {
      if (!D || D->n_nodes == 0 || app->sel_node < 0 || app->sel_node >= D->n_nodes){
        ui_placeholder(P, CLAY_STRING("select a node"));
      } else {
        const Node *nd = &D->nodes[app->sel_node];

        CLAY({ .id = CLAY_ID("nodes_detail_scroll"),
               .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0) },
                           .layoutDirection = CLAY_TOP_TO_BOTTOM, .padding = CLAY_PADDING_ALL(UISC(18)),
                           .childGap = UISCI(14) },
               .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() } }) {

            /* header: name + state word (the state dot lives in the node list; the
               state word is color-coded, so it carries the state on its own). The
               small caps are box-centered high against the big title, so nudge them
               down with a top padding to sit at the title's optical center. */
            CLAY({ .layout = { .childGap = UISCI(10), .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
                CLAY_TEXT(ui_str(nd->name), CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_BOLD, FS_HERO),
                                                               .textColor = P->text, .wrapMode = CLAY_TEXT_WRAP_NONE }));
                CLAY({ .layout = { .padding = { .top = UISCI(6) } } }) {
                    CLAY_TEXT(nd_state_word(nd->state),
                              CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_SEMI, FS_CAPTION), .letterSpacing = 1,
                                                 .textColor = ui_state_color(P, nd->state), .wrapMode = CLAY_TEXT_WRAP_NONE }));
                }
            }

            /* ========= DISCOVERY: what the discovery protocol itself carries ========= */
            ui_section_label(P, CLAY_STRING("DISCOVERY"));
            CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .layoutDirection = CLAY_TOP_TO_BOTTOM },
                   .backgroundColor = P->panel2, .cornerRadius = CLAY_CORNER_RADIUS(UISC(6)),
                   .border = { .width = CLAY_BORDER_OUTSIDE(1), .color = P->border } }) {
                node_kv_row(P, CLAY_STRING("Unicast locator"), ui_str(nd->disc.unicast),         0, 0,
                               CLAY_STRING("Discovery group"), ui_str(nd->disc.discovery_group), 0, 0);
                node_kv_row(P, CLAY_STRING("Announce"),      ui_fmt("every %.1f s", nd->disc.announce_period_s), 0, 0,
                               CLAY_STRING("Last announce"), nf_age(nd->disc.last_announce_age_s), 0, 0);
                node_kv_row(P, CLAY_STRING("Liveliness lease"), ui_fmt("%.1f s", nd->disc.lease_s), 0, 0,
                               CLAY_STRING("Observed for"),     nf_dur(nd->observed_s),             0, 0);
                node_kv_single(P, CLAY_STRING("Announce updates"), nf_grp((long)nd->updates), 0, 0);
            }

            /* ===== ANNOUNCE METADATA: the transport overlay, decoded by our node ===== */
            ui_section_label(P, CLAY_STRING("ANNOUNCE METADATA"));
            CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .layoutDirection = CLAY_TOP_TO_BOTTOM },
                   .backgroundColor = P->panel2, .cornerRadius = CLAY_CORNER_RADIUS(UISC(6)),
                   .border = { .width = CLAY_BORDER_OUTSIDE(1), .color = P->border } }) {
                node_kv_row(P, CLAY_STRING("Fragment size"), nf_bytes(nd->disc.frag_size_bytes), 0, 0,
                               CLAY_STRING("Announce blob"), nf_bytes(nd->disc.blob_bytes),      0, 0);
                node_kv_single(P, CLAY_STRING("Transport"), ui_str(nd->disc.transport), 0, 0);
            }
            ui_section_label(P, ui_fmt("PUBLISHES  %d", nd->n_pubs));
            node_endpoint_list(P, D, nd->pubs, nd->pub_rel, nd->n_pubs);
            ui_section_label(P, ui_fmt("SUBSCRIBES  %d", nd->n_subs));
            node_endpoint_list(P, D, nd->subs, nd->sub_rel, nd->n_subs);

            /* ===== NOT OBSERVABLE: a peer's own runtime internals, never on the wire ===== */
            ui_section_label(P, CLAY_STRING("NOT OBSERVABLE"));
            CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .padding = CLAY_PADDING_ALL(UISC(12)) },
                   .backgroundColor = P->panel2, .cornerRadius = CLAY_CORNER_RADIUS(UISC(6)),
                   .border = { .width = CLAY_BORDER_OUTSIDE(1), .color = P->border } }) {
                CLAY_TEXT(CLAY_STRING("A peer's own runtime internals. Neither discovery nor the announce "
                                      "metadata carries them, and they include the peer table the node "
                                      "holds itself, so they show as a placeholder."),
                          CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_SMALL), .textColor = P->faint }));
            }
            CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .childGap = UISCI(10) } }) {
                node_stat(P, CLAY_STRING("Uptime"),    nf_dur(nd->uptime_s),        1);
                node_stat(P, CLAY_STRING("Heartbeat"), nf_age(nd->heartbeat_age_s), 1);
                node_stat(P, CLAY_STRING("CPU"),       nd->cpu_pct  < 0 ? ui_str(ND_DASH) : ui_fmt("%d%%", nd->cpu_pct), 1);
                node_stat(P, CLAY_STRING("Memory"),    nf_bytes(nd->mem_bytes),     1);
            }
            CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .childGap = UISCI(10) } }) {
                node_stat(P, CLAY_STRING("Msgs out"),  nf_grp(nd->msgs_sent), 1);
                node_stat(P, CLAY_STRING("Msgs in"),   nf_grp(nd->msgs_recv), 1);
                node_stat_gap();
                node_stat_gap();
            }
            CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .layoutDirection = CLAY_TOP_TO_BOTTOM },
                   .backgroundColor = P->panel2, .cornerRadius = CLAY_CORNER_RADIUS(UISC(6)),
                   .border = { .width = CLAY_BORDER_OUTSIDE(1), .color = P->border } }) {
                node_kv_row(P, CLAY_STRING("Max message"),  ui_str(ND_DASH), 1, 0,
                               CLAY_STRING("Announces sent"), nf_grp(nd->disc.announces_sent), 1, 0);
                node_kv_row(P, CLAY_STRING("Fragments TX"), nf_grp(nd->disc.frags_tx), 1, 0,
                               CLAY_STRING("AckNacks RX"),  nf_grp(nd->disc.acknacks_rx), 1, 0);
                node_kv_row(P, CLAY_STRING("NACKs (retransmit)"), nf_grp(nd->disc.nacks_rx), 1, nd->disc.nacks_rx > 100,
                               CLAY_STRING("Known peers"), ui_str(ND_DASH), 1, 0);
            }
        }
      }
    }
}

static void nodes_tab(AppState *app, const Palette *P){
    nodes_list(app, P);
    nodes_detail(app, P);
}

#endif /* UI_TAB_NODES_H */
