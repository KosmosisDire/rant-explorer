/* Display data model for the explorer. These structs mirror what dart.h exposes
   at runtime (shaped after the handoff DATA_MODEL); the UI renders from them and
   never holds widget state of its own. Store raw numbers, format at draw time.

   The sample dataset and the topic-tree builder are not populated here yet: the
   skeleton renders empty regions. Port handoff/source/data.js into a Dataset and
   wire net_capture's live peers in as the next step. */
#ifndef UI_MODEL_H
#define UI_MODEL_H

#define UI_MAX_ENDPOINTS 16

typedef enum { NODE_ALIVE, NODE_JOINING, NODE_GONE } NodeState;
typedef enum { LVL_ERROR, LVL_WARN, LVL_INFO, LVL_DEBUG } LogLevel;
typedef enum { QOS_BEST_EFFORT, QOS_RELIABLE } Reliability;
typedef enum { DUR_VOLATILE, DUR_TRANSIENT_LOCAL } Durability;

typedef struct {
    char id[16];
    char host[32];
    char ip[40];
    char os[32];
} Machine;

/* per-node discovery + transport internals, shown in Nodes -> Discovery & transport */
typedef struct {
    char   guid[24];
    char   proto[24];
    double announce_period_s;
    double last_announce_age_s;
    double heartbeat_period_s;
    double lease_s;
    int    frag_size_bytes;
    int    max_msg_bytes;
    int    disc_wire_max;
    char   transport[24];
    char   unicast[48];
    char   multicast[48];
    long   announces_sent;
    long   frags_tx;
    long   acknacks_rx;
    long   nacks_rx;          /* retransmit requests; flag red when high */
} DiscInfo;

typedef struct {
    char      id[32];
    char      name[32];
    int       machine;        /* index into machines[] */
    char      ip[40];
    int       pid;
    NodeState state;

    double uptime_s;
    double heartbeat_age_s;
    int    cpu_pct;
    long   mem_bytes;
    long   msgs_sent;
    long   msgs_recv;

    int pubs[UI_MAX_ENDPOINTS]; int n_pubs;   /* indices into topics[] */
    int subs[UI_MAX_ENDPOINTS]; int n_subs;

    DiscInfo disc;
} Node;

typedef struct {
    Reliability reliability;
    Durability  durability;
    int         history_depth;   /* KEEP_LAST n; -1 = KEEP_ALL */
    double      deadline_s;      /* < 0 = none */
} Qos;

typedef struct {
    char path[96];               /* "sensors/lidar/points", '/'-separated tree */
    Qos  qos;
    int  pubs[UI_MAX_ENDPOINTS]; int n_pubs;   /* node indices */
    int  subs[UI_MAX_ENDPOINTS]; int n_subs;

    double rate_hz;
    int    rate_on_event;
    long   size_bytes;
    double last_age_s;
    long   count;

    int    reliable;             /* qos.reliability == QOS_RELIABLE */
    int    drops;                /* valid only when reliable */

    char   preview[256];
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
    int          depth;
    int          is_branch;  /* has children: show a caret */
    int          has_topic;  /* selectable: show a status dot */
    int          open;
    const Topic *topic;      /* NULL for a pure namespace */
} TreeRow;

/* ---- status to color, mirroring data.js ---- */
static inline Clay_Color ui_state_color(const Palette *P, NodeState s){
    return s == NODE_ALIVE ? P->green : s == NODE_JOINING ? P->amber : P->gray;
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
