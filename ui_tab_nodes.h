/* Nodes tab: a node list on the left (grouped by machine = advertised IP) and a
   scrolling detail pane. Both render from the live Dataset (fed by net_capture via
   ui_data). The detail pane groups a peer's info by where it comes from: DISCOVERY
   (what the discovery protocol carries: locator, observer metrics) and ANNOUNCE
   METADATA (the transport overlay our node decoded: fragment size + pub/sub interest).
   A peer's own runtime internals (CPU, memory, msg counts, uptime, its AckNack
   counters, the peer table it holds) never ride the wire, so they are not shown at
   all rather than as a placeholder. Requires ui_widgets.h, ui_model.h, ui_app.h. */
#ifndef UI_TAB_NODES_H
#define UI_TAB_NODES_H

#include <time.h>   /* localtime: the log sidebar's wall-clock timestamps */

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
static Clay_String nf_bytesu(uint64_t b){     /* the u64 twin (RSS can pass LONG_MAX on Windows) */
    if (b < 1024u)                  return ui_fmt("%u B", (unsigned)b);
    if (b < 1024u * 1024u)          return ui_fmt("%.0f KB", (double)b / 1024.0);
    if (b < 1024u * 1024u * 1024u)  return ui_fmt("%.1f MB", (double)b / (1024.0 * 1024.0));
    return ui_fmt("%.2f GB", (double)b / (1024.0 * 1024.0 * 1024.0));
}
static Clay_String nf_clock(uint64_t wall_us){   /* the sender's wall clock as local HH:MM:SS */
    time_t t = (time_t)(wall_us / 1000000u);
    struct tm *lt = wall_us ? localtime(&t) : NULL;
    if (!lt) return ui_str(ND_DASH);
    return ui_fmt("%02d:%02d:%02d", lt->tm_hour, lt->tm_min, lt->tm_sec);
}
static Clay_String nf_clock_ms(uint64_t wall_us){   /* the same, to the millisecond */
    time_t t = (time_t)(wall_us / 1000000u);
    struct tm *lt = wall_us ? localtime(&t) : NULL;
    if (!lt) return ui_str(ND_DASH);
    return ui_fmt("%02d:%02d:%02d.%03d", lt->tm_hour, lt->tm_min, lt->tm_sec,
                  (int)(wall_us % 1000000u) / 1000);
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
        ui_scrollbar(P, CLAY_ID("nodes_list_scroll"));
    }
}

/* =================================================================== detail pane */

static void node_kv_cell(const Palette *P, Clay_String label, Clay_String value){
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0) },
                       .padding = { .left = UISCI(12), .right = UISCI(12) },
                       .childGap = UISCI(8), .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
        CLAY_TEXT(label, CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_SMALL),
                                            .textColor = P->dim, .wrapMode = CLAY_TEXT_WRAP_NONE }));
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}   /* spacer */
        CLAY_TEXT(value, CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_SMALL),
                                            .textColor = P->text, .wrapMode = CLAY_TEXT_WRAP_NONE }));
    }
}
static void node_kv_row(const Palette *P, Clay_String l1, Clay_String v1, Clay_String l2, Clay_String v2){
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(UISC(34)) } },
           .border = { .width = { 0, 0, 0, UISCI(1), 0 }, .color = P->border } }) {
        node_kv_cell(P, l1, v1);
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_FIXED(UISC(1)), .height = CLAY_SIZING_GROW(0) } },
               .backgroundColor = P->border }) {}
        node_kv_cell(P, l2, v2);
    }
}
/* a single full-width kv row (no divider) for an odd trailing entry */
static void node_kv_single(const Palette *P, Clay_String label, Clay_String value){
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(UISC(34)) } },
           .border = { .width = { 0, 0, 0, UISCI(1), 0 }, .color = P->border } }) {
        node_kv_cell(P, label, value);
    }
}

/* the @dart/meta counter row for a topic name, or NULL if this snapshot lacks it */
static const CapMetaTopic *node_meta_find(const CapMetaStats *ms, const char *path){
    int i;
    if (!ms) return NULL;
    for (i = 0; i < ms->n_topic_rows; i++)
        if (!strcmp(ms->topic_rows[i].name, path)) return &ms->topic_rows[i];
    return NULL;
}

/* one Publishes/Subscribes entry: reliability dot + topic path + qos word, with the
   node's own @dart/meta counters folded in when a snapshot is fresh (tx on a publish
   row, rx on a subscribe row; consumer-queue drops in red). tr is NULL with no snapshot. */
static void node_endpoint_row(const Palette *P, Clay_String path, int reliable,
                              const CapMetaTopic *tr, int is_pub){
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(UISC(28)) },
                       .padding = { .left = UISCI(10), .right = UISCI(12) },
                       .childGap = UISCI(9), .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
           .backgroundColor = P->panel2, .cornerRadius = CLAY_CORNER_RADIUS(UISC(5)),
           .border = { .width = CLAY_BORDER_OUTSIDE(1), .color = P->border } }) {
        ui_dot(UISC(7), ui_qos_color(P, reliable), UI_NONE);
        CLAY_TEXT(path, CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_SMALL),
                                           .textColor = P->text, .wrapMode = CLAY_TEXT_WRAP_NONE }));
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}   /* spacer */
        if (tr){
            if (tr->drops)
                CLAY_TEXT(ui_fmt("drops %u", tr->drops),
                          CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_CAPTION),
                                             .textColor = P->red, .wrapMode = CLAY_TEXT_WRAP_NONE }));
            CLAY_TEXT(is_pub ? ui_fmt("tx %llu", (unsigned long long)tr->tx_msgs)
                             : ui_fmt("rx %llu", (unsigned long long)tr->rx_msgs),
                      CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_CAPTION),
                                         .textColor = P->dim, .wrapMode = CLAY_TEXT_WRAP_NONE }));
        }
        CLAY_TEXT(reliable ? CLAY_STRING("reliable") : CLAY_STRING("best-effort"),
                  CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_CAPTION),
                                     .textColor = ui_qos_color(P, reliable), .wrapMode = CLAY_TEXT_WRAP_NONE }));
    }
}

static void node_endpoint_list(const Palette *P, const Dataset *D,
                               const int *topic_indices, const unsigned char *rel, int n,
                               const CapMetaStats *ms, int is_pub){
    int i;
    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) },
                       .layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = UISCI(5) } }) {
        if (n == 0){
            CLAY_TEXT(CLAY_STRING("none"), CLAY_TEXT_CONFIG({ UI_FONT(FAM_SANS, WT_REG, FS_SMALL),
                                                             .textColor = P->faint, .wrapMode = CLAY_TEXT_WRAP_NONE }));
        }
        for (i = 0; i < n; i++){
            const Topic *t = &D->topics[topic_indices[i]];
            node_endpoint_row(P, ui_str(t->path), rel[i], node_meta_find(ms, t->path), is_pub);
        }
    }
}

/* the RUNTIME / PROCESS sections of the detail pane, fed by the 1 Hz @dart/meta poll.
   Everything here is the NODE'S OWN report (its allocator, its counters, its process),
   unlike the wire-derived sections above. The poll's per-topic counters are folded into
   the PUBLISHES / SUBSCRIBES lists instead of shown here. ms is the shared snapshot;
   watching = a poll is aimed here, fresh = it answered for this node. */
static void nodes_runtime_sections(const Palette *P, const Node *nd,
                                   const CapMetaStats *ms, int watching, int fresh){
    ui_section_label(P, CLAY_STRING("RUNTIME  (@dart/meta)"));
    if (!fresh){
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .layoutDirection = CLAY_TOP_TO_BOTTOM },
               .backgroundColor = P->panel2, .cornerRadius = CLAY_CORNER_RADIUS(UISC(6)),
               .border = { .width = CLAY_BORDER_OUTSIDE(1), .color = P->border } }) {
            node_kv_single(P, CLAY_STRING("Status"),
                           (watching && ms->failing >= 2) ? CLAY_STRING("no @dart/meta response")
                         : nd->state == NODE_GONE          ? CLAY_STRING("node gone")
                                                           : CLAY_STRING("querying..."));
        }
    } else {
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .layoutDirection = CLAY_TOP_TO_BOTTOM },
               .backgroundColor = P->panel2, .cornerRadius = CLAY_CORNER_RADIUS(UISC(6)),
               .border = { .width = CLAY_BORDER_OUTSIDE(1), .color = P->border } }) {
            node_kv_row(P, CLAY_STRING("Uptime"),        nf_dur(ms->uptime_s),
                           CLAY_STRING("Snapshot age"),  nf_age(ms->age_s));
            node_kv_row(P, CLAY_STRING("Peers"),         ui_fmt("%u / %u", ms->peers, ms->max_peers),
                           CLAY_STRING("Topics"),        ui_fmt("%u / %u", ms->topics, ms->max_topics));
            node_kv_row(P, CLAY_STRING("Mem in use"),    nf_bytesu(ms->mem_in_use),
                           CLAY_STRING("Mem peak"),      nf_bytesu(ms->mem_peak));
            node_kv_row(P, CLAY_STRING("Alloc calls"),   nf_grp((long)ms->alloc_calls),
                           CLAY_STRING("Evicted unsent"),nf_grp((long)ms->evicted_unsent));
            node_kv_row(P, CLAY_STRING("BP waits"),      nf_grp((long)ms->bp_waits),
                           CLAY_STRING("BP waited"),     ui_fmt("%.1f s", (double)ms->bp_waited_us / 1e6));
            node_kv_row(P, CLAY_STRING("SHM sent"),      nf_grp((long)ms->shm_tx),
                           CLAY_STRING("SHM received"),  nf_grp((long)ms->shm_rx));
            node_kv_single(P, CLAY_STRING("Last error"),
                           ms->last_error ? ui_fmt("%s", ms->last_error_text[0] ? ms->last_error_text
                                                                                : "(no text)")
                                          : ui_str(ND_DASH));
        }
        ui_section_label(P, CLAY_STRING("PROCESS"));
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .layoutDirection = CLAY_TOP_TO_BOTTOM },
               .backgroundColor = P->panel2, .cornerRadius = CLAY_CORNER_RADIUS(UISC(6)),
               .border = { .width = CLAY_BORDER_OUTSIDE(1), .color = P->border } }) {
            if (!ms->have_proc){
                node_kv_single(P, CLAY_STRING("Process stats"),
                               CLAY_STRING("not measured on that platform"));
            } else {
                node_kv_row(P, CLAY_STRING("CPU"),  ms->have_cpu && ms->cpu_pct >= 0.0 ? ui_fmt("%.1f %%", ms->cpu_pct)
                                                                      : ui_str(ND_DASH),
                               CLAY_STRING("PID"),  ui_fmt("%llu", (unsigned long long)ms->pid));
                node_kv_row(P, CLAY_STRING("Memory (RSS)"), nf_bytesu(ms->rss),
                               CLAY_STRING("Peak RSS"),     nf_bytesu(ms->peak_rss));
                if (ms->heap_total){
                    node_kv_row(P, CLAY_STRING("Default heap"), nf_bytesu(ms->heap_total),
                                   CLAY_STRING("Free now"),     nf_bytesu(ms->heap_free));
                    node_kv_row(P, CLAY_STRING("Minimum free"), nf_bytesu(ms->heap_min_free),
                                   CLAY_STRING("Largest block"), nf_bytesu(ms->heap_largest_free_block));
                }
            }
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

        /* the node's own @dart/meta snapshot, fetched once and shared by the RUNTIME /
           PROCESS sections and the per-topic counters folded into PUBLISHES / SUBSCRIBES.
           Read only during this layout pass (never handed to Clay as a pointer). */
        CapMetaStats ms;
        int watching = app->cap ? cap_meta_stats(app->cap, &ms) : 0;
        int fresh    = watching && ms.valid && strcmp(ms.node, nd->name) == 0;

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
                node_kv_row(P, CLAY_STRING("Unicast locator"), ui_str(nd->disc.unicast),
                               CLAY_STRING("Discovery group"), ui_str(nd->disc.discovery_group));
                node_kv_row(P, CLAY_STRING("Last announce"), nf_age(nd->disc.last_announce_age_s),
                               CLAY_STRING("Observed for"),  nf_dur(nd->observed_s));
                node_kv_single(P, CLAY_STRING("Announce updates"), nf_grp((long)nd->updates));
            }

            /* ===== ANNOUNCE METADATA: the transport overlay, decoded by our node ===== */
            ui_section_label(P, CLAY_STRING("ANNOUNCE METADATA"));
            CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .layoutDirection = CLAY_TOP_TO_BOTTOM },
                   .backgroundColor = P->panel2, .cornerRadius = CLAY_CORNER_RADIUS(UISC(6)),
                   .border = { .width = CLAY_BORDER_OUTSIDE(1), .color = P->border } }) {
                node_kv_row(P, CLAY_STRING("Fragment size"), nf_bytes(nd->disc.frag_size_bytes),
                               CLAY_STRING("Announce blob"), nf_bytes(nd->disc.blob_bytes));
            }

            /* ===== RUNTIME: the node's own internals, served by its @dart/meta endpoint
               (a directed call once per second; see net_capture's cap_meta_poll) ===== */
            nodes_runtime_sections(P, nd, &ms, watching, fresh);

            ui_section_label(P, ui_fmt("PUBLISHES  %d", nd->n_pubs));
            node_endpoint_list(P, D, nd->pubs, nd->pub_rel, nd->n_pubs, fresh ? &ms : NULL, 1);
            ui_section_label(P, ui_fmt("SUBSCRIBES  %d", nd->n_subs));
            node_endpoint_list(P, D, nd->subs, nd->sub_rel, nd->n_subs, fresh ? &ms : NULL, 0);
        }
        ui_scrollbar(P, CLAY_ID("nodes_detail_scroll"));
      }
    }
}

/* ================================================================== log sidebar */

/* a level filter pill: colored while its level is visible, faint while hidden */
static void nodes_log_pill(AppState *app, const Palette *P, Clay_String label,
                           unsigned bit, Clay_Color on_color){
    int on = (app->nodelog_mask & bit) != 0;
    if (ui_pill(P, label, FAM_SANS, WT_SEMI, FS_CAPTION,
                on ? on_color : P->faint,
                on ? P->panel2 : UI_NONE,
                on ? on_color : P->border, UISC(22)))
        app->nodelog_mask ^= bit;
}

static void nodes_log_row(AppState *app, const Palette *P, const CapNodeLogLine *L, int idx){
    Clay_Color lc = L->level == 0 ? P->red : L->level == 1 ? P->amber : P->dim;
    (void)app;
    CLAY({ .id = CLAY_IDI("nodelog_row", (uint32_t)idx),
           .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) },
                       .padding = { .left = UISCI(12), .right = UISCI(12), .top = UISCI(5), .bottom = UISCI(5) },
                       .childGap = UISCI(8) } }) {
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_FIXED(UISC(8)) },
                           .padding = { .top = UISCI(4) },
                           .childAlignment = { .x = CLAY_ALIGN_X_CENTER } } }) {
            ui_dot(UISC(7), lc, UI_NONE);
        }
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) },
                           .layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = UISCI(2) } }) {
            CLAY_TEXT(nf_clock(L->wall_us),
                      CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_CAPTION),
                                         .textColor = P->faint, .wrapMode = CLAY_TEXT_WRAP_NONE }));
            CLAY_TEXT(ui_str(L->text[0] ? L->text : "(empty line)"),
                      CLAY_TEXT_CONFIG({ UI_FONT(FAM_MONO, WT_REG, FS_SMALL),
                                         .textColor = P->text, .wrapMode = CLAY_TEXT_WRAP_WORDS }));
        }
    }
}

/* right-hand sidebar: the SELECTED node's @dart/log stream (the explorer subscribes to
   all three shared levels at start; each node's recent history replays on join), newest
   first, filtered by the per-level pills in the header. */
#define ND_LOG_SHOW 96   /* lines rendered (the capture ring holds more) */
static void nodes_log_sidebar(AppState *app, const Palette *P){
    const Dataset *D = app->data;
    const char *sel = (D && D->n_nodes && app->sel_node >= 0 && app->sel_node < D->n_nodes)
                    ? D->nodes[app->sel_node].name : NULL;
    static CapNodeLogLine lines[ND_LOG_SHOW];
    int n = (app->cap && sel) ? cap_node_log(app->cap, sel, app->nodelog_mask, lines, ND_LOG_SHOW) : 0;
    CLAY({ .id = CLAY_ID("nodes_log"),
           .layout = { .sizing = { .width = CLAY_SIZING_FIXED(UISC(340)), .height = CLAY_SIZING_GROW(0) },
                       .layoutDirection = CLAY_TOP_TO_BOTTOM },
           .backgroundColor = P->panel,
           .border = { .width = { UISCI(1), 0, 0, 0, 0 }, .color = P->border } }) {
        /* header: label + the level filter pills */
        CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(UISC(40)) },
                           .padding = { .left = UISCI(12), .right = UISCI(10) },
                           .childGap = UISCI(6), .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
               .border = { .width = { 0, 0, 0, UISCI(1), 0 }, .color = P->border } }) {
            ui_section_label(P, CLAY_STRING("NODE LOG"));
            CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}   /* spacer */
            nodes_log_pill(app, P, CLAY_STRING("ERR"),  1u << 0, P->red);
            nodes_log_pill(app, P, CLAY_STRING("WARN"), 1u << 1, P->amber);
            nodes_log_pill(app, P, CLAY_STRING("INFO"), 1u << 2, P->dim);
        }
        CLAY({ .id = CLAY_ID("nodes_log_scroll"),
               .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0) },
                           .layoutDirection = CLAY_TOP_TO_BOTTOM, .padding = { .bottom = UISCI(12) } },
               .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() } }) {
            if (!sel)          ui_placeholder(P, CLAY_STRING("select a node"));
            else if (n == 0)   ui_placeholder(P, CLAY_STRING("no log lines"));
            else { int i; for (i = 0; i < n; i++) nodes_log_row(app, P, &lines[i], i); }
        }
        ui_scrollbar(P, CLAY_ID("nodes_log_scroll"));
    }
}

static void nodes_tab(AppState *app, const Palette *P){
    const Dataset *D = app->data;
    /* keep the 1 Hz @dart/meta poll aimed at the selected node */
    if (app->cap)
        cap_meta_watch(app->cap, (D && D->n_nodes && app->sel_node >= 0 && app->sel_node < D->n_nodes)
                                 ? D->nodes[app->sel_node].name : NULL);
    nodes_list(app, P);
    nodes_detail(app, P);
    nodes_log_sidebar(app, P);
}

#endif /* UI_TAB_NODES_H */
