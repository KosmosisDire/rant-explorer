/* The display data model. The UI renders from these structs and holds no widget state of
   its own. Every field is wire data or a real observer side derivation of it. */
#ifndef UI_MODEL_H
#define UI_MODEL_H

/* Two axes sized apart: NODE_TOPICS is the topics one node may publish or subscribe, and
   ENDPOINTS the nodes on one topic, bounded by the node count. */
#define UI_MAX_NODE_TOPICS 16000
#define UI_MAX_ENDPOINTS      64

typedef enum { NODE_ALIVE, NODE_JOINING } NodeState;   /* dropped/gone peers never reach the UI */
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
    /* the link as measured from here: our node's reliable traffic to this peer (RTT) */
    uint32_t rtt_us, rtt_jitter_us, rtt_min_us, rtt_samples;
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
    /* this node's own offered or requested reliability per endpoint, parallel to
       pubs[] and subs[]. 1 = reliable, 0 = best effort */
    unsigned char pub_rel[UI_MAX_NODE_TOPICS];
    unsigned char sub_rel[UI_MAX_NODE_TOPICS];

    DiscInfo disc;
} Node;

typedef struct {
    Reliability reliability;
} Qos;

typedef struct {
    char path[96];   /* "sensors/lidar/points". The tree groups segments split on '/' or '.' */
    int  kind;                   /* CAP_KIND_*: plain topic, or a function/variable/task ENTITY */
    int  writable;               /* variable: an owner advertises a set channel */
    int  forceable;   /* variable: an owner advertises allow_force */
    int  cancellable;   /* task: the provider honors cancel, which gates the cancel buttons */
    int  exclusive;              /* task: declared serialization (attrs) */
    int  multi;                  /* task: redundant providers intended (attrs) */
    int  incomplete;             /* pattern half-pair (a diagnosable misadvertisement) */
    Qos  qos;
    int  pubs[UI_MAX_ENDPOINTS]; int n_pubs;   /* node indices */
    int  subs[UI_MAX_ENDPOINTS]; int n_subs;
    unsigned char pub_rel[UI_MAX_ENDPOINTS];   /* each publisher's offered reliability */
    unsigned char sub_rel[UI_MAX_ENDPOINTS];   /* each subscriber's requested reliability */

    double rate_hz;
    double last_age_s;
    double jitter_p90_ms;        /* p90 inter arrival jitter in ms over the topic's whole observed
                                    lifetime. Below 0 = not enough samples, real only while subscribed */
    CapMiniPreview mini;   /* the newest received value, compact, for the tree's VALUE column */

    int    reliable;             /* the displayed QoS: reliable iff every live endpoint is reliable,
                                    so a lone reliable subscriber still reads reliable */
    int    has_qos;              /* any endpoint declares a reliability so the badge is meaningful,
                                    0 = no endpoints yet, show a dash */
    int    reliable_recommend;   /* the reliability to subscribe as: reliable only if every
                                    publisher offers it, since a reliable sub refuses. -1 = none */

    /* live subscription state for the tree light: 0 = not subscribed, grey. 1 = subscribed
       and healthy, green. 2 = dropping or erroring, red. */
    int    sub_state;
    /* the explorer's OWN role on this topic (peer lists above are discovery-only): whether we
       publish/subscribe and at what reliability, so the topic reflects what we advertise. */
    int    self_pub;
    int    self_pub_reliable;
    int    self_sub;
    int    self_sub_reliable;
} Topic;

/* everything the UI draws from. The counts let the tab bar show "Nodes 6" */
typedef struct {
    const Machine  *machines; int n_machines;
    const Node     *nodes;    int n_nodes;
    const Topic    *topics;   int n_topics;
} Dataset;

/* the derived topic tree built from the topic paths. A node can be both a namespace and
   a topic, so topic is attached to interior nodes too. */
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
    return s == NODE_ALIVE ? P->green : P->amber;   /* joining: announce blob still settling */
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
