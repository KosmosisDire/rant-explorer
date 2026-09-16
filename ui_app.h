/* The single app state struct. Every widget is re emitted from it each frame, since Clay
   is immediate mode and holds no retained state of ours. */
#ifndef UI_APP_H
#define UI_APP_H

typedef enum { TAB_TOPICS, TAB_NODES } Tab;
typedef enum { DRAWER_PUBLISH, DRAWER_INSPECT } DrawerTab;   /* the sidebar tabs, in order */

#define UI_MAX_EXPANDED 128    /* tracked expanded tree branches (default = collapsed) */
#define UI_COMPOSE_MAX   1024  /* bytes the message composer accepts (fits one fragment) */
#define UI_FORM_MAX      24    /* fields the structured publish form edits (all depths) */
#define UI_FORM_VAL      40    /* value text per form field */

typedef struct {
    int  theme_dark;          /* 1 dark, 0 light */
    Tab  tab;

    /* Selection is keyed by identity, since the dataset is rebuilt and reordered every
       frame. The index fields are this frame's resolution, -1 = absent (spec/explorer.md). */
    int  sel_node;            /* index into data->nodes, resolved from sel_node_id  */
    int  sel_topic;           /* index into data->topics, resolved from sel_topic_path */
    char sel_node_id[32];     /* the selected node's id ("" = index-only default) */
    char sel_topic_path[96];  /* the selected topic's path ("" = nothing selected) */

    unsigned nodelog_mask;    /* the Nodes tab log sidebar level filter, bits (1 << RantLogLevel),
                                 all three on by default */

    int        drawer_open;   /* Topics right sidebar (Inspect / Publish) */
    DrawerTab  drawer_tab;    /* which sidebar tab is showing */

    /* topic tree: a set of EXPANDED branch-path hashes (absent = collapsed, the default) */
    uint64_t expanded[UI_MAX_EXPANDED];
    int      n_expanded;

    /* message composer at the bottom of the Topics feed (publishes to the selected topic) */
    char     compose[UI_COMPOSE_MAX];
    int      compose_len;
    int      compose_topic;   /* selected topic the draft belongs to (reset draft on change) */

    /* the structured publish form on a typed topic: one value string per top level field,
       prefilled with the default, or for a variable with the value it holds once it lands */
    char     form_val[UI_FORM_MAX][UI_FORM_VAL];
    int      form_len[UI_FORM_MAX];
    int      form_n;          /* fields shown this frame (the composer sets it) */
    int      form_focus;      /* focused field, or -1 (the free-text composer owns input) */
    int      form_topic;      /* the sel_topic the values belong to, re defaulted on change */
    int      form_filled;     /* the variable's value has seeded the form (or the user typed
                                 first): stop trying, so a later write never stomps an edit */
    uint32_t form_seed;       /* hash of the values WE wrote, so an edit is recognizable */

    /* a message pulled out of the feed to inspect, a durable copy that survives the ring
       overwriting the original. Cleared when the selected topic changes. */
    CapFeedItem inspect_msg;
    int         has_inspect_msg;
    char        inspect_msg_topic[CAP_TOPIC_CAP];   /* topic the copied message came from */

    /* the topic tree filter: rows whose path, endpoint node name or schema type name lacks
       this substring, case insensitive, are hidden */
    char     topic_filter[CAP_TOPIC_CAP];
    int      topic_filter_len;

    /* the topic tree category filter, a bitmask of TT_CAT_* checkboxes, 0 = every topic. Bits
       within the kind and QoS groups are OR'd, the groups are AND'd. */
    unsigned topic_cats;

    /* "add a topic" input (the + by the filter): type a name to publish to a new topic */
    int      adding_topic;    /* 1 = the new-topic name field is showing */
    char     new_topic[CAP_TOPIC_CAP];
    int      new_topic_len;
    char     select_topic[CAP_TOPIC_CAP];   /* pending: select this topic once it appears */

    /* text-box widget state, one per editable field (ui_textbox.h owns the semantics) */
    UiTbState tb_filter, tb_new_topic, tb_compose;
    UiTbState tb_form[UI_FORM_MAX];

    /* the feed auto scroll: stick to the newest message while the user is parked at the
       bottom, a scroll up unlocks it, returning to the bottom re locks */
    int      feed_pinned;       /* 1 = glued to the bottom */
    int      feed_sel_topic;    /* selected topic the pin state belongs to (reset on change) */
    float    feed_prev_scroll_y;/* the scroll offset we left set last frame (detects user scroll) */

    /* the feed table's visible field name hashes, toggled from the header menu and seeded
       when the topic changes. Sized for two schemas, a function feed carries req. and rsp. */
    uint64_t col_vis[CAP_MSG_FIELDS * 2];
    int      n_col_vis;
    int      col_vis_topic;     /* sel_topic the set was seeded for (-1 = unseeded) */
    int      col_show_time;     /* the fixed time / sender columns: toggled from the same
                                   header menu, persistent across topics (not schema-bound) */
    int      col_show_from;

    Capture          *cap;    /* the live observer, NULL if not started */
    const Dataset     *data;  /* rebuilt each frame from the live snapshot */
} AppState;

/* topic-tree category-filter bits (AppState.topic_cats). Three groups: KIND (which
   entity), QOS (reliability), STATE (live subscription / traffic). */
enum {
    TT_CAT_TOPIC      = 1u << 0,   /* plain pub/sub topics */
    TT_CAT_FUNCTION   = 1u << 1,   /* function entities */
    TT_CAT_VARIABLE   = 1u << 2,   /* variable entities */
    TT_CAT_RELIABLE   = 1u << 3,   /* reliable QoS */
    TT_CAT_BEST_EFF   = 1u << 4,   /* best-effort QoS */
    TT_CAT_SUBSCRIBED = 1u << 5,   /* the explorer is subscribed */
    TT_CAT_ACTIVE     = 1u << 6,   /* actively publishing (a measurable rate) */
    TT_CAT_TASK       = 1u << 7    /* task entities */
};
#define TT_CAT_GROUP_KIND (TT_CAT_TOPIC | TT_CAT_FUNCTION | TT_CAT_VARIABLE | TT_CAT_TASK)
#define TT_CAT_GROUP_QOS  (TT_CAT_RELIABLE | TT_CAT_BEST_EFF)

/* 1 if a topic passes the category filter (see AppState.topic_cats). cats == 0 = all pass. */
static int app_topic_cat_match(const Topic *t, unsigned cats){
    unsigned kind = cats & TT_CAT_GROUP_KIND, qos = cats & TT_CAT_GROUP_QOS;
    if (kind){                                   /* OR within the kind group */
        unsigned bit = t->kind == CAP_KIND_FUNCTION ? TT_CAT_FUNCTION
                     : t->kind == CAP_KIND_VARIABLE ? TT_CAT_VARIABLE
                     : t->kind == CAP_KIND_TASK     ? TT_CAT_TASK
                     :                                TT_CAT_TOPIC;
        if (!(kind & bit)) return 0;
    }
    if (qos){                                    /* OR within the QoS group (needs a known QoS) */
        unsigned bit;
        if (!t->has_qos) return 0;
        bit = t->reliable ? TT_CAT_RELIABLE : TT_CAT_BEST_EFF;
        if (!(qos & bit)) return 0;
    }
    if ((cats & TT_CAT_SUBSCRIBED) && !t->sub_state)     return 0;   /* AND: more predicates */
    if ((cats & TT_CAT_ACTIVE)     && !(t->rate_hz > 0.0)) return 0;
    return 1;
}

static void app_init(AppState *a, const Dataset *data){
    int fk;
    a->theme_dark  = 1;
    a->tab         = TAB_TOPICS;
    a->sel_node    = 0;
    a->sel_topic   = -1;
    a->sel_node_id[0]    = '\0';
    a->sel_topic_path[0] = '\0';
    a->nodelog_mask = 0x7;   /* error + warn + info visible */
    a->drawer_open = 1;
    a->drawer_tab  = DRAWER_PUBLISH;   /* publishing is the common reason to open a topic */
    a->n_expanded  = 0;
    a->compose_len   = 0;
    a->compose[0]    = '\0';
    a->compose_topic = -1;
    a->form_n     = 0;
    a->form_focus = -1;
    a->form_topic = -1;
    a->form_filled = 0;
    a->form_seed   = 0;
    a->has_inspect_msg   = 0;
    a->inspect_msg_topic[0] = '\0';
    a->topic_filter_len   = 0;
    a->topic_filter[0]    = '\0';
    a->topic_cats       = 0;
    a->adding_topic     = 0;
    a->new_topic_len    = 0;
    a->new_topic[0]     = '\0';
    a->select_topic[0]  = '\0';
    memset(&a->tb_filter, 0, sizeof a->tb_filter);
    memset(&a->tb_new_topic, 0, sizeof a->tb_new_topic);
    memset(&a->tb_compose, 0, sizeof a->tb_compose);
    for (fk = 0; fk < UI_FORM_MAX; fk++) memset(&a->tb_form[fk], 0, sizeof a->tb_form[fk]);
    a->feed_pinned    = 1;
    a->feed_sel_topic = -1;
    a->feed_prev_scroll_y = 0.0f;
    a->n_col_vis      = 0;
    a->col_vis_topic  = -1;
    a->col_show_time  = 1;
    a->col_show_from  = 1;
    a->cap         = NULL;
    a->data        = data;
}

/* FNV-1a over a path string, the key for the expanded-branch set */
static uint64_t ui_path_hash(const char *s){
    uint64_t h = 1469598103934665603ull;
    for (; s && *s; s++){ h ^= (unsigned char)*s; h *= 1099511628211ull; }
    return h;
}
static int app_is_expanded(const AppState *a, const char *path){
    uint64_t h = ui_path_hash(path); int i;
    for (i = 0; i < a->n_expanded; i++) if (a->expanded[i] == h) return 1;
    return 0;
}
static void app_toggle_expanded(AppState *a, const char *path){
    uint64_t h = ui_path_hash(path); int i;
    for (i = 0; i < a->n_expanded; i++)
        if (a->expanded[i] == h){ a->expanded[i] = a->expanded[--a->n_expanded]; return; }
    if (a->n_expanded < UI_MAX_EXPANDED) a->expanded[a->n_expanded++] = h;
}

/* expand every ancestor branch of a topic path so it is visible in the collapsed tree, for
   a selection arriving by name. Branch keys normalize both separators to '/'. */
static void app_expand_to(AppState *a, const char *path){
    char acc[96]; int n = 0;
    const char *p;
    for (p = path; *p; p++){
        if (*p == '/' || *p == '.'){
            if (n && acc[n - 1] != '/'){
                if (!app_is_expanded(a, acc)) app_toggle_expanded(a, acc);
                if (n < (int)sizeof acc - 1) acc[n++] = '/';
            }
        } else if (n < (int)sizeof acc - 1) acc[n++] = *p;
        acc[n] = '\0';
    }
}

/* select a topic: the path is the durable key, the index this frame's resolution. It also
   joins the data plane at the recommended reliability, except a function. */
static void app_select_topic(AppState *a, const Dataset *D, int idx){
    a->sel_topic = idx;
    snprintf(a->sel_topic_path, sizeof a->sel_topic_path, "%s",
             (idx >= 0 && idx < D->n_topics) ? D->topics[idx].path : "");
    if (a->cap && idx >= 0 && idx < D->n_topics){
        const Topic *t = &D->topics[idx];
        if (t->kind != CAP_KIND_FUNCTION && !t->sub_state)
            cap_subscribe(a->cap, t->path, t->reliable_recommend > 0);
    }
}
static void app_select_node(AppState *a, const Dataset *D, int idx){
    a->sel_node = idx;
    snprintf(a->sel_node_id, sizeof a->sel_node_id, "%s",
             (idx >= 0 && idx < D->n_nodes) ? D->nodes[idx].id : "");
}

/* message-table column visibility, keyed by a field's flattened name (pos.x) */
static int app_col_visible(const AppState *a, const char *name){
    uint64_t h = ui_path_hash(name); int i;
    for (i = 0; i < a->n_col_vis; i++) if (a->col_vis[i] == h) return 1;
    return 0;
}
static void app_col_toggle(AppState *a, const char *name){
    uint64_t h = ui_path_hash(name); int i;
    for (i = 0; i < a->n_col_vis; i++)
        if (a->col_vis[i] == h){ a->col_vis[i] = a->col_vis[--a->n_col_vis]; return; }
    if (a->n_col_vis < (int)(sizeof a->col_vis / sizeof a->col_vis[0])) a->col_vis[a->n_col_vis++] = h;
}

/* copy a feed message into the durable inspect slot and route the sidebar to show it. The
   copy is by value so it outlives the feed ring overwriting the original. */
static void app_inspect_msg(AppState *a, const char *topic, const CapFeedItem *m){
    a->inspect_msg = *m;
    a->has_inspect_msg = 1;
    snprintf(a->inspect_msg_topic, sizeof a->inspect_msg_topic, "%s", topic ? topic : "");
    a->drawer_open = 1;
    a->drawer_tab  = DRAWER_INSPECT;
}

#endif /* UI_APP_H */
