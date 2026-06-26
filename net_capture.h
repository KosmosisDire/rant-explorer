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
   A flat, plain-types view of the live discovery table, copied out for the UI
   (the UI TU has no DART/Win32 types). This is EVERYTHING a passive discovery
   observer can know about a peer: its name, advertised unicast locator + UDP
   fragment size, announce-blob version, and its pub/sub interest list (with each
   entry's offered/requested reliability). Node-internal facts (CPU, memory, msg
   counts, the node's own GUID, its transport AckNack counters, the discovery
   table IT holds) are NOT here: they never ride discovery, so the UI placeholders
   them. cap_snapshot() rebuilds the view each frame. */

#define CAP_NAME_CAP   33    /* DART_NODE_NAME_MAX (32) + NUL  */
#define CAP_TOPIC_CAP  65    /* DART_TOPIC_NAME_MAX (64) + NUL */
#define CAP_MAX_EP     32    /* pub or sub entries captured per node */
#define CAP_SNAP_NODES 64    /* mirrors CAP_MAX_PEERS */
#define CAP_LOG_LINE   160   /* bytes per observer log line */
#define CAP_SNAP_LOG   256   /* most-recent observer log lines exposed, newest first */

typedef enum { CAP_ST_ACTIVE = 0, CAP_ST_DROPPED = 1, CAP_ST_GONE = 2 } CapState;

typedef struct {
    char     name[CAP_TOPIC_CAP];
    uint16_t alias;          /* the advertiser's local channel index */
    int      reliable;       /* offered (pub) / requested (sub) reliability */
} CapEndpoint;

typedef struct {
    uint32_t id;             /* discovery local peer id (a local handle, NOT a GUID) */
    CapState state;
    int      have_meta;
    int      meta_version;   /* announce-blob version (6 or 7) */
    char     name[CAP_NAME_CAP];
    char     ip[40];         /* advertised unicast IP, dotted */
    uint16_t port;           /* advertised unicast data port */
    uint16_t frag;           /* advertised UDP fragment size */
    uint16_t meta_len;       /* size of the announce blob we received */
    int      n_pub; CapEndpoint pub[CAP_MAX_EP];
    int      n_sub; CapEndpoint sub[CAP_MAX_EP];
    unsigned updates;        /* times its announce changed (addr/interest) */
    double   observed_s;     /* seconds since first observed (observer view) */
    double   age_s;          /* seconds since its last announce change */
} CapNode;

typedef struct {
    int      n_nodes;
    CapNode  nodes[CAP_SNAP_NODES];
    int      n_log;                       /* lines filled in log[], newest first */
    char     log[CAP_SNAP_LOG][CAP_LOG_LINE];  /* observer event stream (UP/DOWN/REFUSED/meta) */
    uint16_t domain;         /* discovery domain being observed */
    char     group[40];      /* discovery multicast group */
    uint16_t disc_port;      /* discovery port */
} CapSnapshot;

void cap_snapshot(const Capture *cap, CapSnapshot *out);

#endif /* NET_CAPTURE_H */
