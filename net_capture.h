/* The public interface of the live observer. net_capture.c is its own translation unit
   owning DART and winsock, and the UI includes only this header of plain types. */
#ifndef NET_CAPTURE_H
#define NET_CAPTURE_H

#include <stdint.h>

typedef struct {
    uint16_t    domain;
    const char *group;
    uint16_t    port;
    const char *ifc;     /* pin to this interface IP, or NULL for every interface */
    const char *name;
} Config;

/* opaque to the UI: the fields are DART runtime pointers, owned by net_capture.c */
typedef struct {
    void *rt;
    void *mem;
} Capture;

void cap_defaults(Config *c);
int  cap_parse_args(int argc, char **argv, Config *c);   /* 1 = run, 0 = exit clean, -1 = bad */
int  cap_start(Capture *cap, const Config *cfg);   /* 1 = ok, 0 = failed but not fatal. Starts the
                                                      service thread */
int  cap_poll(Capture *cap);   /* once per UI frame: drain the subscribed topics'
                                  queues on this thread */
void cap_stop(Capture *cap);

/* The snapshot: a flat, plain types view of the live peer table copied out for the UI each
   frame. Only active peers appear, and node internal facts never ride the wire. */

#define CAP_NAME_CAP   33    /* DART_NODE_NAME_MAX (32) + NUL  */
#define CAP_TOPIC_CAP  65    /* DART_TOPIC_NAME_MAX (64) + NUL */
#define CAP_SNAP_NODES 64    /* mirrors CAP_MAX_PEERS */
#define CAP_LOG_LINE   160   /* bytes per observer log line */
#define CAP_SNAP_LOG   256   /* most-recent observer log lines exposed, newest first */
#define CAP_FEED_MAX   128   /* live messages retained per subscribed topic (ring) */
#define CAP_MSG_PREVIEW 120  /* payload bytes kept per message for the feed preview */
#define CAP_SNAP_SUBS  16000 /* subscription entries exposed to the UI, matching CAP_MAX_SUBS
                                in net_capture.c (spec/explorer.md sizes it) */

/* Entity kind, values mirroring DartEntityKind. The capture walks peers through the entity
   reflection, so pattern channels never reach the UI, an entity is one entry. */
enum {
    CAP_KIND_TOPIC = 0, CAP_KIND_FUNCTION, CAP_KIND_VARIABLE, CAP_KIND_TASK
};

typedef struct {
    char     name[CAP_TOPIC_CAP];   /* the ENTITY base name (never an @-mangled internal) */
    uint16_t index;          /* the advertiser's primary channel index (schema query key) */
    int      reliable;       /* offered (pub) / requested (sub) reliability */
    uint8_t  kind;           /* CAP_KIND_*: what this entity is */
    uint8_t  writable;       /* CAP_KIND_VARIABLE: a set channel is advertised */
    uint8_t  forceable;      /* CAP_KIND_VARIABLE: the owner permits force/unforce (allow_force) */
    uint8_t  cancellable;    /* CAP_KIND_TASK: the provider honors cancel, default on */
    uint8_t  exclusive;      /* CAP_KIND_TASK: declared serialization (attrs) */
    uint8_t  multi;          /* redundant providers intended (attrs) */
    uint8_t  incomplete;     /* a pattern half-pair (partner channel missing/unresolved) */
} CapEndpoint;

typedef struct {
    int      have_meta;      /* 1 = the announce overlay was decoded (frag + interest below) */
    int      meta_stale;     /* 1 = peer advertises a newer blob version than the one we hold */
    char     name[CAP_NAME_CAP];
    char     ip[40];         /* advertised unicast IP, dotted (discovery) */
    uint16_t port;           /* advertised unicast data port (discovery) */
    uint16_t frag;           /* advertised UDP fragment size (announce metadata) */
    uint16_t meta_len;       /* size of the announce overlay we received (announce metadata) */
    uint32_t rtt_us, rtt_jitter_us, rtt_min_us, rtt_samples;   /* our measured round trip to it */
    /* the peer's advertised entities grown to its topic count: a high water heap buffer
       owned by the snapshot and reused across frames */
    int      n_pub, pub_cap; CapEndpoint *pub;
    int      n_sub, sub_cap; CapEndpoint *sub;
    unsigned updates;        /* times its announce metadata changed (observer view) */
    double   observed_s;     /* seconds since first observed (observer view) */
    double   age_s;          /* seconds since its last announce change (observer view) */
} CapNode;

/* The mini value preview for the topic list's VALUE column (spec/explorer.md). std values
   mirror DartStdType, CAP_STD_NONE = not a standard type. */
enum {
    CAP_STD_NONE = 0,
    CAP_STD_FLOAT2, CAP_STD_FLOAT3, CAP_STD_FLOAT4,
    CAP_STD_DOUBLE2, CAP_STD_DOUBLE3, CAP_STD_DOUBLE4,
    CAP_STD_INT2, CAP_STD_INT3, CAP_STD_INT4,
    CAP_STD_QUATERNION,
    CAP_STD_COLOR,
    CAP_STD_RECT, CAP_STD_RECTI,
    CAP_STD_POSE, CAP_STD_TWIST,
    CAP_STD_GEOPOINT,
    CAP_STD_UUID,
    CAP_STD_TIMESTAMP, CAP_STD_DURATION,
    CAP_STD_MATRIX3X3, CAP_STD_MATRIX4X4,
    CAP_STD_URI,
    CAP_STD_IMAGE, CAP_STD_VIDEOFRAME, CAP_STD_EXTERNAL_VIDEO_STREAM
};

#define CAP_MINI_TEXT 48   /* bytes of mini-preview text (the UI truncates to its column) */
typedef struct {
    int     have;                 /* 1 = a value has been rendered (needs a live subscription) */
    uint8_t std;                  /* CAP_STD_* of the newest value's root type */
    char    text[CAP_MINI_TEXT];  /* the compact one-line rendering ("" = nothing printable) */
    uint8_t rgba[4];              /* CAP_STD_COLOR: the value's bytes (sRGB, straight alpha) */
    uint8_t mat_n;                /* CAP_STD_MATRIX*: dimension (3 or 4), else 0 */
    float   cells[16];            /* CAP_STD_MATRIX*: row-major values (n*n filled) */
} CapMiniPreview;

/* Per topic subscription state, so the UI can colour each topic's status light: healthy
   green, dropping or erroring red, absent grey. */
typedef struct {
    char     name[CAP_TOPIC_CAP];
    int      active;         /* 1 = a live subscription right now */
    int      publishing;     /* 1 = the explorer is publishing to this topic */
    int      reliable;       /* the live topic's reliability (the reliability we offer/request) */
    int      error;          /* 1 = dropped messages or a QoS/oversize error */
    uint32_t n_msgs;         /* messages received on this subscription */
    uint32_t n_drops;        /* messages the reliable layer reported skipped */
    double   rate_hz;   /* publish rate across the stored feed window, 0 = too few to measure */
    double   last_age_s;     /* seconds since the newest stored message, below 0 = none received */
    double   jitter_p90_ms;  /* p90 of inter arrival jitter in ms, smoothed over the topic's
                                whole lifetime. Below 0 = not enough samples yet */
    CapMiniPreview mini;     /* newest value, compact (the topic list's VALUE column) */
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
/* Free the grown per node entity buffers cap_snapshot allocated into out. The caller owns
   the struct. Call once at shutdown. */
void cap_snapshot_free(CapSnapshot *out);

/* The live feed: cap_subscribe creates or reactivates a real subscriber for topic and
   cap_unsubscribe flips it inactive. Both return 1 and must run on the cap_poll thread. */
int  cap_subscribe(Capture *cap, const char *topic, int reliable);
int  cap_unsubscribe(Capture *cap, const char *topic);

/* Declare a publish interest on a topic without sending, so subscribers match the
   explorer as a publisher. Used when a topic is created in the UI. Returns 1 ok. */
int  cap_declare_publish(Capture *cap, const char *topic);

/* Publish one message from the observer node, reusing a subscription's topic when one
   exists. The message is echoed into the topic's own feed with mine = 1. Returns 1 ok. */
int  cap_publish(Capture *cap, const char *topic, const void *data, size_t len);

/* Publish a structured message built from per field value strings in field order, the
   publish form. Empty keeps the default. Returns 1, or 0 with the reason logged. */
int  cap_publish_form(Capture *cap, const char *topic, const char *const *values, int n_values);

/* Live per field validation for the publish form, advisory with no side effects: valid[i]
   is 1 when values[i] parses or is empty. Returns 1 if a schema was found. */
int  cap_form_validate(const Capture *cap, const char *topic, const char *const *values,
                       int n_values, unsigned char *valid);

/* One captured message copied out for the selected topic's feed: reflected into per field
   rows plus a one line preview when the sender had a schema, else the raw head. */
#define CAP_MSG_FIELDS    24   /* fields captured per message (all depths) */
#define CAP_MSG_FIELD_VAL 96   /* rendered value text per field */

typedef struct {
    char    name[CAP_TOPIC_CAP];     /* field's own name */
    char    value[CAP_MSG_FIELD_VAL];/* rendered value ("42", "1.5", "[104 101 108 ..]", "{...}") */
    uint8_t depth;                   /* 0 = top level */
} CapMsgField;

typedef struct {
    uint64_t wall_us;                /* the write time: the publisher's wall clock when it wrote the
                                        message, us since the Unix epoch. Our arrival when stamped = 0 */
    int      stamped;   /* 1 = wall_us is the writer's own stamp, 0 = the publisher opted
                           out and it is our arrival time */
    uint32_t uid;                    /* stable per-topic message id (expand/collapse key) */
    uint32_t len;                    /* true payload length */
    uint16_t preview_len;            /* bytes filled in preview[] */
    int      mine;                   /* 1 = we published it (local echo) */
    int      forced;   /* VARIABLE value: the owner published it forced */
    int      call_status;            /* FUNCTION reply: its DartCallStatus, nonzero = the call
                                        failed. 0 on every other entry */
    char     call_msg[256];          /* FUNCTION reply: the response message, the provider's text or
                                        the default status text. Sized to DART_CALL_MSG_MAX + 1 */
    int      decoded;                /* 1 = fields[] holds the reflected decode */
    int      n_fields;               /* fields filled (capped to CAP_MSG_FIELDS) */
    int      total_fields;           /* fields the schema actually has */
    char     type_name[CAP_TOPIC_CAP];  /* schema root name when decoded, else "" */
    char     sender[CAP_NAME_CAP];   /* sending node's name */
    char     preview[CAP_MSG_PREVIEW];  /* decoded: a one line summary. raw: the payload head */
    CapMsgField fields[CAP_MSG_FIELDS];
} CapFeedItem;

/* Copy up to max of topic's most recent messages, oldest first, into out[] and return the
   count. The optional pointers report the subscription's state. 0 for an unknown topic. */
int  cap_topic_feed(const Capture *cap, const char *topic, CapFeedItem *out, int max,
                    int *subscribed, int *error, int *reliable, uint32_t *n_msgs, uint32_t *n_drops);

/* Drop every message stored for topic plus the history derived from them. The
   subscription stays live. Returns 1 if the topic was known. */
int  cap_topic_clear(Capture *cap, const char *topic);

/* The topic schema: any endpoint is authoritative about its own, and cap_topic_schema
   shows the widest compatible one, preferring a publisher. Only a conflict sets hash_conflict. */
#define CAP_SCHEMA_FIELDS 32    /* fields captured per schema, with headroom for standard
                                   composites that flatten to about 10 each */
#define CAP_ENUM_VARIANTS 16   /* enum option names carried per field, for the form dropdown */
#define CAP_ENUM_NAME     28    /* bytes per enum option name (truncated) */

/* A field's type kind, values mirroring DartSchemaTypeKind so the UI can pick a type aware
   input. elem is an array's element kind or an enum's backing kind. */
enum {
    CAP_K_U8 = 0, CAP_K_U16, CAP_K_U32, CAP_K_U64,
    CAP_K_I8, CAP_K_I16, CAP_K_I32, CAP_K_I64,
    CAP_K_F32, CAP_K_F64, CAP_K_BOOL,
    CAP_K_ARR, CAP_K_STRUCT, CAP_K_STR,
    CAP_K_VSTR, CAP_K_VARR, CAP_K_MAP,   /* variable kinds: live-sized, ride the message tail */
    CAP_K_ENUM,   /* a named integer mirroring DART_ENUM, a dropdown of variants */
    CAP_K_NAMED                          /* a nominal tag, unwrapped into type_name so a field never
                                            reports it as its own kind */
};

typedef struct {
    char     name[CAP_TOPIC_CAP];  /* field's own name */
    char     type[32];             /* display type: "u64", "u8[256]", "Float3[4]", "map", "Pose" */
    char     type_name[24];        /* the field type's NAME ("Pose"), "" when anonymous */
    char     elem_name[24];        /* an array ELEMENT type's name ("Float3"), "" when anonymous */
    uint8_t  kind;   /* the CAP_K_* of the field itself, a named type reports what it wraps */
    uint8_t  elem;   /* the CAP_K_* of an array's element or an enum's backing, else 0 */
    uint16_t count;                /* array element count (kind == CAP_K_ARR), else 0 */
    uint16_t str_cap;              /* string capacity (STR field or STR-element array), else 0 */
    uint16_t arr_parent;           /* flat index of the enclosing struct ARRAY, 0xFFFF for none */
    uint8_t  depth;                /* 0 = top level, nested members one deeper */
    uint32_t offset;               /* the absolute byte offset in a message, 0 for variable kinds */
    uint32_t size;                 /* the byte size of the field, 0 for variable kinds */
    uint32_t elem_size;            /* bytes of one array element, else 0 */
    uint8_t  n_variants;           /* ENUM: option count (capped to CAP_ENUM_VARIANTS), else 0 */
    char     variants[CAP_ENUM_VARIANTS][CAP_ENUM_NAME];   /* ENUM: the option names */
} CapSchemaField;

typedef struct {
    uint64_t hash;                 /* the schema's 64-bit identity */
    char     type_name[CAP_TOPIC_CAP];  /* root type name (when inlined) */
    char     from[CAP_NAME_CAP];   /* the endpoint we read it from */
    uint32_t msg_size;             /* exact message size in bytes (when inlined) */
    int      inlined;              /* 1 = the wire bytes arrived and parsed, 0 = hash only */
    int      n_fields;             /* fields filled below (capped to CAP_SCHEMA_FIELDS) */
    int      total_fields;         /* fields the schema actually has */
    int      n_advertisers;        /* endpoints (pub or sub) advertising a compatible schema */
    int      hash_conflict;        /* 1 = endpoints advertise structurally INCOMPATIBLE schemas
                                      (a subset/superset is compatible, not a conflict) */
    CapSchemaField fields[CAP_SCHEMA_FIELDS];
} CapSchema;

/* Fill *out with topic's advertised schema, 1 if any endpoint advertises one. For a
   function this is the request schema. Rebuilt from the live peer view on each call. */
int  cap_topic_schema(const Capture *cap, const char *topic, CapSchema *out);

/* A function or task entity's response schema. 0 for every other kind or when no
   provider advertises one. */
int  cap_topic_rsp_schema(const Capture *cap, const char *topic, CapSchema *out);

/* A task entity's progress schema. 0 for every other kind. */
int  cap_topic_prg_schema(const Capture *cap, const char *topic, CapSchema *out);

/* Spell topic's advertised schema as compile ready DSL text: which 0 is the primary, 1 a
   response, 2 a task progress. Returns the length, or 0 when none is inlinable. */
int  cap_topic_schema_dsl(const Capture *cap, const char *topic, int which, char *out, size_t out_cap);

/* Variable force control on a writable variable: force pins the value built from the form
   values until unforce, both opaque ops on the set channel. Return 1 on send. */
int  cap_variable_force_form(Capture *cap, const char *topic, const char *const *values, int n_values);
int  cap_variable_unforce(Capture *cap, const char *topic);

/* A variable's newest value re spelled in the publish form's syntax, one entry per flat
   field in cap_topic_schema order. name confirms the field really is this value's. */
#define CAP_FORM_VAL 40    /* value text per field (mirrors the explorer's form box) */

typedef struct {
    char name[CAP_TOPIC_CAP];   /* the field's own name (cap_topic_schema's label for it) */
    char value[CAP_FORM_VAL];   /* form syntax, "" = nothing to fill in */
} CapFormValue;

/* Copy up to max of topic's newest variable value into out[] and return the count. A field
   with no form input, or one that would not fit, comes back "" rather than truncated. */
int  cap_variable_form_values(const Capture *cap, const char *topic, CapFormValue *out, int max);

/* 1 while a send on topic is parked waiting for its match to resolve. cap_poll flushes it
   on resolve and drops it loudly after a few seconds, the UI shows a sending indicator. */
int  cap_topic_send_pending(const Capture *cap, const char *topic);

/* Task control: subscribing a task opens a raw best effort tap on its @prg channel, so the
   explorer sees every live run, and a call from the form lands its state here. */

/* the explorer's own in-flight call on a task topic */
enum { CAP_TCALL_IDLE = 0, CAP_TCALL_SENT, CAP_TCALL_RUNNING, CAP_TCALL_DONE };
typedef struct {
    int      phase;              /* CAP_TCALL_*: SENT until the RUNNING ack (or a reply) */
    uint32_t call_id;            /* the call's id (the cancel key) */
    uint32_t progress_count;     /* progress payloads received for this call */
    int      call_status;        /* terminal DartCallStatus (phase DONE) */
    char     call_msg[256];      /* terminal response message ("" = none) */
    int      has_progress;       /* latest holds the newest decoded progress update */
    CapFeedItem latest;          /* that update, decoded like any feed message */
} CapTaskCall;
/* Fill *out with the explorer's own call state on topic, 0 for a non task or never
   called topic with out zeroed and phase IDLE. */
int  cap_task_call_state(const Capture *cap, const char *topic, CapTaskCall *out);
/* Cancel the explorer's OWN in-flight call (phase SENT/RUNNING). Cooperative: the terminal
   status is the answer. Returns 1 on send (0: nothing in flight, or provider no_cancel). */
int  cap_task_cancel_call(Capture *cap, const char *topic);

/* one observed live run on a task topic (from the raw @prg tap), the explorer's own
   call excluded (that one is cap_task_call_state) */
#define CAP_TASK_RUNS 16          /* live-run rows kept (oldest evicted when full) */
typedef struct {
    char     caller[CAP_NAME_CAP];    /* caller resolved via discovery uuids, or lo-32 hex */
    char     provider[CAP_NAME_CAP];  /* the node working the call */
    uint32_t caller_lo;               /* the caller's uuid low-32 (the wire discriminator) */
    uint32_t call_id;
    uint32_t provider_id;             /* provider peer id (the cancel destination) */
    uint32_t updates;                 /* progress payloads seen */
    double   age_s;                   /* seconds since the last update */
    uint32_t last_len;                /* newest update's payload length */
    char     preview[CAP_MSG_PREVIEW];/* newest update, one line (decoded when typed) */
} CapTaskRun;
/* Copy up to max of `topic`'s observed live runs (newest-updated first). Rows age out
   ~30s after their last update. Returns the count. */
int  cap_task_runs(const Capture *cap, const char *topic, CapTaskRun *out, int max);
/* Third party cancel: craft the raw CANCEL op naming the run's caller on the task's request
   channel, directed at its provider. Never acked, the run's outcome answers. 1 on send. */
int  cap_task_cancel_run(Capture *cap, const char *topic, uint32_t provider_id,
                         uint32_t caller_lo, uint32_t call_id);

/* Node logs: the explorer subscribes to all three @dart/log topics at start, so each
   node's lines collect here, including the history logged before the explorer joined. */
#define CAP_NODELOG_TEXT 192          /* bytes kept per line (longer lines truncate) */
#define CAP_NODELOG_MAX  512          /* lines retained across all nodes (ring) */
typedef struct {
    char     node[CAP_NAME_CAP];      /* the publishing node's name */
    uint8_t  level;                   /* 0 error, 1 warn, 2 info (DartLogLevel) */
    uint64_t wall_us;                 /* the sender's wall clock, us since the Unix epoch */
    char     text[CAP_NODELOG_TEXT];
} CapNodeLogLine;
/* Copy up to max lines (NEWEST FIRST) into out[]: lines from `node_name` (NULL = every
   node) whose level bit (1 << level) is set in level_mask. Returns the count. */
int  cap_node_log(const Capture *cap, const char *node_name, unsigned level_mask,
                  CapNodeLogLine *out, int max);

/* Node runtime stats: cap_meta_watch names the node to poll once per second, NULL stops
   it, and cap_meta_stats copies out the latest decoded snapshot. */
#define CAP_META_TOPICS 64            /* per-topic counter rows kept from the snapshot */
typedef struct {
    char     name[CAP_TOPIC_CAP];
    uint64_t tx_msgs, tx_bytes, rx_msgs, rx_bytes;
    uint32_t subs, pubs;              /* matched subscribers / publishers at the node */
    uint32_t drops;                   /* its consumer-queue drops */
} CapMetaTopic;
#define CAP_META_PEERS 32             /* per-peer rows kept from the snapshot */
typedef struct {
    char     name[CAP_NAME_CAP];
    int      active;
    uint32_t publish_to, receive_from;   /* matched lanes toward / from that peer */
    uint32_t rtt_us, rtt_jitter_us, rtt_min_us, rtt_samples;   /* the node's round trip to it */
} CapMetaPeer;
typedef struct {
    int      valid;                   /* >= 1 snapshot decoded for the watched node */
    int      failing;                 /* consecutive unanswered polls (endpoint absent/slow) */
    double   age_s;                   /* seconds since the snapshot arrived, below 0 = never */
    char     node[CAP_NAME_CAP];      /* the watched node's name */
    /* node section */
    double   uptime_s;
    uint64_t mem_in_use, mem_peak, alloc_calls;
    uint64_t bp_waited_us;
    uint32_t bp_waits, evicted_unsent;
    uint32_t peers, max_peers, topics, max_topics;
    uint32_t shm_tx, shm_rx;
    uint32_t last_error;              /* DartErrorKind (0 = none) */
    char     last_error_text[128];
    /* proc section: absent where the node's platform cannot measure (DART_PROC_STATS off) */
    int      have_proc;
    int      have_cpu;
    uint64_t pid, cpu_us, rss, peak_rss;
    /* the ESP MALLOC_CAP_DEFAULT heap, zero when the platform reports no heap geometry */
    uint64_t heap_total, heap_free, heap_min_free, heap_largest_free_block;
    double   cpu_pct;   /* CPU%% between the last two polls, below 0 until two arrived */
    /* topics section */
    int      n_topic_rows;
    CapMetaTopic topic_rows[CAP_META_TOPICS];
    /* peers section: the node's own view of its peers (its RTT to each) */
    int      n_peer_rows;
    CapMetaPeer peer_rows[CAP_META_PEERS];
} CapMetaStats;
void cap_meta_watch(Capture *cap, const char *node_name);
int  cap_meta_stats(const Capture *cap, CapMetaStats *out);   /* 1 = watching (read out->valid) */

#endif /* NET_CAPTURE_H */
