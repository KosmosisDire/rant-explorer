/* Public interface to the live DART discovery observer. The implementation
   (net_capture.c) is a separate translation unit: it pulls in
   dart_transport.h -> winsock2.h/windows.h, and isolating the DART/Win32 side
   from the SDL3/Clay UI side keeps each TU clean (SDL coexists with windows.h,
   so this is a tidiness choice now, not a hard collision as it was under raylib).

   The UI side includes only this header (plain types, no DART, no Win32). */
#ifndef NET_CAPTURE_H
#define NET_CAPTURE_H

#include <stdint.h>

typedef struct {
    uint16_t    domain;
    const char *group;
    uint16_t    port;
    const char *ifc;     /* multicast interface IP, or NULL for auto */
    const char *name;
} Config;

/* opaque to the UI: the fields are DART runtime pointers, owned by net_capture.c */
typedef struct {
    void *rt;
    void *mem;
} Capture;

void cap_defaults(Config *c);
int  cap_parse_args(int argc, char **argv, Config *c);   /* 1 = run, 0 = exit clean, -1 = bad args */
int  cap_start(Capture *cap, const Config *cfg);          /* 1 = ok, 0 = failed (non-fatal) */
int  cap_poll(Capture *cap);                              /* drain datagrams + pump timers */
void cap_stop(Capture *cap);

/* ------------------------------------------------------------------ snapshot
   A flat, plain-types view of the live peer table, copied out for the UI (the UI
   TU has no DART/Win32 types). The explorer runs a real DART NODE, so each peer is
   sourced through the node's peer API (dart_node_peers): the node decodes the
   announce-metadata overlay for us, so net_capture never parses it itself.

   Two origins are kept distinct on purpose. Discovery-level facts: the peer's name,
   advertised unicast locator, liveness, and how long we've observed it. Announce-
   metadata facts (the transport overlay the node hands back decoded): its UDP
   fragment size and pub/sub interest list, with each entry's offered/requested
   reliability. Node-internal facts (CPU, memory, msg counts, a peer's transport
   AckNack counters, the peer table IT holds) are NOT here: they never ride the wire,
   so the UI placeholders them. cap_snapshot() rebuilds the view each frame. */

#define CAP_NAME_CAP   33    /* DART_NODE_NAME_MAX (32) + NUL  */
#define CAP_TOPIC_CAP  65    /* DART_TOPIC_NAME_MAX (64) + NUL */
#define CAP_MAX_EP     32    /* pub or sub entries captured per node */
#define CAP_SNAP_NODES 64    /* mirrors CAP_MAX_PEERS */
#define CAP_LOG_LINE   160   /* bytes per observer log line */
#define CAP_SNAP_LOG   256   /* most-recent observer log lines exposed, newest first */
#define CAP_FEED_MAX   128   /* live messages retained per subscribed topic (ring) */
#define CAP_MSG_PREVIEW 120  /* payload bytes kept per message for the feed preview */
#define CAP_SNAP_SUBS  64    /* subscription-state entries exposed to the UI */

typedef enum { CAP_ST_ACTIVE = 0, CAP_ST_DROPPED = 1, CAP_ST_GONE = 2 } CapState;

typedef struct {
    char     name[CAP_TOPIC_CAP];
    uint16_t alias;          /* the advertiser's local channel index */
    int      reliable;       /* offered (pub) / requested (sub) reliability */
} CapEndpoint;

typedef struct {
    CapState state;
    int      have_meta;      /* 1 = the announce overlay was decoded (frag + interest below) */
    char     name[CAP_NAME_CAP];
    char     ip[40];         /* advertised unicast IP, dotted (discovery) */
    uint16_t port;           /* advertised unicast data port (discovery) */
    uint16_t frag;           /* advertised UDP fragment size (announce metadata) */
    uint16_t meta_len;       /* size of the announce overlay we received (announce metadata) */
    int      n_pub; CapEndpoint pub[CAP_MAX_EP];
    int      n_sub; CapEndpoint sub[CAP_MAX_EP];
    unsigned updates;        /* times its announce metadata changed (observer view) */
    double   observed_s;     /* seconds since first observed (observer view) */
    double   age_s;          /* seconds since its last announce change (observer view) */
} CapNode;

/* Per-topic subscription state, so the UI can colour each topic's status light:
   subscribed + healthy (green), subscribed but dropping/erroring (red), or absent =
   not subscribed (grey). The explorer subscribes on demand, so this is real. */
typedef struct {
    char     name[CAP_TOPIC_CAP];
    int      active;         /* 1 = a live subscription right now */
    int      error;          /* 1 = dropped messages or a QoS/oversize error */
    uint32_t n_msgs;         /* messages received on this subscription */
    uint32_t n_drops;        /* messages the reliable layer reported skipped */
} CapSubInfo;

typedef struct {
    int      n_nodes;
    CapNode  nodes[CAP_SNAP_NODES];
    int      n_log;                       /* lines filled in log[], newest first */
    char     log[CAP_SNAP_LOG][CAP_LOG_LINE];  /* observer event stream (UP/DOWN/REFUSED/meta) */
    int      n_subs;                      /* live subscriptions (active or with error history) */
    CapSubInfo subs[CAP_SNAP_SUBS];
    uint16_t domain;         /* discovery domain being observed */
    char     group[40];      /* discovery multicast group */
    uint16_t disc_port;      /* discovery port */
} CapSnapshot;

void cap_snapshot(const Capture *cap, CapSnapshot *out);

/* ------------------------------------------------------------------- live feed
   The explorer can join a topic's data plane on demand: cap_subscribe creates (or
   reactivates) a real DART subscriber for `topic`, so matching publishers start
   sending it data; cap_unsubscribe flips that channel inactive. reliable should
   mirror the topic's advertised reliability (a reliable sub gets drop reporting; a
   best-effort sub matches the widest set of publishers). Both return 1 on success.
   These mutate node state, so call them from the same thread as cap_poll. */
int  cap_subscribe(Capture *cap, const char *topic, int reliable);
int  cap_unsubscribe(Capture *cap, const char *topic);

/* One captured message, oldest-first, copied out for the selected topic's feed. */
typedef struct {
    double   t_s;                    /* seconds since the observer started */
    uint32_t len;                    /* true payload length */
    uint16_t preview_len;            /* bytes filled in preview[] */
    char     sender[CAP_NAME_CAP];   /* sending node's name */
    char     preview[CAP_MSG_PREVIEW];
} CapFeedItem;

/* Copy up to `max` of `topic`'s most recent messages (oldest first) into out[] and
   return the count. Any of subscribed/error/n_msgs/n_drops may be NULL; they report
   the subscription's current state. Returns 0 for an unknown / never-subscribed topic. */
int  cap_topic_feed(const Capture *cap, const char *topic, CapFeedItem *out, int max,
                    int *subscribed, int *error, uint32_t *n_msgs, uint32_t *n_drops);

#endif /* NET_CAPTURE_H */
