/* Display data model for the explorer. These structs mirror what dart.h exposes
   at runtime; the UI renders from them and never holds widget state of its own.
   Store raw numbers, format at draw time. Every field here is either wire data
   (from a peer's discovery announce / announce metadata / topic traffic) or
   real observer-side derivation of that wire data (rates, ages, counts); there
   are no hardcoded or fabricated fields. */
#ifndef UI_MODEL_H
#define UI_MODEL_H

/* Two distinct axes, sized apart so the big one doesn't bloat the other:
   NODE_TOPICS = topics ONE node may publish or subscribe (its endpoint list; the
   thing you scale to observe a many-topic node). ENDPOINTS = nodes on ONE topic
   (its pub/sub list), bounded by the node count (CAP_SNAP_NODES), never by topics. */
#define UI_MAX_NODE_TOPICS 16000
#define UI_MAX_ENDPOINTS      64

typedef enum { NODE_ALIVE, NODE_JOINING, NODE_DROPPED, NODE_GONE } NodeState;
typedef enum { LVL_ERROR, LVL_WARN, LVL_INFO, LVL_DEBUG } LogLevel;
typedef enum { QOS_BEST_EFFORT, QOS_RELIABLE } Reliability;

typedef struct {
    char id[16];
    char ip[40];
} Machine;

/* per-node info for the Nodes detail pane, grouped by origin: what discovery itself
   carries, and what the announce-metadata overlay carries (the node decodes it for us). */
typedef struct {
    /* discovery-level */
    double last_announce_age_s;
    char   unicast[48];
    char   discovery_group[48];
    /* announce metadata (the transport overlay, decoded by our node) */
    int    frag_size_bytes;
    int    blob_bytes;        /* observed overlay size */
} DiscInfo;

typedef struct {
    char      id[32];
    char      name[32];
    int       machine;        /* index into machines[] */
    char      ip[40];
    NodeState state;

    /* observer-derived, genuinely live (not node-internal): how long WE have seen
       this peer, and how many times its announce changed. */
    double observed_s;
    unsigned updates;

    int pubs[UI_MAX_NODE_TOPICS]; int n_pubs;   /* indices into topics[] */
    int subs[UI_MAX_NODE_TOPICS]; int n_subs;
    /* this node's own offered/requested reliability per endpoint, parallel to
       pubs[]/subs[] (the Topic carries one topic-level value; a dot in the node's
       pub/sub list wants the node's own QoS). 1 = reliable, 0 = best-effort. */
    unsigned char pub_rel[UI_MAX_NODE_TOPICS];
    unsigned char sub_rel[UI_MAX_NODE_TOPICS];

    DiscInfo disc;
} Node;

typedef struct {
    Reliability reliability;
} Qos;

typedef struct {
    char path[96];               /* "sensors/lidar/points"; the tree (ui_tree.h) groups
                                     segments split on '/' OR '.' */
    int  kind;                   /* CAP_KIND_*: plain topic, or a function/variable ENTITY */
    int  writable;               /* variable: an owner advertises a set channel */
    int  forceable;              /* variable: an owner advertises allow_force (force/unforce permitted) */
    int  incomplete;             /* pattern half-pair (a diagnosable misadvertisement) */
    Qos  qos;
    int  pubs[UI_MAX_ENDPOINTS]; int n_pubs;   /* node indices */
    int  subs[UI_MAX_ENDPOINTS]; int n_subs;
    unsigned char pub_rel[UI_MAX_ENDPOINTS];   /* each publisher's offered reliability (parallel to pubs[]) */
    unsigned char sub_rel[UI_MAX_ENDPOINTS];   /* each subscriber's requested reliability (parallel to subs[]) */

    double rate_hz;
    double last_age_s;
    double jitter_p90_ms;        /* p90 inter-arrival jitter (ms), smoothed over the topic's whole
                                     observed lifetime, not just the visible feed; < 0 = not enough
                                     samples yet (rides the data plane, so real only while subscribed) */

    int    reliable;             /* the DISPLAYED QoS: reliable iff every live endpoint (publisher
                                    offered + subscriber requested) is reliable, so a topic with
                                    only a reliable subscriber still reads reliable. */
    int    has_qos;              /* any endpoint (pub OR sub) declares a reliability, so the badge
                                    is meaningful; 0 = no endpoints yet, show a dash. */
    int    reliable_recommend;   /* the reliability to subscribe AS: reliable only if every
                                    publisher we consider offers it (a mix downgrades to best
                                    effort, since a reliable sub would refuse the best-effort
                                    publisher); gone/dropped publishers are ignored unless they
                                    are the only ones. -1 = no publishers (default best effort). */

    /* live subscription state (the explorer can join a topic's data plane on demand), for
       the tree status light: 0 = not subscribed (grey), 1 = subscribed & healthy (green),
       2 = subscribed but dropping/erroring (red). The feed reads live counts directly. */
    int    sub_state;
    /* the explorer's OWN role on this topic (peer lists above are discovery-only): whether we
       publish/subscribe and at what reliability, so the topic reflects what we advertise. */
    int    self_pub;
    int    self_pub_reliable;
    int    self_sub;
    int    self_sub_reliable;
} Topic;

typedef struct {
    char     t[16];
    LogLevel level;
    char     src[24];            /* node name or subsystem: transport, qos, shm, discovery */
    char     msg[160];
} LogEntry;

/* everything the UI draws from; counts let the tab bar show "Nodes 6" etc. */
typedef struct {
    const Machine  *machines; int n_machines;
    const Node     *nodes;    int n_nodes;
    const Topic    *topics;   int n_topics;
    const LogEntry *logs;     int n_logs;
} Dataset;

/* derived topic tree, built from the topic paths (see NUKLEAR_SPEC section 3).
   A node can be both a namespace and a topic, so topic is attached to interior
   nodes too. Builder lands with the Topics tab. */
typedef struct {
    const char  *name;       /* leaf segment label */
    const char  *path;       /* accumulated path (key for the expand/collapse set) */
    int          depth;
    int          is_branch;  /* has children: show a caret */
    int          has_topic;  /* selectable: show a status dot */
    int          open;
    const Topic *topic;      /* NULL for a pure namespace */
} TreeRow;

/* ---- status to color, mirroring data.js ---- */
static inline Clay_Color ui_state_color(const Palette *P, NodeState s){
    return s == NODE_ALIVE ? P->green : s == NODE_JOINING ? P->amber
         : s == NODE_DROPPED ? P->amber : P->gray;   /* dropped: silent, may resume; gone: dead */
}
static inline Clay_Color ui_level_color(const Palette *P, LogLevel l){
    return l == LVL_ERROR ? P->red : l == LVL_WARN ? P->amber : l == LVL_INFO ? P->dim : P->faint;
}
static inline Clay_Color ui_qos_color(const Palette *P, int reliable){
    return reliable ? P->green : P->amber;
}

static inline const Node *ui_node_by_id(const Dataset *D, const char *id){
    int i;
    for (i = 0; i < D->n_nodes; i++) if (!strcmp(D->nodes[i].id, id)) return &D->nodes[i];
    return NULL;
}
static inline const Topic *ui_topic_by_path(const Dataset *D, const char *path){
    int i;
    for (i = 0; i < D->n_topics; i++) if (!strcmp(D->topics[i].path, path)) return &D->topics[i];
    return NULL;
}

#endif /* UI_MODEL_H */
