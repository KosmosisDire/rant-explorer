/* The single app-state struct. Every widget is re-emitted from this each frame;
   Clay is immediate-mode and holds no retained state of ours. */
#ifndef UI_APP_H
#define UI_APP_H

typedef enum { TAB_NODES, TAB_TOPICS, TAB_LOG } Tab;

#define UI_MAX_COLLAPSED 128   /* tracked collapsed tree branches (default = expanded) */
#define UI_MAX_EXPANDED  64    /* tracked expanded feed messages (default = collapsed) */
#define UI_COMPOSE_MAX   1024  /* bytes the message composer accepts (fits one fragment) */
#define UI_FORM_MAX      24    /* fields the structured publish form edits (all depths) */
#define UI_FORM_VAL      40    /* value text per form field */

typedef struct {
    int  theme_dark;          /* 1 dark, 0 light */
    Tab  tab;

    int  sel_node;            /* index into data->nodes  */
    int  sel_topic;           /* index into data->topics */

    int        drawer_open;   /* Topics inspector drawer */

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
    int      form_open;       /* the publish form starts collapsed to its header bar */

    /* feed messages EXPANDED by click (collapsed one-liners are the default): a small
       replaceable set of hash(topic path) ^ message uid keys */
    uint64_t msg_expanded[UI_MAX_EXPANDED];
    int      n_msg_expanded;

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
    a->n_collapsed = 0;
    a->compose_len   = 0;
    a->compose_send  = 0;
    a->compose[0]    = '\0';
    a->compose_topic = -1;
    a->form_n     = 0;
    a->form_focus = -1;
    a->form_topic = -1;
    a->form_send  = 0;
    a->form_open  = 0;
    a->n_msg_expanded = 0;
    a->adding_topic     = 0;
    a->new_topic_len    = 0;
    a->new_topic[0]     = '\0';
    a->new_topic_commit = 0;
    a->select_topic[0]  = '\0';
    a->feed_pinned    = 1;
    a->feed_sel_topic = -1;
    a->feed_prev_scroll_y = 0.0f;
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

/* per-message feed expansion (default collapsed); key = topic path + message uid */
static uint64_t app_msg_key(const char *topic, uint32_t uid){
    return ui_path_hash(topic) ^ ((uint64_t)uid * 0x9E3779B97F4A7C15ull);
}
static int app_msg_is_expanded(const AppState *a, uint64_t key){
    int i;
    for (i = 0; i < a->n_msg_expanded; i++) if (a->msg_expanded[i] == key) return 1;
    return 0;
}
static void app_msg_toggle(AppState *a, uint64_t key){
    int i;
    for (i = 0; i < a->n_msg_expanded; i++)
        if (a->msg_expanded[i] == key){ a->msg_expanded[i] = a->msg_expanded[--a->n_msg_expanded]; return; }
    if (a->n_msg_expanded == UI_MAX_EXPANDED){          /* full: drop the oldest entry */
        memmove(a->msg_expanded, a->msg_expanded + 1, (UI_MAX_EXPANDED - 1) * sizeof a->msg_expanded[0]);
        a->n_msg_expanded--;
    }
    a->msg_expanded[a->n_msg_expanded++] = key;
}

#endif /* UI_APP_H */
