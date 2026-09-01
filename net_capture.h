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
    const char *ifc;     /* pin to this interface IP, or NULL for every interface */
    const char *name;
} Config;

/* opaque to the UI: the fields are DART runtime pointers, owned by net_capture.c */
typedef struct {
    void *rt;
    void *mem;
} Capture;

void cap_defaults(Config *c);
int  cap_parse_args(int argc, char **argv, Config *c);   /* 1 = run, 0 = exit clean, -1 = bad args */
int  cap_start(Capture *cap, const Config *cfg);          /* 1 = ok, 0 = failed (non-fatal); starts the
                                                             node's service thread (network at its own
                                                             cadence, independent of the frame rate) */
int  cap_poll(Capture *cap);                              /* once per UI frame: drain the subscribed
                                                             topics' consumer queues on THIS thread */
void cap_stop(Capture *cap);

/* ------------------------------------------------------------------ snapshot
   A flat, plain-types view of the live peer table, copied out for the UI (the UI
   TU has no DART/Win32 types). The explorer runs a real DART NODE, so each peer is
   sourced through the node's peer API (dart_node_peers): the node decodes the
   announce-metadata overlay for us, so net_capture never parses it itself.

   Only ACTIVE peers appear: a peer the node reports dropped or gone leaves the
   snapshot (and so the UI) on the next rebuild.

   Two origins are kept distinct on purpose. Discovery-level facts: the peer's name,
   advertised unicast locator, and how long we've observed it. Announce-
   metadata facts (the transport overlay the node hands back decoded): its UDP
   fragment size and pub/sub interest list, with each entry's offered/requested
   reliability. Node-internal facts (CPU, memory, msg counts, a peer's transport
   AckNack counters, the peer table IT holds) are NOT here: they never ride the wire,
   so the UI placeholders them. cap_snapshot() rebuilds the view each frame. */

#define CAP_NAME_CAP   33    /* DART_NODE_NAME_MAX (32) + NUL  */
#define CAP_TOPIC_CAP  65    /* DART_TOPIC_NAME_MAX (64) + NUL */
#define CAP_SNAP_NODES 64    /* mirrors CAP_MAX_PEERS */
#define CAP_LOG_LINE   160   /* bytes per observer log line */
#define CAP_SNAP_LOG   256   /* most-recent observer log lines exposed, newest first */
#define CAP_FEED_MAX   128   /* live messages retained per subscribed topic (ring) */
#define CAP_MSG_PREVIEW 120  /* payload bytes kept per message for the feed preview */
#define CAP_SNAP_SUBS  16000 /* subscription-state entries exposed to the UI (== CAP_MAX_SUBS in
                                net_capture.c, matching the ~13k announce topic ceiling): every
                                subscribed topic shows live data in the table, no artificial limit.
                                CapSubInfo is ~130 B, so the fixed array is ~2 MB (not the feed
                                itself: the per-topic message ring is lazily allocated, see CapSub). */

/* ENTITY kind, values mirroring DartEntityKind (patterns/core.h). The capture layer walks
   peers through the canonical entity reflection (dart_node_peer_entity_next), so pattern
   channels (f@req / f@rsp / t@prg / v@set) never reach the UI: a function, task or variable
   is ONE entry under its base name, everything else a plain topic. */
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
    uint8_t  cancellable;    /* CAP_KIND_TASK: the provider honors cancel (attrs; default on) */
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
    /* the peer's advertised entities, grown to its actual topic count (no fixed ceiling): a
       high-water heap buffer owned by the snapshot, reused across frames. A typical node
       advertises a handful; a many-topic node grows this once and it stays. */
    int      n_pub, pub_cap; CapEndpoint *pub;
    int      n_sub, sub_cap; CapEndpoint *sub;
    unsigned updates;        /* times its announce metadata changed (observer view) */
    double   observed_s;     /* seconds since first observed (observer view) */
    double   age_s;          /* seconds since its last announce change (observer view) */
} CapNode;

/* ---- mini value preview (the topic list's VALUE column) ----------------------------
   The newest received value on a subscribed topic, rendered compactly at intake. A
   standard-type root gets a type-aware one-liner ("1.2, -0.5, 3.1", "#FF8800",
   "RTSP cam-front"), plus the raw value for the kinds the UI draws instead of prints
   (a Color swatch, a Matrix heat grid). Otherwise only a bare-type root (a single
   value) or a raw payload's printable head is shown; a plain struct's text stays ""
   (a full serialization never fits the column). std values mirror DartStdType
   (net_capture.c static-asserts the mirror); CAP_STD_NONE = not a standard type. */
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

/* Per-topic subscription state, so the UI can colour each topic's status light:
   subscribed + healthy (green), subscribed but dropping/erroring (red), or absent =
   not subscribed (grey). The explorer subscribes on demand, so this is real. */
typedef struct {
    char     name[CAP_TOPIC_CAP];
    int      active;         /* 1 = a live subscription right now */
    int      publishing;     /* 1 = the explorer is publishing to this topic */
    int      reliable;       /* the live topic's reliability (the reliability we offer/request) */
    int      error;          /* 1 = dropped messages or a QoS/oversize error */
    uint32_t n_msgs;         /* messages received on this subscription */
    uint32_t n_drops;        /* messages the reliable layer reported skipped */
    double   rate_hz;        /* publish rate across the stored feed window; 0 = too few to measure */
    double   last_age_s;     /* seconds since the newest stored message; < 0 = none received */
    double   jitter_p90_ms;  /* p90 of inter-arrival jitter (ms), smoothed by an online estimator
                                 over the topic's WHOLE lifetime (not just the stored feed ring, which
                                 overwrites); < 0 = not enough samples yet */
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
/* Free the grown per-node entity buffers cap_snapshot allocated into `out` (net_capture
   owns them; the caller owns the CapSnapshot struct). Call once at shutdown. */
void cap_snapshot_free(CapSnapshot *out);

/* ------------------------------------------------------------------- live feed
   The explorer can join a topic's data plane on demand: cap_subscribe creates (or
   reactivates) a real DART subscriber for `topic`, so matching publishers start
   sending it data; cap_unsubscribe flips that topic inactive. reliable should
   mirror the topic's advertised reliability (a reliable sub gets drop reporting; a
   best-effort sub matches the widest set of publishers). Both return 1 on success.
   These mutate node state, so call them from the same thread as cap_poll. */
int  cap_subscribe(Capture *cap, const char *topic, int reliable);
int  cap_unsubscribe(Capture *cap, const char *topic);

/* Declare a publish interest on a topic without sending: creates (or reuses) the topic's
   publisher topic and advertises it, so subscribers match the explorer as a publisher.
   Used when a topic is created in the UI. Returns 1 ok. */
int  cap_declare_publish(Capture *cap, const char *topic);

/* Publish one message to a topic from the observer node. Creates (or reuses) a publisher
   for the topic; if we are also subscribed it rides the same topic (so one identity keeps
   one live topic), otherwise it publishes reliable to reach the most subscribers. The
   message is echoed into the topic's own feed (mine=1) so the sender sees it. Returns 1 ok. */
int  cap_publish(Capture *cap, const char *topic, const void *data, size_t len);

/* Publish a STRUCTURED message built from per-field value strings, one per top-level
   field of the topic's schema in field order (the publish form). Empty/NULL values keep
   the canonical default (zero); scalars parse by kind ("42", "-1.5", "true"); arrays take
   comma/space-separated numbers (at most the element count, the rest zero); struct fields
   stay default (no setter yet). Requires the topic's live topic to carry the schema
   (created while a publisher advertised one). Returns 1, or 0 with the reason logged. */
int  cap_publish_form(Capture *cap, const char *topic, const char *const *values, int n_values);

/* Live per-field validation for the publish form (advisory, no side effects): parse each
   values[i] against `topic`'s advertised schema exactly as cap_publish_form would, writing
   1 (parses, or empty = keeps the default) or 0 (would be rejected) into valid[0..n-1].
   One schema parse per call, not per field. Returns 1 if a schema was found (results are
   meaningful), 0 otherwise (all fields left marked valid). */
int  cap_form_validate(const Capture *cap, const char *topic, const char *const *values,
                       int n_values, unsigned char *valid);

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
    uint64_t wall_us;                /* WRITE time: the publisher's own wall clock when it wrote the
                                        message, us since the Unix epoch (DartMsg.written_us, stamped
                                        at the writer's commit point, so it survives repair, replay
                                        and queueing). Being the writer's clock, a replayed message
                                        reads older than this observer and a clock-skewed host reads
                                        shifted: show it as a time of day so that is visible.
                                        Holds OUR arrival when stamped = 0. */
    int      stamped;                /* 1 = wall_us is the writer's own stamp; 0 = the publisher
                                        opted out (qos.no_timestamp) and it is our arrival time */
    uint32_t uid;                    /* stable per-topic message id (expand/collapse key) */
    uint32_t len;                    /* true payload length */
    uint16_t preview_len;            /* bytes filled in preview[] */
    int      mine;                   /* 1 = we published it (local echo) */
    int      forced;                 /* VARIABLE value: the owner published it FORCED (prefix flag) */
    int      call_status;            /* FUNCTION reply: its DartCallStatus (nonzero = the call
                                        failed); 0 on every other entry */
    char     call_msg[256];          /* FUNCTION reply: the response message (DartResponse.message:
                                        the provider's text, or default status text on a failure;
                                        "" on an OK reply that carried none and on non-replies).
                                        Sized to DART_CALL_MSG_MAX + 1. */
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

/* Drop every message stored for `topic` plus the history derived from them: the msg/drop
   counts, the error mark, and the rate/jitter estimates. The subscription itself stays
   live (the feed restarts from the next message). Returns 1 if the topic was known. */
int  cap_topic_clear(Capture *cap, const char *topic);

/* --------------------------------------------------------------- topic schema
   A topic's advertised message schema, decoded to plain types for the UI. Any endpoint
   is authoritative about its own schema: publishers advertise the shape they send, and a
   subscriber-in-charge topic advertises the shape it expects a generic publisher to fill.
   cap_topic_schema scans every peer that publishes OR subscribes to `topic` and shows the
   widest compatible schema (preferring a publisher, which owns the wire). Endpoints whose
   schemas are subset-compatible (dart_schema_subset, the C matcher's rule) agree; only a
   structurally incompatible schema sets hash_conflict. */
#define CAP_SCHEMA_FIELDS 32    /* fields captured per schema (a standard composite like
                                   Pose flattens to ~10, so a schema embedding a few of
                                   them needs headroom over the old 24) */
#define CAP_ENUM_VARIANTS 16    /* enum option names carried per field (for the publish-form dropdown) */
#define CAP_ENUM_NAME     28    /* bytes per enum option name (truncated) */

/* A field's type kind, numeric values mirroring DartSchemaTypeKind so the UI (which sees
   no DART types) can branch a field onto a type-aware input control. .elem uses the same
   values for an array's element kind, or an ENUM's backing integer kind. */
enum {
    CAP_K_U8 = 0, CAP_K_U16, CAP_K_U32, CAP_K_U64,
    CAP_K_I8, CAP_K_I16, CAP_K_I32, CAP_K_I64,
    CAP_K_F32, CAP_K_F64, CAP_K_BOOL,
    CAP_K_ARR, CAP_K_STRUCT, CAP_K_STR,
    CAP_K_VSTR, CAP_K_VARR, CAP_K_MAP,   /* variable kinds: live-sized, ride the message tail */
    CAP_K_ENUM,                          /* named integer (mirrors DART_ENUM): a dropdown of variants */
    CAP_K_NAMED                          /* a nominal tag; reflection unwraps it into .type_name,
                                            so a field never reports this as its own .kind */
};

typedef struct {
    char     name[CAP_TOPIC_CAP];  /* field's own name */
    char     type[32];             /* display type: "u64", "u8[256]", "Float3[4]", "map", "Pose" */
    char     type_name[24];        /* the field type's NAME ("Pose"), "" when anonymous */
    char     elem_name[24];        /* an array ELEMENT type's name ("Float3"), "" when anonymous */
    uint8_t  kind;                 /* CAP_K_* of the field itself (a NAMED type reports what it wraps) */
    uint8_t  elem;                 /* CAP_K_* of an array's element (ARR/VARR) or an ENUM's backing, else 0 */
    uint16_t count;                /* array element count (kind == CAP_K_ARR), else 0 */
    uint16_t str_cap;              /* string capacity (STR field or STR-element array), else 0 */
    uint16_t arr_parent;           /* flat index of the enclosing struct ARRAY, 0xFFFF for none */
    uint8_t  depth;                /* 0 = top level; nested members are one deeper */
    uint32_t offset;               /* absolute byte offset in a message; 0 for variable kinds */
    uint32_t size;                 /* byte size of the field; 0 for variable kinds */
    uint32_t elem_size;            /* bytes of one array element, else 0 */
    uint8_t  n_variants;           /* ENUM: option count (capped to CAP_ENUM_VARIANTS), else 0 */
    char     variants[CAP_ENUM_VARIANTS][CAP_ENUM_NAME];  /* ENUM: option names (the dropdown choices) */
} CapSchemaField;

typedef struct {
    uint64_t hash;                 /* the schema's 64-bit identity */
    char     type_name[CAP_TOPIC_CAP];  /* root type name (when inlined) */
    char     from[CAP_NAME_CAP];   /* the endpoint we read it from */
    uint32_t msg_size;             /* exact message size in bytes (when inlined) */
    int      inlined;              /* 1 = the wire bytes arrived and parsed; 0 = hash only */
    int      n_fields;             /* fields filled below (capped to CAP_SCHEMA_FIELDS) */
    int      total_fields;         /* fields the schema actually has */
    int      n_advertisers;        /* endpoints (pub or sub) advertising a compatible schema */
    int      hash_conflict;        /* 1 = endpoints advertise structurally INCOMPATIBLE schemas
                                      (a subset/superset is compatible, not a conflict) */
    CapSchemaField fields[CAP_SCHEMA_FIELDS];
} CapSchema;

/* Fill *out with `topic`'s advertised schema; 1 if any endpoint advertises one, else 0
   (out zeroed). Rebuilt from the live peer view on each call. For a FUNCTION entity this
   is the REQUEST schema (what the publish form fills / a call sends). */
int  cap_topic_schema(const Capture *cap, const char *topic, CapSchema *out);

/* A FUNCTION or TASK entity's RESPONSE schema (what a reply carries); 0 for every other
   kind or when no provider advertises one. */
int  cap_topic_rsp_schema(const Capture *cap, const char *topic, CapSchema *out);

/* A TASK entity's PROGRESS schema (what a @prg update carries); 0 for every other kind. */
int  cap_topic_prg_schema(const Capture *cap, const char *topic, CapSchema *out);

/* Spell `topic`'s advertised schema as compile-ready DSL text into out[out_cap] (always
   NUL-terminated). which = 0 spells the primary (request/value/payload) schema, 1 a
   function/task RESPONSE, 2 a task PROGRESS. Returns the text length, or 0 when no
   inlinable schema is advertised (hash-only or none). Walks the FULL field set, not the
   display cap; nested structs render as `name: { ... }`. */
int  cap_topic_schema_dsl(const Capture *cap, const char *topic, int which, char *out, size_t out_cap);

/* Variable FORCE control (a writable VARIABLE entity only): force pins the value built
   from the publish-form values (same field parsing as cap_publish_form) until unforce;
   both are opaque ops on the variable's set channel (an owner without allow_force ignores
   them silently). Return 1 on send. */
int  cap_variable_force_form(Capture *cap, const char *topic, const char *const *values, int n_values);
int  cap_variable_unforce(Capture *cap, const char *topic);

/* ------------------------------------------------- variable value -> publish-form text
   A VARIABLE's newest value re-spelled in the syntax the publish form's inputs accept (the
   inverse of the parsing cap_publish_form does), so a form can open on what the variable
   currently holds instead of on zeros. One entry per flat schema field, in the order
   cap_topic_schema reports; `name` lets a caller confirm the field it is filling really is
   the one this value came from (an endpoint advertising a conflicting schema must not
   prefill through it). */
#define CAP_FORM_VAL 40    /* value text per field (mirrors the explorer's form box) */

typedef struct {
    char name[CAP_TOPIC_CAP];   /* the field's own name (cap_topic_schema's label for it) */
    char value[CAP_FORM_VAL];   /* form syntax; "" = nothing to fill in (keep the default) */
} CapFormValue;

/* Copy up to `max` of `topic`'s newest VARIABLE value into out[] and return the count; 0
   for anything but a variable that has received a decodable value. A field with no form
   input of its own (a struct or map row) and one whose text will not FIT come back as "",
   never truncated: an empty form field keeps the schema default, while half an array or
   string would send a wrong value silently. */
int  cap_variable_form_values(const Capture *cap, const char *topic, CapFormValue *out, int max);

/* 1 while a send on `topic` is PARKED waiting for its match to resolve (a variable op or
   a best-effort publish fired while the announce/detail cycle was still
   verifying a candidate receiver). cap_poll flushes it the moment matching resolves and
   drops it loudly after a few seconds; the UI shows a sending indicator meanwhile. */
int  cap_topic_send_pending(const Capture *cap, const char *topic);

/* ---------------------------------------------------------------------- task control
   A TASK entity (a function with progress + cancel). Subscribing one opens a raw tap on
   its broadcast @prg channel (best-effort: an observer must never stall the task), so the
   explorer sees EVERY live run's progress; a call from the publish form goes through a
   real remote-task handle (cap_publish_form routes it), and its live state lands here. */

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
/* Fill *out with the explorer's own call state on `topic`; 0 for a non-task / never-called
   topic (out zeroed, phase IDLE). */
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
/* Third-party cancel: craft the raw CANCEL op ([u32 call_id][op 1] + [u32 caller_lo])
   on the task's request channel, directed at the run's provider. Cooperative, never
   acked; the run's terminal outcome (at ITS caller) is the answer. Returns 1 on send. */
int  cap_task_cancel_run(Capture *cap, const char *topic, uint32_t provider_id,
                         uint32_t caller_lo, uint32_t call_id);

/* ------------------------------------------------------------------- node logs
   Every DART node hosts the built-in @dart/log/{error,warn,info} topics. The explorer
   subscribes to all three at start (widening its own built-in handles to PUBSUB), so
   each node's lines collect here -- including the KEEP_LAST history a node logged
   before the explorer joined (catch_up replay is per writer lane). */
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

/* ------------------------------------------------------------ node runtime stats
   The @dart/meta introspection endpoint every node hosts. cap_meta_watch names the node
   to poll (a directed call once per second; NULL or a vanished node stops/idles it);
   cap_meta_stats copies out the latest decoded snapshot for the UI. */
#define CAP_META_TOPICS 64            /* per-topic counter rows kept from the snapshot */
typedef struct {
    char     name[CAP_TOPIC_CAP];
    uint64_t tx_msgs, tx_bytes, rx_msgs, rx_bytes;
    uint32_t subs, pubs;              /* matched subscribers / publishers at the node */
    uint32_t drops;                   /* its consumer-queue drops */
} CapMetaTopic;
typedef struct {
    int      valid;                   /* >= 1 snapshot decoded for the watched node */
    int      failing;                 /* consecutive unanswered polls (endpoint absent/slow) */
    double   age_s;                   /* seconds since the snapshot arrived; < 0 = never */
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
    /* ESP MALLOC_CAP_DEFAULT heap; zero when the platform does not report heap geometry. */
    uint64_t heap_total, heap_free, heap_min_free, heap_largest_free_block;
    double   cpu_pct;                 /* CPU%% between the last two polls; < 0 until two arrived */
    /* topics section */
    int      n_topic_rows;
    CapMetaTopic topic_rows[CAP_META_TOPICS];
} CapMetaStats;
void cap_meta_watch(Capture *cap, const char *node_name);
int  cap_meta_stats(const Capture *cap, CapMetaStats *out);   /* 1 = watching (read out->valid) */

#endif /* NET_CAPTURE_H */
