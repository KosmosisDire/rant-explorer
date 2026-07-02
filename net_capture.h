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
    int      meta_stale;     /* 1 = peer advertises a newer blob version than the one we hold */
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
    int      publishing;     /* 1 = the explorer is publishing to this topic */
    int      reliable;       /* the live channel's reliability (the reliability we offer/request) */
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

/* Declare a publish interest on a topic without sending: creates (or reuses) the topic's
   publisher channel and advertises it, so subscribers match the explorer as a publisher.
   Used when a topic is created in the UI. Returns 1 ok. */
int  cap_declare_publish(Capture *cap, const char *topic);

/* Publish one message to a topic from the observer node. Creates (or reuses) a publisher
   for the topic; if we are also subscribed it rides the same channel (so one identity keeps
   one live channel), otherwise it publishes reliable to reach the most subscribers. The
   message is echoed into the topic's own feed (mine=1) so the sender sees it. Returns 1 ok. */
int  cap_publish(Capture *cap, const char *topic, const void *data, size_t len);

/* Publish a STRUCTURED message built from per-field value strings, one per top-level
   field of the topic's schema in field order (the publish form). Empty/NULL values keep
   the canonical default (zero); scalars parse by kind ("42", "-1.5", "true"); arrays take
   comma/space-separated numbers (at most the element count, the rest zero); struct fields
   stay default (no setter yet). Requires the topic's live channel to carry the schema
   (created while a publisher advertised one). Returns 1, or 0 with the reason logged. */
int  cap_publish_form(Capture *cap, const char *topic, const char *const *values, int n_values);

/* One captured message, oldest-first, copied out for the selected topic's feed. When the
   sender advertised a schema the message is REFLECTED into per-field rows (every depth,
   nested members flattened with their depth; decoded exactly as the writer declared it,
   no content guessing: a u8 array is an array) AND a one-line summary in preview[] whose
   arrays/structs are expanded inline (arrays up to 4 elements, structs up to 4 members
   recursed 2 levels deep; past either cap the rest folds to " .."/{...}). For a schema-less sender
   preview holds the raw payload head instead. uid identifies the message across snapshot
   rebuilds (for per-message expand/collapse UI state). */
#define CAP_MSG_FIELDS    24   /* fields captured per message (all depths) */
#define CAP_MSG_FIELD_VAL 96   /* rendered value text per field */

typedef struct {
    char    name[CAP_TOPIC_CAP];     /* field's own name */
    char    value[CAP_MSG_FIELD_VAL];/* rendered value ("42", "1.5", "[104 101 108 ..]", "{...}") */
    uint8_t depth;                   /* 0 = top level */
} CapMsgField;

typedef struct {
    double   t_s;                    /* seconds since the observer started */
    uint32_t uid;                    /* stable per-topic message id (expand/collapse key) */
    uint32_t len;                    /* true payload length */
    uint16_t preview_len;            /* bytes filled in preview[] */
    int      mine;                   /* 1 = we published it (local echo) */
    int      decoded;                /* 1 = fields[] holds the reflected decode */
    int      n_fields;               /* fields filled (capped to CAP_MSG_FIELDS) */
    int      total_fields;           /* fields the schema actually has */
    char     type_name[CAP_TOPIC_CAP];  /* schema root name when decoded, else "" */
    char     sender[CAP_NAME_CAP];   /* sending node's name */
    char     preview[CAP_MSG_PREVIEW];  /* decoded: one-line summary; raw: payload head */
    CapMsgField fields[CAP_MSG_FIELDS];
} CapFeedItem;

/* Copy up to `max` of `topic`'s most recent messages (oldest first) into out[] and
   return the count. Any of subscribed/error/reliable/n_msgs/n_drops may be NULL; they
   report the subscription's current state (reliable = the reliability it was created
   with). Returns 0 for an unknown / never-subscribed topic. */
int  cap_topic_feed(const Capture *cap, const char *topic, CapFeedItem *out, int max,
                    int *subscribed, int *error, int *reliable, uint32_t *n_msgs, uint32_t *n_drops);

/* --------------------------------------------------------------- topic schema
   A publisher's advertised message schema for a topic, decoded to plain types for the
   UI. Publishers advertise a schema hash (and usually the schema itself) in their
   announce metadata; cap_topic_schema scans the peers publishing `topic`, decodes the
   first advertised schema, and reports whether every publisher agrees on it. */
#define CAP_SCHEMA_FIELDS 24    /* top-level fields captured per schema */

typedef struct {
    char     name[CAP_TOPIC_CAP];  /* field's own name */
    char     type[24];             /* display type: "u64", "u8[256]", "struct" */
    uint8_t  depth;                /* 0 = top level; nested members are one deeper */
    uint32_t offset;               /* absolute byte offset in a message */
    uint32_t size;                 /* byte size of the field */
} CapSchemaField;

typedef struct {
    uint64_t hash;                 /* the schema's 64-bit identity */
    char     type_name[CAP_TOPIC_CAP];  /* root type name (when inlined) */
    char     from[CAP_NAME_CAP];   /* the publisher we read it from */
    uint32_t msg_size;             /* exact message size in bytes (when inlined) */
    int      inlined;              /* 1 = the wire bytes arrived and parsed; 0 = hash only */
    int      n_fields;             /* fields filled below (capped to CAP_SCHEMA_FIELDS) */
    int      total_fields;         /* fields the schema actually has */
    int      n_pubs_hash;          /* publishers advertising this same hash */
    int      hash_conflict;        /* 1 = publishers advertise DIFFERENT schemas for this topic */
    CapSchemaField fields[CAP_SCHEMA_FIELDS];
} CapSchema;

/* Fill *out with `topic`'s advertised schema; 1 if any publisher advertises one, else 0
   (out zeroed). Rebuilt from the live peer view on each call. */
int  cap_topic_schema(const Capture *cap, const char *topic, CapSchema *out);

#endif /* NET_CAPTURE_H */
