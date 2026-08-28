/* Topic tree: build a segment tree from the topics' paths (segments split on '/' OR
   '.', either works as a namespace separator) and flatten it into TreeRow[] honoring
   the app's expanded-branch set (branches are collapsed by default). This is the C port of buildTree()/flatten() in the
   handoff's data.js. A node can be both a namespace and a topic (e.g. "diagnostics"
   with child "diagnostics/heartbeat"), so an interior node carries a Topic* when one
   exists. Rebuilt each frame before the tree renders; the pools are static, so the
   TreeRow name/path pointers stay valid for the frame. Requires ui_model.h
   (Dataset/TreeRow) and ui_app.h (the collapsed set). */
#ifndef UI_TREE_H
#define UI_TREE_H

#define UT_MAX_NODES (UID_MAX_TOPICS * 4)   /* segments can outnumber topics (namespaces) */
#define UT_HASH      131072                  /* power of two, >= 2*UT_MAX_NODES (128000): (parent,seg)->node map */

typedef struct {
    char name[CAP_TOPIC_CAP];   /* one path segment */
    char path[96];              /* accumulated path to this segment */
    int  topic;                 /* index into Dataset.topics, or -1 for a pure namespace */
    int  parent;                /* owning node, or -1 for the root */
    int  depth;                 /* 0 = top level (matches ut_flatten's row depth); root is -1 */
    int  first_child, last_child, next_sibling, n_children;
} UtNode;

static UtNode  ut_pool[UT_MAX_NODES];
static int     ut_n;
static TreeRow ut_rows[UT_MAX_NODES];
static int     ut_n_rows;
static int     ut_slot[UT_HASH];          /* (parent,seg) hash -> node index, -1 empty; reset per build */
static int     ut_order[UT_MAX_NODES];    /* topic indices sorted by path (drives sibling order) */

static uint32_t ut_child_key(int parent, const char *seg){
    uint32_t h = 2166136261u ^ (uint32_t)parent;
    h *= 16777619u;
    for (; *seg; seg++){ h ^= (unsigned char)*seg; h *= 16777619u; }
    return h;
}

/* find (or create) the child of `parent` named `seg`. Lookup is O(1) via ut_slot; on a miss the
   child is appended at the tail. Siblings therefore render in the order topics are fed in, which
   ut_build makes alphabetical by walking topics path-sorted (so same-prefix topics arrive adjacent
   and their new segments append in order) -- no per-insert sibling scan, which was O(n^2). */
static int ut_child(int parent, const char *seg, const char *fullpath){
    uint32_t h = ut_child_key(parent, seg) & (UT_HASH - 1);
    int c;
    while ((c = ut_slot[h]) >= 0){
        if (ut_pool[c].parent == parent && !strcmp(ut_pool[c].name, seg)) return c;
        h = (h + 1) & (UT_HASH - 1);
    }
    if (ut_n >= UT_MAX_NODES) return parent;   /* pool full: fold into parent (degrade, don't crash) */
    { int nc = ut_n++;
      memset(&ut_pool[nc], 0, sizeof ut_pool[nc]);
      snprintf(ut_pool[nc].name, sizeof ut_pool[nc].name, "%s", seg);
      snprintf(ut_pool[nc].path, sizeof ut_pool[nc].path, "%s", fullpath);
      ut_pool[nc].topic = -1; ut_pool[nc].parent = parent;
      ut_pool[nc].depth = (parent == 0) ? 0 : ut_pool[parent].depth + 1;
      ut_pool[nc].first_child = ut_pool[nc].last_child = ut_pool[nc].next_sibling = -1;
      if (ut_pool[parent].first_child == -1) ut_pool[parent].first_child = nc;
      else ut_pool[ut_pool[parent].last_child].next_sibling = nc;
      ut_pool[parent].last_child = nc;
      ut_pool[parent].n_children++;
      ut_slot[h] = nc;
      return nc;
    }
}

static const Dataset *ut_sort_D;   /* qsort comparator context (UI is single-threaded) */
static int ut_path_cmp(const void *a, const void *b){
    return strcmp(ut_sort_D->topics[*(const int *)a].path, ut_sort_D->topics[*(const int *)b].path);
}

/* case-insensitive substring test for the topic filter (ASCII, like the names on the wire) */
static int ut_strcasestr(const char *hay, const char *needle){
    size_t nl = strlen(needle);
    if (!nl) return 1;
    for (; *hay; hay++){
        size_t i;
        for (i = 0; i < nl; i++){
            char a = hay[i], b = needle[i];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
        }
        if (i == nl) return 1;
    }
    return 0;
}

/* a topic passes the text filter if its path, any endpoint node's name, or its advertised
   schema type name contains the text. The schema lookup is a live peer scan, so it is done
   last (only when the cheaper checks miss) and only when a capture is available. */
static int ut_topic_match(const Dataset *D, const Topic *tp, const char *filter, const Capture *cap){
    int i;
    if (ut_strcasestr(tp->path, filter)) return 1;
    for (i = 0; i < tp->n_pubs; i++)
        if (ut_strcasestr(D->nodes[tp->pubs[i]].name, filter)) return 1;
    for (i = 0; i < tp->n_subs; i++)
        if (ut_strcasestr(D->nodes[tp->subs[i]].name, filter)) return 1;
    if (cap){
        CapSchema sch;
        if (cap_topic_schema(cap, tp->path, &sch) && sch.type_name[0]
            && ut_strcasestr(sch.type_name, filter)) return 1;
    }
    return 0;
}

static int ut_n_matched;   /* topics that passed the filter this build */

/* filter = the text filter (NULL = none); cats = the category-filter bitmask (0 = none);
   cap backs the schema-name text match (NULL = skip it). */
static void ut_build(const Dataset *D, const char *filter, const Capture *cap, unsigned cats){
    int t, no = 0;
    ut_n = 1;                                  /* node 0 = implicit root */
    memset(ut_slot, 0xFF, sizeof ut_slot);     /* 0xFF bytes = -1 ints: empty the (parent,seg) map */
    memset(&ut_pool[0], 0, sizeof ut_pool[0]);
    ut_pool[0].topic = -1; ut_pool[0].parent = -1; ut_pool[0].depth = -1;
    ut_pool[0].first_child = ut_pool[0].last_child = ut_pool[0].next_sibling = -1;

    for (t = 0; t < D->n_topics && no < UT_MAX_NODES; t++){
        const Topic *tp = &D->topics[t];
        if (cats && !app_topic_cat_match(tp, cats)) continue;         /* category filter (cheap) */
        if (filter && !ut_topic_match(D, tp, filter, cap)) continue;  /* text filter (may scan schemas) */
        ut_order[no++] = t;
    }
    ut_n_matched = no;
    ut_sort_D = D;
    qsort(ut_order, (size_t)no, sizeof ut_order[0], ut_path_cmp);

    for (t = 0; t < no; t++){
        int ti = ut_order[t];
        const char *p = D->topics[ti].path;
        char acc[96]; int acc_len = 0, node = 0;
        acc[0] = '\0';
        while (*p){
            char seg[CAP_TOPIC_CAP]; int sl = 0;
            while (*p && *p != '/' && *p != '.'){ if (sl < (int)sizeof seg - 1) seg[sl++] = *p; p++; }
            seg[sl] = '\0';
            while (*p == '/' || *p == '.') p++;
            if (acc_len && acc_len < (int)sizeof acc - 1) acc[acc_len++] = '/';
            { int k = 0; while (seg[k] && acc_len < (int)sizeof acc - 1) acc[acc_len++] = seg[k++]; }
            acc[acc_len] = '\0';
            node = ut_child(node, seg, acc);
        }
        if (node != 0) ut_pool[node].topic = ti;
    }
}

/* DFS into ut_rows[], skipping the children of a collapsed branch. While a filter is
   active every branch renders open, so the matches are always visible. */
static void ut_flatten(const AppState *app, const Dataset *D, int node, int depth){
    int c;
    for (c = ut_pool[node].first_child; c != -1; c = ut_pool[c].next_sibling){
        int is_branch = ut_pool[c].n_children > 0;
        int filtering = app->topic_filter_len != 0 || app->topic_cats != 0;   /* any filter forces branches open */
        int collapsed = is_branch && !filtering && !app_is_expanded(app, ut_pool[c].path);
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
    ut_n_matched = 0;
    if (!D || D->n_topics == 0) return 0;
    ut_build(D, app->topic_filter_len ? app->topic_filter : NULL, app->cap, app->topic_cats);
    ut_flatten(app, D, 0, 0);
    return ut_n_rows;
}

#endif /* UI_TREE_H */
