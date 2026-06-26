/* Topic tree: build a segment tree from the topics' '/'-separated paths and flatten
   it into TreeRow[] honoring the app's collapsed-branch set. This is the C port of
   buildTree()/flatten() in the handoff's data.js. A node can be both a namespace and
   a topic (e.g. "diagnostics" with child "diagnostics/heartbeat"), so an interior
   node carries a Topic* when one exists. Rebuilt each frame before the tree renders;
   the pools are static, so the TreeRow name/path pointers stay valid for the frame.
   Requires ui_model.h (Dataset/TreeRow) and ui_app.h (the collapsed set). */
#ifndef UI_TREE_H
#define UI_TREE_H

#define UT_MAX_NODES (UID_MAX_TOPICS * 4)   /* segments can outnumber topics (namespaces) */

typedef struct {
    char name[CAP_TOPIC_CAP];   /* one path segment */
    char path[96];              /* accumulated path to this segment */
    int  topic;                 /* index into Dataset.topics, or -1 for a pure namespace */
    int  first_child, next_sibling, n_children;
} UtNode;

static UtNode  ut_pool[UT_MAX_NODES];
static int     ut_n;
static TreeRow ut_rows[UT_MAX_NODES];
static int     ut_n_rows;

/* find (or create) the child of `parent` named `seg`; children keep insertion order */
static int ut_child(int parent, const char *seg, const char *fullpath){
    int c = ut_pool[parent].first_child, last = -1;
    for (; c != -1; c = ut_pool[c].next_sibling){
        if (!strcmp(ut_pool[c].name, seg)) return c;
        last = c;
    }
    if (ut_n >= UT_MAX_NODES) return parent;   /* pool full: fold into parent (degrade, don't crash) */
    c = ut_n++;
    memset(&ut_pool[c], 0, sizeof ut_pool[c]);
    snprintf(ut_pool[c].name, sizeof ut_pool[c].name, "%s", seg);
    snprintf(ut_pool[c].path, sizeof ut_pool[c].path, "%s", fullpath);
    ut_pool[c].topic = -1; ut_pool[c].first_child = -1; ut_pool[c].next_sibling = -1;
    if (last == -1) ut_pool[parent].first_child = c;
    else            ut_pool[last].next_sibling  = c;
    ut_pool[parent].n_children++;
    return c;
}

static void ut_build(const Dataset *D){
    int t;
    ut_n = 1;                                  /* node 0 = implicit root */
    memset(&ut_pool[0], 0, sizeof ut_pool[0]);
    ut_pool[0].topic = -1; ut_pool[0].first_child = -1; ut_pool[0].next_sibling = -1;

    for (t = 0; t < D->n_topics; t++){
        const char *p = D->topics[t].path;
        char acc[96]; int acc_len = 0, node = 0;
        acc[0] = '\0';
        while (*p){
            char seg[CAP_TOPIC_CAP]; int sl = 0;
            while (*p && *p != '/'){ if (sl < (int)sizeof seg - 1) seg[sl++] = *p; p++; }
            seg[sl] = '\0';
            while (*p == '/') p++;
            if (acc_len && acc_len < (int)sizeof acc - 1) acc[acc_len++] = '/';
            { int k = 0; while (seg[k] && acc_len < (int)sizeof acc - 1) acc[acc_len++] = seg[k++]; }
            acc[acc_len] = '\0';
            node = ut_child(node, seg, acc);
        }
        if (node != 0) ut_pool[node].topic = t;
    }
}

/* DFS into ut_rows[], skipping the children of a collapsed branch */
static void ut_flatten(const AppState *app, const Dataset *D, int node, int depth){
    int c;
    for (c = ut_pool[node].first_child; c != -1; c = ut_pool[c].next_sibling){
        int is_branch = ut_pool[c].n_children > 0;
        int collapsed = is_branch && app_is_collapsed(app, ut_pool[c].path);
        TreeRow *r;
        if (ut_n_rows >= UT_MAX_NODES) return;
        r = &ut_rows[ut_n_rows++];
        r->name      = ut_pool[c].name;
        r->path      = ut_pool[c].path;
        r->depth     = depth;
        r->is_branch = is_branch;
        r->has_topic = ut_pool[c].topic >= 0;
        r->open      = !collapsed;
        r->topic     = r->has_topic ? &D->topics[ut_pool[c].topic] : NULL;
        if (is_branch && !collapsed) ut_flatten(app, D, c, depth + 1);
    }
}

/* (re)build the flattened tree for this frame; returns the row count */
static int ui_tree_build(const AppState *app){
    const Dataset *D = app->data;
    ut_n_rows = 0;
    if (!D || D->n_topics == 0) return 0;
    ut_build(D);
    ut_flatten(app, D, 0, 0);
    return ut_n_rows;
}

#endif /* UI_TREE_H */
