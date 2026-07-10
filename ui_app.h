/* The single app-state struct. Every widget is re-emitted from this each frame;
   Clay is immediate-mode and holds no retained state of ours. */
#ifndef UI_APP_H
#define UI_APP_H

typedef enum { TAB_NODES, TAB_TOPICS, TAB_LOG } Tab;
typedef enum { DRAWER_INSPECT, DRAWER_PUBLISH } DrawerTab;   /* the Topics right-sidebar tabs */

#define UI_MAX_COLLAPSED 128   /* tracked collapsed tree branches (default = expanded) */
#define UI_COMPOSE_MAX   1024  /* bytes the message composer accepts (fits one fragment) */
#define UI_FORM_MAX      24    /* fields the structured publish form edits (all depths) */
#define UI_FORM_VAL      40    /* value text per form field */

typedef struct {
    int  theme_dark;          /* 1 dark, 0 light */
    Tab  tab;

    int  sel_node;            /* index into data->nodes  */
    int  sel_topic;           /* index into data->topics */

    int        drawer_open;   /* Topics right sidebar (Inspect / Publish) */
    DrawerTab  drawer_tab;    /* which sidebar tab is showing */

    /* topic tree: a set of COLLAPSED branch-path hashes (absent = expanded, the default) */
    uint64_t collapsed[UI_MAX_COLLAPSED];
    int      n_collapsed;

    /* message composer at the bottom of the Topics feed (publishes to the selected topic) */
    char     compose[UI_COMPOSE_MAX];
    int      compose_len;
    int      compose_send;    /* set by Enter in the event loop; consumed when the feed draws */
    int      compose_topic;   /* selected topic the draft belongs to (reset draft on change) */

    /* structured publish form (replaces the composer on a typed topic): one value string
       per top-level schema field, prefilled with the type's default */
    char     form_val[UI_FORM_MAX][UI_FORM_VAL];
    int      form_len[UI_FORM_MAX];
    int      form_n;          /* fields shown this frame (the composer sets it) */
    int      form_focus;      /* focused field, or -1 (the free-text composer owns input) */
    int      form_topic;      /* sel_topic the values belong to; re-defaulted on change */
    int      form_send;       /* Enter in a form field: publish (consumed by the composer) */

    /* a message pulled out of the feed to inspect: a durable COPY (survives the feed ring
       overwriting the original), shown in the Inspect sidebar tab in place of the topic
       overview. Cleared when the selected topic changes or the user backs out. */
    CapFeedItem inspect_msg;
    int         has_inspect_msg;
    char        inspect_msg_topic[CAP_TOPIC_CAP];   /* topic the copied message came from */

    /* "add a topic" input (the + by the filter): type a name to publish to a new topic */
    int      adding_topic;    /* 1 = the new-topic name field has focus (text routes here) */
    char     new_topic[CAP_TOPIC_CAP];
    int      new_topic_len;
    int      new_topic_commit;/* set by Enter in the event loop; consumed when the tree draws */
    char     select_topic[CAP_TOPIC_CAP]; /* pending: select this topic once it appears in the list */

    /* Topics feed auto-scroll: stick to the newest message at the bottom while the user
       is parked there; a scroll up unlocks it, returning to the bottom re-locks. */
    int      feed_pinned;       /* 1 = glued to the bottom */
    int      feed_sel_topic;    /* selected topic the pin state belongs to (reset on change) */
    float    feed_prev_scroll_y;/* the scroll offset we left set last frame (detects user scroll) */

    /* message-feed table columns: a set of VISIBLE field-name hashes. Right-click the header
       to toggle a field; seeded to the first few fields when the selected topic changes. */
    uint64_t col_vis[CAP_MSG_FIELDS];
    int      n_col_vis;
    int      col_vis_topic;     /* sel_topic the set was seeded for (-1 = unseeded) */
    int      col_menu_open;     /* the header right-click column menu is showing */
    float    col_menu_x, col_menu_y;   /* menu anchor (physical px, cursor at open) */

    Capture          *cap;    /* live observer (subscribe / read the feed); NULL if not started */
    const Dataset     *data;  /* rebuilt each frame from the live snapshot */
    const CapSnapshot *snap;  /* the raw snapshot (Log tab reads its event lines) */
} AppState;

static void app_init(AppState *a, const Dataset *data){
    a->theme_dark  = 1;
    a->tab         = TAB_NODES;
    a->sel_node    = 0;
    a->sel_topic   = 0;
    a->drawer_open = 1;
    a->drawer_tab  = DRAWER_INSPECT;
    a->n_collapsed = 0;
    a->compose_len   = 0;
    a->compose_send  = 0;
    a->compose[0]    = '\0';
    a->compose_topic = -1;
    a->form_n     = 0;
    a->form_focus = -1;
    a->form_topic = -1;
    a->form_send  = 0;
    a->has_inspect_msg   = 0;
    a->inspect_msg_topic[0] = '\0';
    a->adding_topic     = 0;
    a->new_topic_len    = 0;
    a->new_topic[0]     = '\0';
    a->new_topic_commit = 0;
    a->select_topic[0]  = '\0';
    a->feed_pinned    = 1;
    a->feed_sel_topic = -1;
    a->feed_prev_scroll_y = 0.0f;
    a->n_col_vis      = 0;
    a->col_vis_topic  = -1;
    a->col_menu_open  = 0;
    a->cap         = NULL;
    a->data        = data;
    a->snap        = NULL;
}

/* FNV-1a over a path string, the key for the collapsed-branch set */
static uint64_t ui_path_hash(const char *s){
    uint64_t h = 1469598103934665603ull;
    for (; s && *s; s++){ h ^= (unsigned char)*s; h *= 1099511628211ull; }
    return h;
}
static int app_is_collapsed(const AppState *a, const char *path){
    uint64_t h = ui_path_hash(path); int i;
    for (i = 0; i < a->n_collapsed; i++) if (a->collapsed[i] == h) return 1;
    return 0;
}
static void app_toggle_collapsed(AppState *a, const char *path){
    uint64_t h = ui_path_hash(path); int i;
    for (i = 0; i < a->n_collapsed; i++)
        if (a->collapsed[i] == h){ a->collapsed[i] = a->collapsed[--a->n_collapsed]; return; }
    if (a->n_collapsed < UI_MAX_COLLAPSED) a->collapsed[a->n_collapsed++] = h;
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
