/* Live DART observer, built on a real NODE (not bare discovery). The node owns the
   discovery->transport wiring and decodes every peer's announce-metadata overlay, so
   this TU reads peers back through the node's peer API (dart_node_peers) and never
   parses the overlay itself. It runs with no topics of its own (it publishes and
   subscribes nothing); a generous topic reserve only sizes discovery's incoming
   overlay buffer so we can hold peers that advertise many topics.

   Compiled as its own TU (see net_capture.h for why) and owns the DART implementation.
   It echoes peer events to the console and exposes a peer snapshot to the UI. */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601        /* GetTickCount64 */
#endif
#define WIN32_LEAN_AND_MEAN

#define DART_TRANSPORT_IMPLEMENTATION
#define DART_NO_SHM
#include "dart_transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#ifndef _WIN32
#include <time.h>
#endif

#include "net_capture.h"

#define CAP_MAX_PEERS         64
#define CAP_OBSERVER_CHANNELS 64     /* node topic reserve: our OWN subscriptions only (each observed
                                        topic owns 1-2 DART topics). This is the STARTING reserve, not a
                                        ceiling: the node grows max_topics automatically when we create
                                        more (dynamic-mode i_dart_node_grow), so subscribing to thousands
                                        of topics just relocates the reserve, it never refuses. A peer
                                        that advertises many topics is handled by the node's meta self-
                                        heal (it grows the accept bound + RX buffers and re-solicits). */
#define CAP_LOG_LINES         400
/* CAP_LOG_LINE comes from net_capture.h (shared with the snapshot) */

/* Ensure *arr can hold index `need` (0-based): grow it by doubling through realloc, so a
   per-node/per-snapshot entity list scales to the peer's actual topic count with no fixed
   ceiling and no steady-state churn (it settles at the high-water mark). Returns 1 if the
   slot is available, 0 on OOM (the caller then skips that entry rather than overrunning). */
static int cap_grow(void *arr_ptr, int *cap, int need, size_t elem){
    void **arr = (void **)arr_ptr;
    if (need < *cap) return 1;
    {   int ncap = *cap ? *cap * 2 : 8;
        void *na;
        while (ncap <= need) ncap *= 2;
        na = realloc(*arr, (size_t)ncap * elem);
        if (!na) return 0;
        *arr = na; *cap = ncap;
    }
    return 1;
}

typedef enum { CAP_ACTIVE = 0, CAP_DROPPED = 1, CAP_GONE = 2 } CapPeerState;

typedef struct {
    char     name[DART_TOPIC_NAME_MAX + 1];   /* entity base name (mangling never surfaces) */
    uint16_t index;
    int      reliable;       /* offered (pub) / requested (sub) */
    uint8_t  kind;           /* DartEntityKind: 0 topic, else function/variable/signal */
    uint8_t  writable;       /* variable: a set channel is advertised */
    uint8_t  forceable;      /* variable: the owner permits force/unforce (allow_force) */
    uint8_t  incomplete;     /* pattern half-pair */
} CapTopic;

/* one observer-side peer record, keyed by the node's local peer id. Events drive the
   observer metrics (first_seen / updates / last_change / down) and the log; the snapshot
   refreshes the cached facts from the node and is authoritative for state. A GONE peer is
   kept (de-emphasized in the UI) with its last-known facts until its slot is reclaimed. */
typedef struct {
    int          used;
    uint32_t     local_id;
    CapPeerState state;
    int          seen_frame;   /* the current snapshot found it in the node's peer table */

    /* cached facts: discovery-level (name/addr) + announce-metadata (frag/interest) */
    int      have_meta;
    int      meta_stale;   /* peer advertises a newer blob version than the one we hold (re-fetch pending) */
    uint16_t frag;
    uint16_t meta_len;
    char     name[DART_NODE_NAME_MAX + 1];
    uint8_t  ip[16];
    uint8_t  ip_len;
    uint16_t port;
    /* advertised entities, grown to the peer's actual topic count (no fixed ceiling). The
       buffers are high-water heap allocations, preserved across a slot reclaim (see
       cap_peer_get) and reused, so a run makes no steady-state malloc/free churn. */
    int      n_pub, pub_cap;
    int      n_sub, sub_cap;
    CapTopic *pub;
    CapTopic *sub;
    uint32_t topics_epoch;   /* dart_node_peer_interest_epoch the pub/sub lists were last built
                                from: the node bumps it on ANY reflected change (interest apply,
                                external-interest assembly, names paging in), so it is the one
                                cache key -- no event handling, no version watching */

    /* observer-derived */
    unsigned           updates;
    unsigned long long first_seen_ms;
    unsigned long long last_change_ms;
    unsigned long long down_ms;
} CapPeer;

static CapPeer cap_peers[CAP_MAX_PEERS];
static char    cap_log[CAP_LOG_LINES][CAP_LOG_LINE];
static int     cap_log_head, cap_log_count;
static unsigned long long cap_start_ms;
static Config  cap_cfg;          /* echoed into the snapshot for display */

/* THREADING. The node's SERVICE THREAD (dart_node_start) owns discovery/RX/timers, so
   the poll cadence is independent of the UI frame rate. Message content never runs
   there: every explorer topic is QUEUED (consumer queues), and cap_poll drains the
   queues on the UI thread each frame (dart_node_dispatch -> cap_on_message), so the
   feed rings and their metrics stay UI-thread-owned with no locking. Only cap_on_event
   still fires on the service thread (peer/log/error bookkeeping); it always runs WITH
   the node lock held, so the shared bits (the log ring, cap_peers, the cap_subs
   topic bookkeeping) are serialized by bracketing the UI side's access with
   dart_node_lock/dart_node_unlock (cap_node below is that handle). The bracket is not
   nestable (an inner unlock releases the outer hold), so bracketed code never calls
   another bracketing helper and never logs. */
static DartNode *cap_node;

static unsigned long long cap_now_ms(void);   /* defined below; used by cap_ring_push */
static void *cap_schema_alloc(void *user, void *ptr, size_t size);          /* defined below */
static DartSchema *cap_topic_schema_parse(DartNode *node, const char *topic);/* defined below */

/* one observer-side topic the explorer is using: a ring of recent messages plus the DART
   topic(s) wiring it up. A topic's reliability is FIXED at creation, but our wanted
   reliability can change as publishers come and go, so a topic owns up to TWO topics
   (ch[0] best-effort, ch[1] reliable), created on demand, with exactly one live at a time.
   That live topic's ROLE is the union of what we want: SUB_ONLY (receive), PUB_ONLY
   (send), or PUBSUB (both) -- one topic does both directions, since two live topics of
   the same identity would misroute. cap_sub_reconcile keeps it in sync. The message/event
   callbacks find the topic by either topic's local index. */
#define CAP_MAX_SUBS 16000           /* distinct topics the explorer may show data for at once (each
                                        owns 1-2 DART topics). Matches the ~13k announce topic ceiling
                                        so the user can watch a whole large network with no artificial
                                        cap. CapSub is ~300 B (the feed ring is a lazily-allocated
                                        pointer, NOT embedded), so the fixed table is ~4.8 MB; an idle
                                        or never-used slot costs nothing beyond that. */

typedef struct {
    double   t_s;
    uint32_t uid;                         /* per-topic message id (UI expand/collapse key) */
    uint32_t len;
    uint16_t preview_len;
    int      mine;                        /* 1 = we published it (local echo) */
    int      forced;                      /* variable value published FORCED (prefix flag) */
    int      decoded;                     /* 1 = fields[] holds the reflected decode */
    int      n_fields, total_fields;
    char     type_name[DART_TOPIC_NAME_MAX + 1];   /* sender's schema root name when decoded */
    char     sender[DART_NODE_NAME_MAX + 1];
    char     preview[CAP_MSG_PREVIEW];    /* decoded: one-line summary; raw: payload head */
    CapMsgField fields[CAP_MSG_FIELDS];   /* per-field rendered decode (all depths) */
} CapMsgRec;

typedef struct {
    int          used;
    int          subscribed;             /* user wants to receive (drives the topic light) */
    int          publishing;             /* user has sent here, so the topic stays PUB-capable */
    int          error;                  /* a QoS-incompatible / oversize event hit the live topic */
    int          reliable;               /* reliability of the currently live topic (0/1) */
    uint8_t      kind;                   /* CAP_KIND_* entity kind, resolved from the peer view at
                                            first use (0 = plain topic). Locked in once channels exist. */
    char         name[DART_TOPIC_NAME_MAX + 1];
    DartTopic *ch[2];                  /* [0] best-effort, [1] reliable; NULL until first needed.
                                          For a VARIABLE ch[1] is the value channel (subscribe side);
                                          for a SIGNAL the one shared channel; a FUNCTION has none. */
    DartTopic   *set_ch;                 /* VARIABLE: our name@set publisher (kind VAR_SET) */
    /* one deadline-bounded pending send, parked when a send races the forming match (the
       core's blocking match wait is disabled here: the UI thread must never stall). Newest
       send overwrites it; cap_poll flushes it the moment matching resolves (or on expiry
       drops it with a log line, never silently). Covers the sends retention cannot: a
       variable set/force/unforce op, a signal emit, a best-effort publish. */
    uint8_t     *pend_data;              /* malloc'd payload (may be empty); pend_used gates */
    uint32_t     pend_len;
    uint8_t      pend_op;                /* VARIABLE: the set-channel op byte */
    uint8_t      pend_used;
    unsigned long long pend_expire_ms;
    DartFunction *fn;                    /* FUNCTION: our caller handle (subscribe = watch own calls) */
    DartSchema  *req_schema;             /* FUNCTION: parsed request schema (ours; form + echo decode) */
    DartSchema  *rsp_schema;             /* FUNCTION: parsed response schema (ours; reply decode) */
    int          forced;                 /* VARIABLE: the newest value carried the FORCED flag */
    uint32_t     write_seq;              /* VARIABLE: the newest value's write counter */
    uint16_t     index[2];               /* their topic indices (valid where ch[i] != NULL) */
    unsigned long long n_msgs;
    unsigned long long n_drops;
    uint32_t     uid_next;               /* next message uid (expand/collapse key) */
    CapMsgRec   *ring;                   /* CAP_FEED_MAX records, allocated LAZILY on the first
                                            message (cap_ring_push): an idle/never-used slot costs
                                            nothing, so 512 slots don't reserve ~275 MB up front */
    int          head, count;            /* ring write cursor + fill */
    double       rate_hz;                /* publish rate (msgs/s), refreshed ~1 Hz (below) */
    double       rate_prev_hr;           /* high-res time of the last rate-window boundary */
    unsigned long long rate_prev_msgs;   /* n_msgs at that boundary (rate = dmsgs / dt) */
    double       jitter_last_hr;         /* high-res time of the previous received message; <= 0 = none yet */
    double       jitter_mean_ms;         /* EWMA of the inter-arrival interval (ms): the topic's "expected" spacing */
    double       jitter_p90_ms;          /* online p90 estimate of the deviation from jitter_mean_ms (ms) */
    int          jitter_n;               /* inter-arrival samples folded in (gates the estimate until warm) */
} CapSub;

static CapSub cap_subs[CAP_MAX_SUBS];

static CapSub *cap_sub_find(const char *name){
    int i;
    for (i = 0; i < CAP_MAX_SUBS; i++)
        if (cap_subs[i].used && !strcmp(cap_subs[i].name, name)) return &cap_subs[i];
    return NULL;
}
static CapSub *cap_sub_get(const char *name){          /* find or claim a slot */
    CapSub *s = cap_sub_find(name); int i;
    if (s) return s;
    /* claiming runs on the UI thread while the service thread's event callback may walk
       the table (cap_sub_by_index): publish the slot atomically under the node lock */
    if (cap_node) dart_node_lock(cap_node);
    for (i = 0; i < CAP_MAX_SUBS; i++) if (!cap_subs[i].used){
        s = &cap_subs[i]; memset(s, 0, sizeof *s);
        snprintf(s->name, sizeof s->name, "%s", name);
        s->used = 1;
        break;
    }
    if (cap_node) dart_node_unlock(cap_node);
    return s;
}
static CapSub *cap_sub_by_index(uint16_t index){
    int i;
    for (i = 0; i < CAP_MAX_SUBS; i++){
        CapSub *s = &cap_subs[i];
        if (!s->used) continue;
        if ((s->ch[0] && s->index[0] == index) || (s->ch[1] && s->index[1] == index)) return s;
    }
    return NULL;
}

/* ---- reflected decode: the explorer subscribes without a schema of its own, so a
   delivered message's DartMsg.schema is the SENDER's. Each message becomes per-field
   rows rendered exactly as the writer declared them; no content guessing (a u8 array
   is an array of numbers, not assumed text). */
static int cap_val_append(char *dst, int cap, int at, const char *fmt, ...){
    va_list ap; int n;
    if (at >= cap - 1) return cap - 1;
    va_start(ap, fmt);
    n = vsnprintf(dst + at, (size_t)(cap - at), fmt, ap);
    va_end(ap);
    if (n < 0 || at + n >= cap) return cap - 1;   /* clamped (still NUL-terminated) */
    return at + n;
}
/* an array value: elements decoded by their kind. Appends at most `max_items` of them
   (0 = as many as the buffer holds), then " .." if any were left out. Floats print
   %g normally, or fixed-width (see cap_fmt_float) when `fixed_float`. */
static void cap_fmt_arr(char *dst, int cap, int *at, DartBytes a, uint8_t elem, uint16_t count,
                        uint16_t str_cap, uint16_t max_items, int fixed_float){
    uint32_t esz = (elem == DART_STR) ? 2u + str_cap
                                      : dart_schema_scalar_size((DartSchemaTypeKind)elem);
    uint16_t limit = (max_items && max_items < count) ? max_items : count;
    uint16_t j;
    *at = cap_val_append(dst, cap, *at, "[");
    for (j = 0; j < limit && esz && *at < cap - 8; j++){   /* keep room for " ..]" */
        const uint8_t *p = a.data + (size_t)j * esz;
        if (j) *at = cap_val_append(dst, cap, *at, " ");
        switch ((DartSchemaTypeKind)elem){
            case DART_U8:  *at = cap_val_append(dst,cap,*at,"%u",  p[0]); break;
            case DART_U16: *at = cap_val_append(dst,cap,*at,"%u",  i_dart_le_r16(p)); break;
            case DART_U32: *at = cap_val_append(dst,cap,*at,"%lu", (unsigned long)i_dart_le_r32(p)); break;
            case DART_U64: *at = cap_val_append(dst,cap,*at,"%llu",(unsigned long long)i_dart_le_r64(p)); break;
            case DART_I8:  *at = cap_val_append(dst,cap,*at,"%d",  (int)(int8_t)p[0]); break;
            case DART_I16: *at = cap_val_append(dst,cap,*at,"%d",  (int)(int16_t)i_dart_le_r16(p)); break;
            case DART_I32: *at = cap_val_append(dst,cap,*at,"%ld", (long)(int32_t)i_dart_le_r32(p)); break;
            case DART_I64: *at = cap_val_append(dst,cap,*at,"%lld",(long long)(int64_t)i_dart_le_r64(p)); break;
            case DART_F32: { uint32_t b = i_dart_le_r32(p); float  v; memcpy(&v,&b,4);
                             *at = cap_val_append(dst,cap,*at, fixed_float ? "%8.3f" : "%g",(double)v); } break;
            case DART_F64: { uint64_t b = i_dart_le_r64(p); double v; memcpy(&v,&b,8);
                             *at = cap_val_append(dst,cap,*at, fixed_float ? "%8.3f" : "%g",v); } break;
            case DART_BOOL: *at = cap_val_append(dst,cap,*at,"%s", p[0] ? "true" : "false"); break;
            case DART_STR: { uint16_t l = i_dart_le_r16(p); if (l > str_cap) l = str_cap;
                             *at = cap_val_append(dst,cap,*at,"\"%.*s\"", (int)l, (const char *)p + 2); } break;
            default: break;
        }
    }
    if (j < count) *at = cap_val_append(dst, cap, *at, " ..");
    *at = cap_val_append(dst, cap, *at, "]");
}
/* the live element count of a variable array's reflected value */
static uint16_t cap_varr_count(const DartSchemaFieldInfo *fi, const DartValue *v){
    uint32_t esz = (fi->elem == DART_STR) ? 2u + fi->str_cap
                                          : dart_schema_scalar_size((DartSchemaTypeKind)fi->elem);
    return esz ? (uint16_t)(v->bytes.len / esz) : 0;
}

/* a map body, "{k=v k2="s" nested={..} ..}"; values by their own tags, depth-capped */
static void cap_fmt_dyn_value(char *dst, int cap, int *at, const DartValue *v,
                              int depth, uint16_t max_items);
static void cap_fmt_map_body(char *dst, int cap, int *at, DartBytes body,
                             int depth, uint16_t max_items){
    uint16_t n = dart_map_count(body), i, limit = (max_items && max_items < n) ? max_items : n;
    *at = cap_val_append(dst, cap, *at, "{");
    for (i = 0; i < limit && *at < cap - 8; i++){
        DartString k; DartValue v;
        if (!dart_map_at(body, i, &k, &v)) break;
        *at = cap_val_append(dst, cap, *at, i ? " %.*s=" : "%.*s=",
                             (int)k.len, k.data ? k.data : "");
        cap_fmt_dyn_value(dst, cap, at, &v, depth, max_items);
    }
    if (i < n) *at = cap_val_append(dst, cap, *at, " ..");
    *at = cap_val_append(dst, cap, *at, "}");
}
static void cap_fmt_dyn_value(char *dst, int cap, int *at, const DartValue *v,
                              int depth, uint16_t max_items){
    switch ((DartSchemaTypeKind)v->kind){
        case DART_BOOL: *at = cap_val_append(dst, cap, *at, "%s", v->v.u ? "true" : "false"); break;
        case DART_U8: case DART_U16: case DART_U32: case DART_U64:
            *at = cap_val_append(dst, cap, *at, "%llu", (unsigned long long)v->v.u); break;
        case DART_I8: case DART_I16: case DART_I32: case DART_I64:
            *at = cap_val_append(dst, cap, *at, "%lld", (long long)v->v.i); break;
        case DART_F32: case DART_F64:
            *at = cap_val_append(dst, cap, *at, "%g", v->v.f); break;
        case DART_VSTR:
            *at = cap_val_append(dst, cap, *at, "\"%.*s\"", (int)v->bytes.len,
                                 v->bytes.data ? (const char *)v->bytes.data : ""); break;
        case DART_VARR: {
            uint16_t n = dart_map_array_count(v->bytes), j,
                     limit = (max_items && max_items < n) ? max_items : n;
            *at = cap_val_append(dst, cap, *at, "[");
            for (j = 0; j < limit && *at < cap - 8; j++){
                DartValue e;
                if (!dart_map_array_at(v->bytes, j, &e)) break;
                if (j) *at = cap_val_append(dst, cap, *at, " ");
                if (depth < 4) cap_fmt_dyn_value(dst, cap, at, &e, depth + 1, max_items);
                else           *at = cap_val_append(dst, cap, *at, "..");
            }
            if (j < n) *at = cap_val_append(dst, cap, *at, " ..");
            *at = cap_val_append(dst, cap, *at, "]");
            break;
        }
        case DART_MAP:
            if (depth < 4) cap_fmt_map_body(dst, cap, at, v->bytes, depth + 1, max_items);
            else           *at = cap_val_append(dst, cap, *at, "{..}");
            break;
        default: *at = cap_val_append(dst, cap, *at, "?"); break;
    }
}

/* one field's value text, from its reflected DartValue. s + field resolve an ENUM's stored
   number to its option NAME (so the feed/inspector shows the name, not the raw integer);
   an unknown/out-of-table number falls back to the number itself. */
static void cap_fmt_value(char *dst, int cap, const DartSchema *s, uint16_t field,
                          const DartSchemaFieldInfo *fi, const DartValue *v){
    int at = 0;
    switch ((DartSchemaTypeKind)fi->kind){
        case DART_ENUM: {
            DartString nm = dart_enum_name_of(s, field, v->v.i);   /* the option name for this value */
            if (nm.len) snprintf(dst, (size_t)cap, "%.*s", (int)nm.len, nm.data);
            else        snprintf(dst, (size_t)cap, "%lld", (long long)v->v.i);   /* unknown value: the number */
            break;
        }
        case DART_BOOL: snprintf(dst, (size_t)cap, "%s", v->v.u ? "true" : "false"); break;
        case DART_U8: case DART_U16: case DART_U32: case DART_U64:
            snprintf(dst, (size_t)cap, "%llu", (unsigned long long)v->v.u); break;
        case DART_I8: case DART_I16: case DART_I32: case DART_I64:
            snprintf(dst, (size_t)cap, "%lld", (long long)v->v.i); break;
        case DART_F32: case DART_F64:
            snprintf(dst, (size_t)cap, "%g", v->v.f); break;
        case DART_ARR:    dst[0] = '\0'; cap_fmt_arr(dst, cap, &at, v->bytes, fi->elem, fi->count, fi->str_cap, 0, 0); break;
        case DART_VARR:   dst[0] = '\0'; cap_fmt_arr(dst, cap, &at, v->bytes, fi->elem, cap_varr_count(fi, v), fi->str_cap, 0, 0); break;
        case DART_STR: case DART_VSTR:
            snprintf(dst, (size_t)cap, "\"%.*s\"", (int)v->bytes.len,
                     v->bytes.data ? (const char *)v->bytes.data : ""); break;
        case DART_MAP:    dst[0] = '\0'; cap_fmt_map_body(dst, cap, &at, v->bytes, 0, 0); break;
        case DART_STRUCT: snprintf(dst, (size_t)cap, "{...}"); break;
        default:          snprintf(dst, (size_t)cap, "?"); break;
    }
}
/* a float, fixed-width (unlike %g, which varies with magnitude/precision and can jump to
   scientific notation) so values line up in the single-line preview */
static void cap_fmt_float(char *dst, int cap, int *at, double v){
    *at = cap_val_append(dst, cap, *at, "%8.3f", v);
}

#define CAP_PREVIEW_MAX_DEPTH 2   /* nested struct levels the preview expands (root = 0) */
#define CAP_PREVIEW_MAX_ITEMS 4   /* array elements / struct members shown before " .." */

/* advance past the field at `idx` and, if it is a struct, all of its members (the flat
   table is depth-first: a struct's subtree is every following entry deeper than it) */
static uint16_t cap_field_skip(const DartSchema *s, uint16_t nf, uint16_t idx){
    DartSchemaFieldInfo fi;
    uint16_t depth;
    if (!dart_schema_field_at(s, idx, &fi)) return (uint16_t)(idx + 1);
    depth = fi.depth;
    idx++;
    while (idx < nf){
        DartSchemaFieldInfo next;
        if (!dart_schema_field_at(s, idx, &next) || next.depth <= depth) break;
        idx++;
    }
    return idx;
}

/* the preview's value for one field: scalars as cap_fmt_value, arrays up to
   CAP_PREVIEW_MAX_ITEMS elements, structs expanded member-by-member (up to
   CAP_PREVIEW_MAX_ITEMS shown, " .." past that) recursively up to CAP_PREVIEW_MAX_DEPTH
   levels of nesting; deeper structs fold to "{...}" */
static void cap_fmt_value_preview(char *dst, int cap, int *at, const DartSchema *s, DartBytes data,
                                  uint16_t nf, uint16_t field_index, const DartSchemaFieldInfo *fi,
                                  const DartValue *v){
    if (fi->kind == DART_STRUCT){
        if (fi->depth < CAP_PREVIEW_MAX_DEPTH){
            uint16_t idx = (uint16_t)(field_index + 1);
            int shown = 0;
            *at = cap_val_append(dst, cap, *at, "{");
            while (idx < nf){
                DartSchemaFieldInfo mfi; DartValue mv;
                if (!dart_schema_field_at(s, idx, &mfi) || mfi.depth <= fi->depth) break;
                if (!dart_get_value(data, s, idx, &mv)) break;
                if (shown >= CAP_PREVIEW_MAX_ITEMS){
                    *at = cap_val_append(dst, cap, *at, " ..");
                    break;
                }
                *at = cap_val_append(dst, cap, *at, shown ? " %.*s=" : "%.*s=",
                                     (int)mfi.name.len, mfi.name.data ? mfi.name.data : "");
                cap_fmt_value_preview(dst, cap, at, s, data, nf, idx, &mfi, &mv);
                shown++;
                idx = cap_field_skip(s, nf, idx);
            }
            *at = cap_val_append(dst, cap, *at, "}");
        } else {
            *at = cap_val_append(dst, cap, *at, "{...}");
        }
    } else if (fi->kind == DART_ARR){
        cap_fmt_arr(dst, cap, at, v->bytes, fi->elem, fi->count, fi->str_cap, CAP_PREVIEW_MAX_ITEMS, 1);
    } else if (fi->kind == DART_VARR){
        cap_fmt_arr(dst, cap, at, v->bytes, fi->elem, cap_varr_count(fi, v), fi->str_cap, CAP_PREVIEW_MAX_ITEMS, 1);
    } else if (fi->kind == DART_STR || fi->kind == DART_VSTR){
        *at = cap_val_append(dst, cap, *at, "\"%.*s\"", (int)v->bytes.len,
                             v->bytes.data ? (const char *)v->bytes.data : "");
    } else if (fi->kind == DART_MAP){
        cap_fmt_map_body(dst, cap, at, v->bytes, 0, CAP_PREVIEW_MAX_ITEMS);
    } else if (fi->kind == DART_F32 || fi->kind == DART_F64){
        cap_fmt_float(dst, cap, at, v->v.f);
    } else {
        char one[32];
        cap_fmt_value(one, (int)sizeof one, s, field_index, fi, v);
        *at = cap_val_append(dst, cap, *at, "%s", one);
    }
}

static void cap_field_type_str(char *dst, size_t cap, const DartSchemaFieldInfo *fi);

/* A field's UI label: its name, or, for the anonymous field of a BARE-TYPE schema
   (`bool`, `f32[]`, ...), the type itself, so no row ever renders blank. */
static void cap_field_label(char *dst, size_t cap, const DartSchemaFieldInfo *fi){
    if (fi->name.len) snprintf(dst, cap, "%.*s", (int)fi->name.len, fi->name.data);
    else              cap_field_type_str(dst, cap, fi);
}

/* A schema's display type: its root type name, or, for a bare type (an anonymous root),
   the type itself. Never empty for a decodable schema. */
static void cap_schema_type_name(char *dst, size_t cap, const DartSchema *sch){
    DartString tn = dart_schema_name(sch);
    DartSchemaFieldInfo fi;
    if (tn.len) snprintf(dst, cap, "%.*s", (int)tn.len, tn.data);
    else if (dart_schema_field_count(sch) == 1 && dart_schema_field_at(sch, 0, &fi)
             && fi.name.len == 0 && fi.kind != DART_STRUCT)
        cap_field_type_str(dst, cap, &fi);
    else if (cap) dst[0] = '\0';
}

/* reflect a message into per-field rows (every depth) + a one-line summary (top-level
   fields only; arrays/structs are expanded inline via cap_fmt_value_preview) */
static void cap_decode_fields(CapMsgRec *m, DartBytes data, const DartSchema *s){
    uint16_t i, nf = dart_schema_field_count(s);
    int at = 0;
    m->n_fields = 0;
    m->total_fields = nf;
    m->preview[0] = '\0';
    for (i = 0; i < nf; i++){
        DartSchemaFieldInfo fi; DartValue v;
        if (!dart_schema_field_at(s, i, &fi) || !dart_get_value(data, s, i, &v)) break;
        if (m->n_fields < CAP_MSG_FIELDS){
            CapMsgField *f = &m->fields[m->n_fields++];
            cap_field_label(f->name, sizeof f->name, &fi);
            f->depth = (uint8_t)(fi.depth > 255 ? 255 : fi.depth);
            cap_fmt_value(f->value, (int)sizeof f->value, s, i, &fi, &v);
        }
        if (fi.depth == 0){                                /* the collapsed summary line */
            if (fi.name.len)                               /* a bare type prints just its value */
                at = cap_val_append(m->preview, CAP_MSG_PREVIEW, at, at ? "  %.*s=" : "%.*s=",
                                    (int)fi.name.len, fi.name.data);
            cap_fmt_value_preview(m->preview, CAP_MSG_PREVIEW, &at, s, data, nf, i, &fi, &v);
        }
    }
    m->preview_len = (uint16_t)strlen(m->preview);
}

/* append one message to a topic's ring (received or our own echo). Does not touch
   n_msgs. With a schema the message is reflected into field rows; raw bytes else. */
static void cap_ring_push(CapSub *s, DartString sender, const void *data, size_t len, int mine,
                          const DartSchema *schema){
    CapMsgRec *m;
    if (!s->ring){                       /* lazy: first message on this topic allocates the ring */
        s->ring = (CapMsgRec *)calloc(CAP_FEED_MAX, sizeof *s->ring);
        if (!s->ring) return;            /* out of memory: drop the message rather than crash */
    }
    m = &s->ring[s->head];
    m->t_s = (double)(cap_now_ms() - cap_start_ms) / 1000.0;
    m->uid = ++s->uid_next;
    m->len = (uint32_t)len;
    m->decoded = 0; m->type_name[0] = '\0';
    m->n_fields = m->total_fields = 0;
    m->preview_len = 0;
    if (schema){
        cap_decode_fields(m, dart_bytes(data, len), schema);
        m->decoded = 1;
        cap_schema_type_name(m->type_name, sizeof m->type_name, schema);
    } else {
        uint16_t c = len < CAP_MSG_PREVIEW ? (uint16_t)len : CAP_MSG_PREVIEW;
        if (c) memcpy(m->preview, data, c);
        m->preview_len = c;
    }
    m->mine = mine; m->forced = 0;
    snprintf(m->sender, sizeof m->sender, "%.*s", (int)sender.len, sender.data ? sender.data : "");
    s->head = (s->head + 1) % CAP_FEED_MAX;
    if (s->count < CAP_FEED_MAX) s->count++;
}

static unsigned long long cap_now_ms(void){
#ifdef _WIN32
    return (unsigned long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)(ts.tv_nsec / 1000000);
#endif
}

/* high-resolution monotonic seconds. GetTickCount64 quantizes to ~15.6 ms, which is too
   coarse for a rate window (it snaps a 250 Hz reading to a couple of discrete values); the
   performance counter is sub-microsecond, so a message-count / elapsed-time rate is exact. */
static double cap_now_hr(void){
#ifdef _WIN32
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (double)c.QuadPart / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

/* Track a topic's receive-time jitter across its WHOLE lifetime, not just the visible feed
   ring (which overwrites after CAP_FEED_MAX messages). jitter_mean_ms is an EWMA of the
   inter-arrival interval: the topic's "expected" spacing, adapting as its rate changes. Each
   new interval's deviation from that mean is one jitter sample, folded into jitter_p90_ms via
   an online quantile tracker (stochastic approximation on the pinball loss: nudge the estimate
   up by step*0.90 when a sample meets or exceeds it, down by step*0.10 when it doesn't; this
   converges to the true 90th percentile without keeping any sample history at all). The step
   scales with jitter_mean_ms so it adapts to the topic's own timescale (a slow topic's jitter
   is naturally larger in absolute ms than a fast one's). */
static void cap_jitter_update(CapSub *s, double t_hr){
    if (s->jitter_last_hr > 0.0){
        double dt_ms = (t_hr - s->jitter_last_hr) * 1000.0;
        if (s->jitter_n == 0){
            s->jitter_mean_ms = dt_ms;                       /* seed the expected spacing */
        } else {
            double dev = dt_ms - s->jitter_mean_ms; if (dev < 0.0) dev = -dev;
            double step;
            s->jitter_mean_ms += 0.2 * (dt_ms - s->jitter_mean_ms);
            step = s->jitter_mean_ms * 0.05;
            if (dev >= s->jitter_p90_ms) s->jitter_p90_ms += step * 0.90;
            else                         s->jitter_p90_ms -= step * 0.10;
            if (s->jitter_p90_ms < 0.0)  s->jitter_p90_ms = 0.0;
        }
        s->jitter_n++;
    }
    s->jitter_last_hr = t_hr;
}

/* Log-ring append, callable from BOTH threads: UI paths acquire the node lock; the
   event callback (service thread) already holds it, so the bracket no-ops there. */
static void cap_logf(const char *fmt, ...){
    unsigned long long t = cap_now_ms() - cap_start_ms;
    char *line;
    int n;
    va_list ap;
    if (cap_node) dart_node_lock(cap_node);
    line = cap_log[cap_log_head];
    n = snprintf(line, CAP_LOG_LINE, "[%6.2fs] ", (double)t / 1000.0);
    va_start(ap, fmt);
    vsnprintf(line + n, (size_t)(CAP_LOG_LINE - n), fmt, ap);
    va_end(ap);
    cap_log_head = (cap_log_head + 1) % CAP_LOG_LINES;
    if (cap_log_count < CAP_LOG_LINES) cap_log_count++;
    fputs(line, stdout); fputc('\n', stdout);
    if (cap_node) dart_node_unlock(cap_node);
}

static void cap_fmt_ip(char *buf, size_t n, const uint8_t *ip, uint8_t ip_len){
    if (ip_len == 4)       snprintf(buf, n, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    else if (ip_len == 16) snprintf(buf, n, "%02x%02x:..:%02x%02x", ip[0], ip[1], ip[14], ip[15]);
    else                   snprintf(buf, n, "(no address)");
}

static CapPeer *cap_peer_find(uint32_t id){
    int i;
    for (i = 0; i < CAP_MAX_PEERS; i++)
        if (cap_peers[i].used && cap_peers[i].local_id == id) return &cap_peers[i];
    return NULL;
}

static CapPeer *cap_peer_get(uint32_t id){
    CapPeer *p = cap_peer_find(id);
    int i, victim = -1;
    unsigned long long oldest = ~0ull;
    if (p) return p;

    for (i = 0; i < CAP_MAX_PEERS; i++) if (!cap_peers[i].used){ victim = i; break; }
    if (victim < 0)
        for (i = 0; i < CAP_MAX_PEERS; i++)
            if (cap_peers[i].state == CAP_GONE && cap_peers[i].down_ms <= oldest){ oldest = cap_peers[i].down_ms; victim = i; }
    if (victim < 0)
        for (i = 0; i < CAP_MAX_PEERS; i++) if (cap_peers[i].state == CAP_DROPPED){ victim = i; break; }
    if (victim < 0) victim = 0;

    p = &cap_peers[victim];
    {   /* preserve the grown pub/sub buffers across the reset: the new peer refills them
           (n_pub/n_sub go to 0 below), so the allocation is reused, not leaked or re-malloc'd */
        CapTopic *keep_pub = p->pub, *keep_sub = p->sub;
        int keep_pub_cap = p->pub_cap, keep_sub_cap = p->sub_cap;
        memset(p, 0, sizeof *p);
        p->pub = keep_pub; p->pub_cap = keep_pub_cap;
        p->sub = keep_sub; p->sub_cap = keep_sub_cap;
    }
    p->used = 1;
    p->local_id = id;
    p->first_seen_ms = cap_now_ms();
    return p;
}

/* ---- node logs: the mesh-wide @dart/log/{error,warn,info} intake ------------------------
   The explorer widens its OWN built-in log handles to PUBSUB at start (one shared topic
   per level, so this subscribes to every node's lines at once) and switches them to
   consumer queues: lines drain on the UI thread with the other feeds, so the ring below
   is UI-thread-owned and lock-free. */
static CapNodeLogLine cap_nodelog[CAP_NODELOG_MAX];
static int      cap_nodelog_head, cap_nodelog_count;
static uint16_t cap_nodelog_index[3] = { 0xFFFF, 0xFFFF, 0xFFFF };  /* our log topics' indices */

/* @dart/meta watch state (the poll/decode functions live below with the pattern interop;
   declared here so cap_stop can reset them) */
static char               cap_meta_node[CAP_NAME_CAP];   /* watched node's name; empty = off */
static CapMetaStats       cap_meta;                      /* latest decode (lock-bracketed) */
static uint32_t           cap_meta_gen;                  /* bumped per watch change */
static uint32_t           cap_meta_req_gen;              /* generation the in-flight call belongs to */
static int                cap_meta_inflight;
static unsigned long long cap_meta_last_ms;              /* last request time */
static unsigned long long cap_meta_recv_ms;              /* when the snapshot arrived (0 = never) */
static uint64_t           cap_meta_prev_cpu, cap_meta_prev_wall;   /* CPU%% deltas */

/* a delivered @dart/log line -> the node-log ring; 1 = consumed (not a topic feed) */
static int cap_nodelog_intake(const DartMsg *msg){
    int lvl;
    for (lvl = 0; lvl < 3; lvl++) if (cap_nodelog_index[lvl] == msg->topic_index) break;
    if (lvl == 3) return 0;
    {   CapNodeLogLine *L = &cap_nodelog[cap_nodelog_head];
        DartString txt = msg->schema ? dart_get_string(msg->data, msg->schema, "text")
                                     : dart_string(NULL, 0);
        size_t tl = txt.len < CAP_NODELOG_TEXT - 1 ? txt.len : CAP_NODELOG_TEXT - 1;
        L->level   = (uint8_t)lvl;
        L->wall_us = msg->schema ? dart_get_uint(msg->data, msg->schema, "wall_us") : 0;
        snprintf(L->node, sizeof L->node, "%.*s", (int)msg->publisher_name.len, msg->publisher_name.data);
        if (tl) memcpy(L->text, txt.data, tl);
        L->text[tl] = '\0';
        cap_nodelog_head = (cap_nodelog_head + 1) % CAP_NODELOG_MAX;
        if (cap_nodelog_count < CAP_NODELOG_MAX) cap_nodelog_count++;
    }
    return 1;
}

int cap_node_log(const Capture *cap, const char *node_name, unsigned level_mask,
                 CapNodeLogLine *out, int max){
    int i, n = 0;
    (void)cap;   /* the ring is UI-thread-owned (queued dispatch fills it on this thread) */
    for (i = 0; i < cap_nodelog_count && n < max; i++){
        int idx = (cap_nodelog_head - 1 - i + 2 * CAP_NODELOG_MAX) % CAP_NODELOG_MAX;   /* newest first */
        const CapNodeLogLine *L = &cap_nodelog[idx];
        if (!(level_mask & (1u << L->level))) continue;
        if (node_name && strcmp(node_name, L->node) != 0) continue;
        out[n++] = *L;
    }
    return n;
}

/* the node's message sink: append a delivered message to its topic ring. Every explorer
   topic is queued, so this only ever runs on the UI thread (cap_poll's dispatch) --
   the rings and their metrics need no locking. We keep only a short payload preview. */
static void cap_on_message(const DartMsg *msg){
    CapSub *s;
    if (cap_nodelog_intake(msg)) return;      /* a @dart/log line, not a topic feed */
    s = cap_sub_by_index(msg->topic_index);
    if (!s) return;
    /* jitter from the POLL-side arrival stamp, not from when this frame got around to
       dispatching: a frame-paced consumer must never quantize inter-arrival times */
    cap_jitter_update(s, (double)msg->recv_us / 1e6);
    cap_ring_push(s, msg->publisher_name, msg->data.data, msg->data.len, 0, msg->schema);
    if (msg->header.len >= 5){   /* a variable value's prefix: [u8 flags][u32 write_seq] */
        int newest = (s->head - 1 + CAP_FEED_MAX) % CAP_FEED_MAX;
        s->forced    = (msg->header.data[0] & 0x01) ? 1 : 0;
        s->write_seq = i_dart_le_r32(msg->header.data + 1);
        if (s->ring) s->ring[newest].forced = s->forced;   /* ring is NULL only if the push OOM'd */
    }
    s->n_msgs++;
}

/* the node's app event sink: maintain the observer metrics + log. We never call back into
   dart_* here (the snapshot reads the facts); the cap_peers table is file-static. Runs on
   the SERVICE thread (with the node lock held), so everything it touches is read by the
   UI only under a dart_node_lock bracket (cap_snapshot, cap_topic_feed). */
static void cap_on_event(const DartEvent *ev){
    switch (ev->kind){
    case DART_PEER_UP: {
        CapPeer *p = cap_peer_get(ev->peer);
        p->last_change_ms = cap_now_ms();
        if (ev->ip_len == 4)
            cap_logf("UP    id=%u  %u.%u.%u.%u:%u", ev->peer,
                     ev->ip[0], ev->ip[1], ev->ip[2], ev->ip[3], ev->port);
        else
            cap_logf("UP    id=%u", ev->peer);
        break; }
    case DART_PEER_INTEREST: {                       /* a peer's interest list was (re)applied */
        CapPeer *p = cap_peer_find(ev->peer);
        if (p){ p->updates++; p->last_change_ms = cap_now_ms(); }
        cap_logf("        interest  id=%u  pub-to=%u  sub-from=%u",
                 ev->peer, ev->publish_topics, ev->receive_topics);
        break; }
    case DART_PEER_DOWN: {
        CapPeer *p = cap_peer_find(ev->peer);
        if (p) p->down_ms = cap_now_ms();
        cap_logf("DOWN  id=%u", ev->peer);
        break; }
    case DART_MSG_LOST: {                            /* reliable subscriber skipped past a gap */
        CapSub *s = cap_sub_by_index(ev->topic);
        if (s) s->n_drops += ev->lost_count;
        cap_logf("        LOST  ch=%u peer=%u first=%llu count=%llu", ev->topic, ev->peer,
                 (unsigned long long)ev->lost_first, (unsigned long long)ev->lost_count);
        break; }
    case DART_ERROR: {                               /* every failure funnels here; mark the sub if topic-scoped */
        char line[160];
        if (ev->error == DART_E_QOS_INCOMPATIBLE || ev->error == DART_E_SCHEMA_MISMATCH ||
            ev->error == DART_E_MSG_TOO_BIG){
            CapSub *s = cap_sub_by_index(ev->topic);
            if (s) s->error = 1;
        }
        cap_logf("        %s", dart_event_str(ev, line, sizeof line));
        break; }
    default: break;
    }
}

/* ---------------------------------------------------------------- public API */

void cap_defaults(Config *c){
    memset(c, 0, sizeof *c);
    c->domain = 0;
    c->group  = "239.255.0.7";
    c->port   = 7400;
    c->ifc    = NULL;
    c->name   = "dart-explorer";
}

static void cap_usage(const char *argv0){
    printf("usage: %s [--domain N] [--group IP] [--port N] [--if IP] [--name STR]\n"
           "  --domain N   discovery domain to observe (default 0; must match the nodes)\n"
           "  --group IP   discovery multicast group   (default 239.255.0.7)\n"
           "  --port  N    discovery port              (default 7400)\n"
           "  --if    IP   pin to one interface IP     (default all; 127.0.0.1 = single-host)\n"
           "  --name  STR  this observer node's name   (default dart-explorer)\n", argv0);
}

int cap_parse_args(int argc, char **argv, Config *c){
    int i;
    for (i = 1; i < argc; i++){
        const char *a = argv[i];
        int has_next = (i + 1 < argc);
        if      ((!strcmp(a,"--domain")||!strcmp(a,"-d")) && has_next) c->domain = (uint16_t)atoi(argv[++i]);
        else if ((!strcmp(a,"--group") ||!strcmp(a,"-g")) && has_next) c->group  = argv[++i];
        else if ((!strcmp(a,"--port")  ||!strcmp(a,"-p")) && has_next) c->port   = (uint16_t)atoi(argv[++i]);
        else if ((!strcmp(a,"--if")    ||!strcmp(a,"-i")) && has_next) c->ifc    = argv[++i];
        else if ((!strcmp(a,"--name")  ||!strcmp(a,"-n")) && has_next) c->name   = argv[++i];
        else if (!strcmp(a,"--help")||!strcmp(a,"-h")){ cap_usage(argv[0]); return 0; }
        else { fprintf(stderr, "unknown/incomplete arg: %s\n", a); cap_usage(argv[0]); return -1; }
    }
    return 1;
}

int cap_start(Capture *cap, const Config *cfg){
    DartAllocator mem;
    DartNode *node;

    memset(cap, 0, sizeof *cap);
    cap_start_ms = cap_now_ms();
    cap_cfg = *cfg;          /* group/name point into argv: stable for the run */

    /* a real node, owning its memory via a dynamic allocator. It starts with no topics but
       subscribes to topics on demand (cap_subscribe); CAP_OBSERVER_CHANNELS bounds those and
       sizes discovery's per-peer overlay buffer, so a peer advertising many topics is held in full. */
    mem  = dart_allocator_dynamic(i_dart_plat_realloc, 0);
    node = dart_node_open(&mem, cfg->name, cap_on_message, cap_on_event, &(DartNodeOpts){
        .domain        = cfg->domain,
        .max_topics  = CAP_OBSERVER_CHANNELS,
        .fetch_details = 1,   /* observer: fetch every peer topic's name + schema */
        .match_wait_ms = -1,  /* never block the UI thread in a send: a send racing the
                                 forming match parks in the per-topic pending slot instead
                                 and cap_poll flushes it when dart_topic_ready flips */
        .net           = { .discovery_group     = cfg->group,
                           .discovery_port      = cfg->port,
                           .multicast_interface = cfg->ifc },
        .discovery     = { .max_peers = CAP_MAX_PEERS },
    });
    if (!node){
        fprintf(stderr, "cap_start: dart_node_open failed (port %u in use? interface?)\n", cfg->port);
        return 0;
    }
    cap->rt  = node;
    cap->mem = NULL;         /* the node owns its memory now; close frees it */
    cap_node = node;         /* the log/table serializer handle (see the THREADING note) */
    {   /* join the mesh-wide @dart/log topics: widen our own built-in handles to PUBSUB
           (one shared topic per level subscribes to EVERY node's lines, with each node's
           recent history replaying on join) and switch them to consumer queues so the
           lines drain on the UI thread with the other feeds. */
        int lvl;
        for (lvl = 0; lvl < 3; lvl++){
            DartTopic *lc = dart_node_log_topic(node, (DartLogLevel)lvl);
            DartMsg m;
            if (!lc) continue;
            dart_topic_set_role(lc, DART_PUBSUB);
            cap_nodelog_index[lvl] = dart_topic_index(lc);
            (void)dart_topic_take(lc, &m, 0);   /* first take switches it to queued delivery */
        }
    }
    /* the service thread owns discovery/RX/timers from here: the poll cadence no longer
       rides the UI frame rate. DART_ERR_NOSYS (threads compiled out) falls back to the
       old single-threaded drive inside cap_poll. */
    if (dart_node_start(node) != DART_OK)
        cap_logf("service thread unavailable: polling on the UI thread");
    cap_logf("observer node \"%s\" started on domain %u (%s:%u)", cfg->name, cfg->domain, cfg->group, cfg->port);
    return 1;
}

/* One UI-frame tick: drain every subscribed topic's consumer queue on THIS thread
   (dart_node_dispatch runs cap_on_message here), so message handling and the UI never
   race while the service thread polls at its own cadence. Without a service thread
   (DART_NO_THREADS) it also drives the loop, exactly the old behavior. */
static void cap_drain_replies(DartNode *node);   /* defined with the pattern interop below */
static void cap_pend_poll(void);                 /* flush/expire parked sends (same block) */
static void cap_meta_poll(DartNode *node);       /* the 1 Hz @dart/meta poll (below) */

int cap_poll(Capture *cap){
    DartNode *node = (DartNode *)cap->rt;
    int n;
    if (!node) return 0;
    if (!dart_node_is_started(node))
        dart_node_poll(node, 0);
    n = dart_node_dispatch(node, 0, 0);
    cap_drain_replies(node);   /* parked function replies -> their feeds (UI thread) */
    cap_pend_poll();           /* parked sends -> the wire once matching resolves */
    cap_meta_poll(node);       /* keep the watched node's runtime stats fresh */
    return n;
}

void cap_stop(Capture *cap){
    int i;
    cap_node = NULL;
    if (cap->rt) dart_node_close((DartNode *)cap->rt, 1);   /* stops the service thread, sends a BYE, frees */
    cap->rt = NULL; cap->mem = NULL;
    /* release the observer's own heap (lazy feed rings, grown peer entity lists, parked
       sends), so a clean shutdown leaves nothing behind for a leak check */
    for (i = 0; i < CAP_MAX_SUBS; i++){
        free(cap_subs[i].ring);   cap_subs[i].ring = NULL; cap_subs[i].count = cap_subs[i].head = 0;
        free(cap_subs[i].pend_data); cap_subs[i].pend_data = NULL; cap_subs[i].pend_used = 0;
        cap_subs[i].used = 0;
    }
    for (i = 0; i < CAP_MAX_PEERS; i++){
        free(cap_peers[i].pub); cap_peers[i].pub = NULL; cap_peers[i].pub_cap = cap_peers[i].n_pub = 0;
        free(cap_peers[i].sub); cap_peers[i].sub = NULL; cap_peers[i].sub_cap = cap_peers[i].n_sub = 0;
        cap_peers[i].used = 0;
    }
    cap_nodelog_head = cap_nodelog_count = 0;
    cap_nodelog_index[0] = cap_nodelog_index[1] = cap_nodelog_index[2] = 0xFFFF;
    cap_meta_node[0] = '\0'; cap_meta_inflight = 0;
    memset(&cap_meta, 0, sizeof cap_meta);
}
/* Free a CapSnapshot's grown per-node entity buffers (the caller owns the CapSnapshot
   struct; net_capture owns the buffers it malloc'd into it). Call once at shutdown. */
void cap_snapshot_free(CapSnapshot *snap){
    int i;
    if (!snap) return;
    for (i = 0; i < CAP_SNAP_NODES; i++){
        free(snap->nodes[i].pub); snap->nodes[i].pub = NULL; snap->nodes[i].pub_cap = 0;
        free(snap->nodes[i].sub); snap->nodes[i].sub = NULL; snap->nodes[i].sub_cap = 0;
    }
}

/* ---- node runtime stats: the 1 Hz @dart/meta poll ---------------------------------------
   A directed call at the watched node's peer id, once per second, decoded straight in the
   response callback (SERVICE thread, node lock held: the same discipline as cap_on_event;
   the UI copies cap_meta out under a dart_node_lock bracket). A generation counter ties a
   reply to the watch that requested it, so a late reply for a previous node is dropped.
   The state itself is declared with the node-log ring above (cap_stop resets it). */

static uint64_t cap_map_u64(DartBytes body, const char *key){
    DartValue v;
    return dart_map_get(body, key, &v) ? v.v.u : 0;   /* the snapshot stores unsigned kinds */
}

static void cap_meta_on_reply(const DartResponse *r){
    cap_meta_inflight = 0;
    if (cap_meta_req_gen != cap_meta_gen) return;        /* a reply for a previous watch */
    if (r->status != DART_CALL_OK || !r->schema){ cap_meta.failing++; return; }
    {   DartBytes info = dart_get_map(r->data, r->schema, "info");
        DartValue v;
        uint64_t wall = 0;
        if (!info.data){ cap_meta.failing++; return; }
        cap_meta.failing = 0;
        cap_meta.valid = 1;
        cap_meta_recv_ms = cap_now_ms();
        if (dart_map_get(info, "node", &v)){
            DartBytes nm = v.bytes;
            DartValue le;
            wall = cap_map_u64(nm, "wall_us");
            cap_meta.uptime_s       = (double)cap_map_u64(nm, "uptime_us") / 1e6;
            cap_meta.mem_in_use     = cap_map_u64(nm, "mem_in_use");
            cap_meta.mem_peak       = cap_map_u64(nm, "mem_peak");
            cap_meta.alloc_calls    = cap_map_u64(nm, "alloc_calls");
            cap_meta.bp_waited_us   = cap_map_u64(nm, "bp_waited_us");
            cap_meta.bp_waits       = (uint32_t)cap_map_u64(nm, "bp_waits");
            cap_meta.evicted_unsent = (uint32_t)cap_map_u64(nm, "evicted_unsent");
            cap_meta.peers          = (uint32_t)cap_map_u64(nm, "peers");
            cap_meta.max_peers      = (uint32_t)cap_map_u64(nm, "max_peers");
            cap_meta.topics         = (uint32_t)cap_map_u64(nm, "topics");
            cap_meta.max_topics     = (uint32_t)cap_map_u64(nm, "max_topics");
            cap_meta.shm_tx         = (uint32_t)cap_map_u64(nm, "shm_tx");
            cap_meta.shm_rx         = (uint32_t)cap_map_u64(nm, "shm_rx");
            cap_meta.last_error     = (uint32_t)cap_map_u64(nm, "last_error");
            cap_meta.last_error_text[0] = '\0';
            if (dart_map_get(nm, "last_error_text", &le) && le.bytes.data){
                size_t tl = le.bytes.len < sizeof cap_meta.last_error_text - 1
                          ? le.bytes.len : sizeof cap_meta.last_error_text - 1;
                memcpy(cap_meta.last_error_text, le.bytes.data, tl);
                cap_meta.last_error_text[tl] = '\0';
            }
        }
        cap_meta.have_proc = 0;
        cap_meta.have_cpu = 0;
        cap_meta.cpu_pct = -1.0;
        if (dart_map_get(info, "proc", &v)){
            DartBytes pr = v.bytes;
            cap_meta.have_proc = 1;
            cap_meta.pid      = cap_map_u64(pr, "pid");
            cap_meta.cpu_us   = cap_map_u64(pr, "cpu_us");
            cap_meta.rss      = cap_map_u64(pr, "rss");
            cap_meta.peak_rss = cap_map_u64(pr, "peak_rss");
            { DartValue cv; cap_meta.have_cpu = dart_map_get(pr, "cpu_us", &cv); }
            cap_meta.heap_total = cap_map_u64(pr, "heap_total");
            cap_meta.heap_free  = cap_map_u64(pr, "heap_free");
            cap_meta.heap_min_free = cap_map_u64(pr, "heap_min_free");
            cap_meta.heap_largest_free_block = cap_map_u64(pr, "heap_largest_free_block");
            /* CPU%% from consecutive snapshots, over the node's own wall clock */
            if (cap_meta_prev_wall && wall > cap_meta_prev_wall && cap_meta.cpu_us >= cap_meta_prev_cpu)
                cap_meta.cpu_pct = 100.0 * (double)(cap_meta.cpu_us - cap_meta_prev_cpu)
                                         / (double)(wall - cap_meta_prev_wall);
            cap_meta_prev_cpu  = cap_meta.cpu_us;
            cap_meta_prev_wall = wall;
        }
        cap_meta.n_topic_rows = 0;
        if (dart_map_get(info, "topics", &v)){
            uint16_t cnt = dart_map_array_count(v.bytes), i;
            for (i = 0; i < cnt && cap_meta.n_topic_rows < CAP_META_TOPICS; i++){
                DartValue e, nv;
                CapMetaTopic *tr;
                if (!dart_map_array_at(v.bytes, i, &e) || e.kind != DART_MAP) continue;
                tr = &cap_meta.topic_rows[cap_meta.n_topic_rows++];
                tr->name[0] = '\0';
                if (dart_map_get(e.bytes, "name", &nv) && nv.bytes.data){
                    size_t tl = nv.bytes.len < sizeof tr->name - 1 ? nv.bytes.len : sizeof tr->name - 1;
                    memcpy(tr->name, nv.bytes.data, tl); tr->name[tl] = '\0';
                }
                tr->tx_msgs  = cap_map_u64(e.bytes, "tx_msgs");
                tr->tx_bytes = cap_map_u64(e.bytes, "tx_bytes");
                tr->rx_msgs  = cap_map_u64(e.bytes, "rx_msgs");
                tr->rx_bytes = cap_map_u64(e.bytes, "rx_bytes");
                tr->subs     = (uint32_t)cap_map_u64(e.bytes, "subs");
                tr->pubs     = (uint32_t)cap_map_u64(e.bytes, "pubs");
                tr->drops    = (uint32_t)cap_map_u64(e.bytes, "q_dropped");
            }
        }
    }
}

static void cap_meta_poll(DartNode *node){
    unsigned long long now = cap_now_ms();
    uint32_t pid = 0; int i;
    DartFunction *fn;
    if (!node || !cap_meta_node[0]) return;
    if (cap_meta_inflight || now - cap_meta_last_ms < 1000) return;
    fn = dart_node_meta_function(node);
    if (!fn) return;
    /* resolve the watched node's peer id from the cached peer table (written on the
       service thread under the node lock: bracket the read) */
    dart_node_lock(node);
    for (i = 0; i < CAP_MAX_PEERS; i++)
        if (cap_peers[i].used && cap_peers[i].state == CAP_ACTIVE
            && !strcmp(cap_peers[i].name, cap_meta_node)){ pid = cap_peers[i].local_id; break; }
    dart_node_unlock(node);
    if (!pid) return;                     /* not discovered / dropped: idle until it is */
    cap_meta_last_ms = now;
    cap_meta_inflight = 1;
    cap_meta_req_gen = cap_meta_gen;
    if (dart_function_call_async(fn, dart_bytes(NULL, 0), cap_meta_on_reply, NULL,
                                 &(DartCallOpts){ .provider = pid }) != DART_OK)
        cap_meta_inflight = 0;
}

void cap_meta_watch(Capture *cap, const char *node_name){
    DartNode *node = cap ? (DartNode *)cap->rt : NULL;
    if (!node) return;
    if (!node_name && !cap_meta_node[0]) return;                       /* already off */
    if (node_name && !strcmp(cap_meta_node, node_name)) return;        /* unchanged */
    dart_node_lock(node);
    memset(&cap_meta, 0, sizeof cap_meta);
    cap_meta.cpu_pct = -1.0;
    cap_meta_gen++;                       /* orphan any in-flight reply */
    cap_meta_prev_cpu = cap_meta_prev_wall = 0;
    cap_meta_recv_ms = 0; cap_meta_last_ms = 0;
    snprintf(cap_meta_node, sizeof cap_meta_node, "%s", node_name ? node_name : "");
    dart_node_unlock(node);
}

int cap_meta_stats(const Capture *cap, CapMetaStats *out){
    DartNode *node = cap ? (DartNode *)cap->rt : NULL;
    if (!out) return 0;
    if (!node || !cap_meta_node[0]){ memset(out, 0, sizeof *out); return 0; }
    dart_node_lock(node);
    *out = cap_meta;
    dart_node_unlock(node);
    out->age_s = cap_meta_recv_ms ? (double)(cap_now_ms() - cap_meta_recv_ms) / 1000.0 : -1.0;
    snprintf(out->node, sizeof out->node, "%s", cap_meta_node);
    return 1;
}

/* ---- pattern-entity interop -----------------------------------------------------------
   Joining a pattern entity's data plane needs KIND-matched channels (the kind gate refuses
   a plain topic against a variable/signal channel) and, for functions, a real caller (the
   replies are DIRECTED to their caller, so an observer can only ever see its own calls).
   This block is the one place in the explorer that knows the wire constants: the channel
   kinds and prefixes mirror src/patterns/core.c (DART__VAR_PREFIX / DART__SET_PREFIX). */
#define CAP_VAR_PREFIX 5   /* value channel prefix: [u8 flags][u32 write_seq] */
#define CAP_SET_PREFIX 1   /* set channel prefix:   [u8 op] */

/* the entity kind (CAP_KIND_*) some peer advertises `name` as, from the cached peer view
   (built each frame by cap_snapshot); writable set for a variable with a set channel */
static uint8_t cap_entity_kind(const char *name, int *writable){
    int i, k;
    if (writable) *writable = 0;
    for (i = 0; i < CAP_MAX_PEERS; i++){
        const CapPeer *p = &cap_peers[i];
        if (!p->used) continue;
        for (k = 0; k < p->n_pub; k++)
            if (!strcmp(p->pub[k].name, name)){
                if (writable) *writable = p->pub[k].writable;
                return p->pub[k].kind;
            }
        for (k = 0; k < p->n_sub; k++)
            if (!strcmp(p->sub[k].name, name)){
                if (writable) *writable = p->sub[k].writable;
                return p->sub[k].kind;
            }
    }
    return CAP_KIND_TOPIC;
}

/* the CHANNEL name that carries an entity's schema: a function's request channel
   ("name@req"; rsp = "name@rsp"), everything else the bare name (a variable's value
   channel and a signal ARE the bare name on the wire) */
static void cap_primary_channel_name(char *dst, size_t cap, const char *topic, uint8_t kind,
                                     int rsp){
    if (kind == CAP_KIND_FUNCTION) snprintf(dst, cap, "%s%s", topic, rsp ? "@rsp" : "@req");
    else                           snprintf(dst, cap, "%s", topic);
}

/* function replies arrive on the SERVICE thread (the caller's delivery callback, node lock
   held). The feed rings are UI-thread-owned, so replies park here and cap_poll drains them
   into the ring under the same lock. */
#define CAP_REPLY_PENDING 8
#define CAP_REPLY_MAX     2048
typedef struct {
    int      used;
    CapSub  *s;
    int      status;              /* DartCallStatus */
    uint32_t provider;            /* peer id (0 = synthesized) */
    uint32_t len;                 /* true reply length */
    uint32_t kept;                /* bytes kept in data[] */
    uint8_t  data[CAP_REPLY_MAX];
} CapReplyPending;
static CapReplyPending cap_replies[CAP_REPLY_PENDING];

static void cap_fn_on_reply(const DartResponse *r){
    CapSub *s = (CapSub *)r->user;
    int i;
    for (i = 0; i < CAP_REPLY_PENDING; i++){
        CapReplyPending *pr = &cap_replies[i];
        if (pr->used) continue;
        pr->s = s; pr->status = (int)r->status; pr->provider = r->provider;
        pr->len  = (uint32_t)r->data.len;
        pr->kept = r->data.len > CAP_REPLY_MAX ? CAP_REPLY_MAX : (uint32_t)r->data.len;
        if (pr->kept) memcpy(pr->data, r->data.data, pr->kept);
        pr->used = 1;
        return;
    }
    /* pending full: the reply is dropped from the feed (the call itself completed) */
}

/* drain parked function replies into their topics' feeds, on the UI thread. The pending
   slots are written by the service thread WITH the node lock held, so the drain brackets
   the copy-out with the same lock (and only the copy-out: ring pushes stay unbracketed,
   UI-thread-owned, per the threading note at the top). */
static void cap_drain_replies(DartNode *node){
    CapReplyPending local[CAP_REPLY_PENDING];
    int i, n = 0;
    dart_node_lock(node);
    for (i = 0; i < CAP_REPLY_PENDING; i++)
        if (cap_replies[i].used){ local[n++] = cap_replies[i]; cap_replies[i].used = 0; }
    dart_node_unlock(node);
    for (i = 0; i < n; i++){
        CapReplyPending *pr = &local[i];
        CapSub *s = pr->s;
        char sender[DART_NODE_NAME_MAX + 1] = "provider";
        int k;
        if (!s || !s->used) continue;
        for (k = 0; k < CAP_MAX_PEERS; k++)   /* provider peer id -> its display name */
            if (cap_peers[k].used && cap_peers[k].local_id == pr->provider && cap_peers[k].name[0]){
                snprintf(sender, sizeof sender, "%s", cap_peers[k].name);
                break;
            }
        s->n_msgs++;
        if (pr->status != 0){   /* a failed call: the outcome text IS the feed entry */
            static const char *st[] = { "OK", "APP_ERROR", "NO_HANDLER", "TIMEOUT", "PEER_LOST" };
            char text[64];
            snprintf(text, sizeof text, "call failed: %s",
                     (pr->status > 0 && pr->status <= 4) ? st[pr->status] : "?");
            cap_ring_push(s, dart_cstr(pr->provider ? sender : "(no reply)"),
                          text, strlen(text), 0, NULL);
        } else {
            /* decode with our response-schema copy when the reply arrived whole */
            const DartSchema *sch = (pr->kept == pr->len) ? s->rsp_schema : NULL;
            if (sch && !dart_schema_validate(sch, dart_bytes(pr->data, pr->kept))) sch = NULL;
            cap_ring_push(s, dart_cstr(sender), pr->data, pr->kept, 0, sch);
        }
    }
}

static int cap_sub_reconcile_topic(CapSub *s, DartNode *node, int want);

/* bring the topic's live channels in line with subscribed/publishing. A plain topic takes
   the original path (cap_sub_reconcile_topic); a pattern entity gets kind-matched channels,
   always reliable: a VARIABLE subscribes its value channel and publishes over its own
   name@set channel (never the value: that would claim ownership), a SIGNAL rides one
   kind-matched channel both ways, and a FUNCTION opens a caller handle (subscribe and
   publish both mean "be a caller": the feed can only ever show our own calls). */
static int cap_sub_reconcile(CapSub *s, DartNode *node, int want){
    if (!s->kind && !s->ch[0] && !s->ch[1] && !s->fn)
        s->kind = cap_entity_kind(s->name, NULL);   /* resolve once, before any channel exists */

    if (s->kind == CAP_KIND_VARIABLE || s->kind == CAP_KIND_SIGNAL){
        int variable = (s->kind == CAP_KIND_VARIABLE);
        DartRole role = variable
                      ? (s->subscribed ? DART_SUB_ONLY : DART_INACTIVE)
                      : ((s->subscribed && s->publishing) ? DART_PUBSUB
                        : s->subscribed ? DART_SUB_ONLY
                        : s->publishing ? DART_PUB_ONLY : DART_INACTIVE);
        if (!s->ch[1] && role != DART_INACTIVE){
            DartSchema *sch = cap_topic_schema_parse(node, s->name);
            DartTopic *ch = i_dart_node_create_pattern_topic(node, s->name, role, sch,
                     &(DartTopicOpts){ .qos = { .reliability = DART_RELIABLE,
                                                .catch_up = (uint16_t)(variable ? 1 : 0) } },
                     variable ? DART_KIND_VARIABLE : DART_KIND_SIGNAL,
                     variable ? CAP_VAR_PREFIX : 0, 0, 0 /*forceable: observer never owns*/, NULL, NULL);
            if (sch) dart_schema_free(sch, cap_schema_alloc, NULL);
            if (!ch) return 0;
            dart_topic_dispatch(ch, 0, 0);            /* queued: deliveries drain on the UI thread */
            dart_node_lock(node);
            s->index[1] = dart_topic_index(ch);
            s->ch[1] = ch;
            dart_node_unlock(node);
        } else if (s->ch[1]){
            dart_topic_set_role(s->ch[1], role);
        }
        if (variable && s->publishing && !s->set_ch){
            char sn[DART_TOPIC_NAME_MAX + 8];
            DartSchema *sch = cap_topic_schema_parse(node, s->name);   /* set payload = value schema */
            snprintf(sn, sizeof sn, "%s@set", s->name);
            /* NO retention (catch_up 0): a set channel is multi-writer, and replay-on-match
               preserves only per-writer order, so retained ops from several writers reach a
               (re)matching owner in arbitrary order and a STALE write can win last-write-wins.
               An op racing the owner match parks in the topic's pending-send slot instead
               (flushed by cap_poll the moment matching resolves; see cap_send_routed). */
            s->set_ch = i_dart_node_create_pattern_topic(node, sn, DART_PUB_ONLY, sch,
                     &(DartTopicOpts){ .qos = { .reliability = DART_RELIABLE } },
                     DART_KIND_VAR_SET, CAP_SET_PREFIX, 0, 0, NULL, NULL);
            if (sch) dart_schema_free(sch, cap_schema_alloc, NULL);
            if (!s->set_ch) return 0;
        }
        s->reliable = 1;
        return 1;
    }
    if (s->kind == CAP_KIND_FUNCTION){
        if (s->publishing) s->subscribed = 1;   /* calling implies watching the call log */
        if ((s->subscribed || s->publishing) && !s->fn){
            char cn[DART_TOPIC_NAME_MAX + 8];
            DartSchema *req, *rsp;
            /* the provider's schemas: our request channel must pass its schema gate, and
               the reply decode needs the response shape */
            cap_primary_channel_name(cn, sizeof cn, s->name, CAP_KIND_FUNCTION, 0);
            req = cap_topic_schema_parse(node, cn);
            cap_primary_channel_name(cn, sizeof cn, s->name, CAP_KIND_FUNCTION, 1);
            rsp = cap_topic_schema_parse(node, cn);
            s->fn = dart_node_create_remote_function(node, s->name, req, rsp, NULL);
            if (!s->fn){
                if (req) dart_schema_free(req, cap_schema_alloc, NULL);
                if (rsp) dart_schema_free(rsp, cap_schema_alloc, NULL);
                return 0;
            }
            s->req_schema = req;   /* keep our parsed copies: form source + feed decode */
            s->rsp_schema = rsp;
        }
        s->reliable = 1;
        return 1;
    }
    return cap_sub_reconcile_topic(s, node, want);
}

/* the plain-topic path: make the topic for `want` reliability live with the combined role
   (SUB_ONLY/PUB_ONLY/PUBSUB), creating it once if needed, and turn the other one off.
   Data is unicast (point-to-point to each subscriber), so a publish reaches every
   subscriber and a subscribe hears every unicast publisher. Returns 1 on success. */
static int cap_sub_reconcile_topic(CapSub *s, DartNode *node, int want){
    DartRole role = (s->subscribed && s->publishing) ? DART_PUBSUB
                  :  s->subscribed                   ? DART_SUB_ONLY
                  :  s->publishing                   ? DART_PUB_ONLY
                  :                                    DART_INACTIVE;
    want = want ? 1 : 0;
    if (role == DART_INACTIVE){
        if (s->ch[0]) dart_topic_set_role(s->ch[0], DART_INACTIVE);
        if (s->ch[1]) dart_topic_set_role(s->ch[1], DART_INACTIVE);
        return 1;
    }
    if (!s->ch[want]){
        /* a typed topic gets a typed topic: adopt the schema its advertisers carry, so
           our publishes reach typed readers and the schema gate matches us. Trade-off:
           this topic then matches only that schema (the drawer flags topics whose
           publishers disagree); a topic with no advertised schema stays generic. */
        DartSchema *sch = cap_topic_schema_parse(node, s->name);
        DartTopic *ch = dart_node_create_topic(node, s->name, role, sch,
                 &(DartTopicOpts){ .qos = { .reliability = want ? DART_RELIABLE : DART_BEST_EFFORT,
                                              .catch_up = 1 } });
        if (sch) dart_schema_free(sch, cap_schema_alloc, NULL);   /* the node keeps its own copy */
        if (!ch) return 0;
        /* switch it to QUEUED delivery now (lazy ring, grows to DART_QUEUE_CAP): its
           messages then arrive via cap_poll's dispatch on the UI thread, never on the
           service thread. An observer never stalls a publisher: at the cap a best-effort
           queue drops oldest, and a parked reliable one only throttles publishers that
           opted into backpressure_wait_us. */
        dart_topic_dispatch(ch, 0, 0);
        /* publish the (index, handle) pair under the node lock: the service thread's
           event callback maps events back to topics through it (cap_sub_by_index) */
        dart_node_lock(node);
        s->index[want] = dart_topic_index(ch);
        s->ch[want] = ch;
        dart_node_unlock(node);
    } else {
        dart_topic_set_role(s->ch[want], role);
    }
    if (s->ch[!want]) dart_topic_set_role(s->ch[!want], DART_INACTIVE);   /* one live at a time */
    s->reliable = want;
    return 1;
}

int cap_subscribe(Capture *cap, const char *topic, int reliable){
    DartNode *node = cap ? (DartNode *)cap->rt : NULL;
    CapSub *s = node ? cap_sub_get(topic) : NULL;
    if (!node || !topic || !*topic) return 0;
    if (!s){ cap_logf("SUBSCRIBE %s refused (topic table full)", topic); return 0; }
    s->subscribed = 1;
    if (!cap_sub_reconcile(s, node, reliable ? 1 : 0)){ cap_logf("SUBSCRIBE %s failed (create_channel)", topic); return 0; }
    s->error = 0; s->n_drops = 0;          /* clear stale errors on a fresh subscribe */
    cap_logf("SUBSCRIBE %s (%s)", topic, s->reliable ? "reliable" : "best-effort");
    return 1;
}

int cap_unsubscribe(Capture *cap, const char *topic){
    DartNode *node = (cap && cap->rt) ? (DartNode *)cap->rt : NULL;
    CapSub *s = node ? cap_sub_find(topic) : NULL;
    if (!s) return 0;
    s->subscribed = 0;
    cap_sub_reconcile(s, node, s->reliable);   /* drops to PUB_ONLY if still publishing, else INACTIVE */
    cap_logf("UNSUBSCRIBE %s", topic);
    return 1;
}

int cap_declare_publish(Capture *cap, const char *topic){
    DartNode *node = cap ? (DartNode *)cap->rt : NULL;
    CapSub *s = node ? cap_sub_get(topic) : NULL;
    int was, want;
    if (!node || !topic || !*topic) return 0;
    if (!s){ cap_logf("PUBLISH %s refused (topic table full)", topic); return 0; }
    was = s->publishing;
    s->publishing = 1;
    /* publishers OFFER RELIABLE by default: RxO means a reliable offer satisfies every
       subscriber (best-effort ones stay out of flow control), and reliability is what arms
       writer-side retention (catch_up) so a first publish racing the match replays instead
       of vanishing (best-effort would silently void it, the classic TRANSIENT_LOCAL +
       BEST_EFFORT footgun). Only an existing SUBSCRIPTION pins the choice: one identity
       means one live topic, so the sub's reliability stands. */
    want = s->subscribed ? s->reliable : 1;
    if (!cap_sub_reconcile(s, node, want)){ s->publishing = was; cap_logf("PUBLISH %s failed (topic)", topic); return 0; }
    if (!was) cap_logf("PUBLISH %s declared (%s pub interest)", topic, want ? "reliable" : "best-effort");
    return 1;
}

/* route one outgoing payload by entity kind: plain publish, variable SET (op 0 over the
   set channel), signal emit (its own channel), or a function CALL (reply parks for
   cap_poll). Returns 1 and fills *echo_sch with the schema the local echo decodes with. */
#define CAP_SET_OP_FORCE   0x01u   /* set-channel ops, mirroring src/patterns/core.c */
#define CAP_SET_OP_UNFORCE 0x02u

/* the live channel that carries this topic's outgoing sends (functions never route here:
   a call goes through the caller handle) */
static DartTopic *cap_send_channel(CapSub *s){
    return s->kind == CAP_KIND_VARIABLE ? s->set_ch : s->ch[s->reliable];
}

static int cap_send_now(CapSub *s, DartTopic *ch, const void *data, size_t len, uint8_t var_op){
    if (s->kind == CAP_KIND_VARIABLE){
        uint8_t op = var_op;   /* 0 = a dumb write; force/unforce ride the same byte */
        return i_dart_topic_send_hdr(ch, dart_bytes(&op, 1), dart_bytes(data, len)) >= 0;
    }
    return dart_topic_send(ch, dart_bytes(data, len)) >= 0;
}

#define CAP_PEND_EXPIRE_MS 3000ull   /* a parked send that never matches is dropped, loudly */

/* park one send in the topic's pending slot: newest overwrites, cap_poll flushes on match
   (or on convergence-to-nobody: fire-and-forget resumes), expiry drops it with a log line */
static int cap_send_park(CapSub *s, const void *data, size_t len, uint8_t var_op){
    uint8_t *copy = (uint8_t *)malloc(len ? len : 1);
    if (!copy) return 0;
    if (len) memcpy(copy, data, len);
    free(s->pend_data);
    s->pend_data = copy; s->pend_len = (uint32_t)len; s->pend_op = var_op;
    s->pend_used = 1;
    s->pend_expire_ms = cap_now_ms() + CAP_PEND_EXPIRE_MS;
    cap_logf("%s %s parked: match still forming (flushes when it resolves)",
             s->kind == CAP_KIND_SIGNAL ? "EMIT" : s->kind == CAP_KIND_VARIABLE ? "SET" : "PUBLISH",
             s->name);
    return 1;
}

/* flush/expire parked sends, on the UI thread each frame (see cap_send_park) */
static void cap_pend_poll(void){
    unsigned long long now = cap_now_ms();
    int i;
    for (i = 0; i < CAP_MAX_SUBS; i++){
        CapSub *s = &cap_subs[i];
        DartTopic *ch;
        if (!s->used || !s->pend_used) continue;
        ch = cap_send_channel(s);
        if (ch && (dart_topic_match_count(ch) > 0 || dart_topic_ready(ch))){
            if (cap_send_now(s, ch, s->pend_data, s->pend_len, s->pend_op))
                cap_logf("%s parked send flushed (%d receiver%s)", s->name,
                         dart_topic_match_count(ch), dart_topic_match_count(ch) == 1 ? "" : "s");
            else
                cap_logf("%s parked send FAILED to flush", s->name);
        } else if (now < s->pend_expire_ms) continue;
        else cap_logf("%s parked send DROPPED: match unresolved for %ds", s->name,
                      (int)(CAP_PEND_EXPIRE_MS / 1000u));
        free(s->pend_data);
        s->pend_data = NULL; s->pend_len = 0; s->pend_used = 0;
    }
}

static int cap_send_routed(CapSub *s, const void *data, size_t len, uint8_t var_op,
                           const DartSchema **echo_sch){
    DartTopic *ch;
    *echo_sch = NULL;
    if (s->kind == CAP_KIND_FUNCTION){
        if (!s->fn || dart_function_call_async(s->fn, dart_bytes(data, len), cap_fn_on_reply, s, NULL) != DART_OK)
            return 0;   /* a call racing the provider match queues in the patterns layer */
        *echo_sch = s->req_schema;
        return 1;
    }
    ch = cap_send_channel(s);
    if (!ch) return 0;
    *echo_sch = dart_topic_schema(ch);
    /* Retention (reliable + catch_up, the explorer's plain-topic default) already covers a
       send racing the forming match: the writer replays it. The un-retainable sends (a
       variable set/force/unforce op is multi-writer, a signal emit never latches, a
       best-effort publish never replays) PARK instead when a match is still resolving:
       the node's blocking match wait is disabled here (never stall the UI thread), so the
       pending slot + cap_pend_poll are its async form, driven by dart_topic_ready. */
    if ((s->kind == CAP_KIND_VARIABLE || s->kind == CAP_KIND_SIGNAL || !s->reliable)
        && dart_topic_match_count(ch) == 0 && !dart_topic_ready(ch))
        return cap_send_park(s, data, len, var_op);
    return cap_send_now(s, ch, data, len, var_op);
}

int cap_publish(Capture *cap, const char *topic, const void *data, size_t len){
    CapSub *s; const DartSchema *echo;
    if (!topic || (!data && len)) return 0;
    if (!cap_declare_publish(cap, topic)) return 0;   /* ensure the publisher topic is live */
    s = cap_sub_find(topic);
    if (!s) return 0;
    if (!cap_send_routed(s, data, len, 0, &echo)){ cap_logf("PUBLISH %s send failed", topic); return 0; }
    if (echo && !dart_schema_validate(echo, dart_bytes(data, len))) echo = NULL;
    cap_ring_push(s, dart_cstr(cap_cfg.name), data, len, 1, echo);
    return 1;
}

#define CAP_FORM_MAX_ELEMS 64   /* variable-array elements the form accepts (a UI bound) */
#define CAP_FORM_SLACK 2048     /* message-buffer room for a form's variable content */

/* parse one form value into flat field i of the message being built (dart_set_value).
   Empty keeps the default; a struct or map row has no value of its own. */
static int cap_form_set(const DartSchema *sch, uint16_t i, const char *v, uint8_t *buf, size_t size){
    DartSchemaFieldInfo fi; DartValue val; char *end;
    if (!dart_schema_field_at(sch, i, &fi)) return 0;
    while (*v == ' ' || *v == '\t') v++;
    if (!*v || fi.kind == DART_STRUCT || fi.kind == DART_MAP)
        return 1;                                         /* empty/struct/map: the default stays */
    memset(&val, 0, sizeof val);
    switch ((DartSchemaTypeKind)fi.kind){
        case DART_BOOL:
            if      (!strcmp(v, "true"))  val.v.u = 1;
            else if (!strcmp(v, "false")) val.v.u = 0;
            else {
                val.v.u = strtoull(v, &end, 0);
                while (*end == ' ') end++;
                if (*end != '\0') return 0;
            }
            break;
        case DART_U8: case DART_U16: case DART_U32: case DART_U64:
            val.v.u = strtoull(v, &end, 0);
            while (*end == ' ') end++;
            if (*end != '\0') return 0;
            break;
        case DART_I8: case DART_I16: case DART_I32: case DART_I64:
            val.v.i = strtoll(v, &end, 0);
            while (*end == ' ') end++;
            if (*end != '\0') return 0;
            break;
        case DART_F32: case DART_F64:
            val.v.f = strtod(v, &end);
            while (*end == ' ') end++;
            if (*end != '\0') return 0;
            break;
        case DART_ENUM: {                                 /* an option NAME (the dropdown), or a raw number */
            int64_t ev;
            if (dart_enum_value_of(sch, i, v, &ev)) val.v.i = ev;
            else {
                val.v.i = strtoll(v, &end, 0);
                while (*end == ' ') end++;
                if (*end != '\0') return 0;               /* not a known name and not a number */
            }
            break;
        }
        case DART_STR: case DART_VSTR:                    /* the form text IS the content */
            val.bytes = dart_bytes(v, strlen(v));
            break;
        case DART_ARR: case DART_VARR: {                  /* comma/space-separated elements */
            uint32_t esz = (fi.elem == DART_STR) ? 2u + fi.str_cap
                                                 : dart_schema_scalar_size((DartSchemaTypeKind)fi.elem);
            uint16_t max_n = fi.kind == DART_VARR ? CAP_FORM_MAX_ELEMS : fi.count;
            uint8_t *w = (uint8_t *)malloc(esz ? (size_t)max_n * esz : 1);
            uint16_t n = 0; const char *p = v; int ok;
            if (!w || !esz){ free(w); return 0; }
            if (fi.elem == DART_STR){                     /* comma-separated strings */
                while (*p){
                    const char *q; size_t l;
                    while (*p == ' ' || *p == '\t') p++;
                    q = p; while (*q && *q != ',') q++;
                    l = (size_t)(q - p);
                    while (l && (p[l-1] == ' ' || p[l-1] == '\t')) l--;
                    if (n >= max_n || l > fi.str_cap){ free(w); return 0; }
                    i_dart_le_w16(w + (size_t)n * esz, (uint16_t)l);
                    memcpy(w + (size_t)n * esz + 2, p, l);
                    memset(w + (size_t)n * esz + 2 + l, 0, (size_t)fi.str_cap - l);
                    n++;
                    p = *q ? q + 1 : q;
                }
                val.bytes = dart_bytes(w, (size_t)n * esz);
                ok = dart_set_value(buf, size, sch, i, &val);
                free(w);
                return ok;
            }
            for (;;){
                while (*p == ' ' || *p == ',' || *p == '\t') p++;
                if (!*p || n >= max_n) break;
                if (fi.elem == DART_F32 || fi.elem == DART_F64){
                    double d = strtod(p, &end);
                    if (end == p) break;
                    if (fi.elem == DART_F32){ float x = (float)d; uint32_t b; memcpy(&b, &x, 4); i_dart_le_w32(w + (size_t)n*esz, b); }
                    else                    { uint64_t b; memcpy(&b, &d, 8); i_dart_le_w64(w + (size_t)n*esz, b); }
                } else if (fi.elem >= DART_I8 && fi.elem <= DART_I64){
                    long long d = strtoll(p, &end, 0);
                    if (end == p) break;
                    if      (esz == 1) w[n] = (uint8_t)d;
                    else if (esz == 2) i_dart_le_w16(w + (size_t)n*2, (uint16_t)d);
                    else if (esz == 4) i_dart_le_w32(w + (size_t)n*4, (uint32_t)d);
                    else               i_dart_le_w64(w + (size_t)n*8, (uint64_t)d);
                } else {                                   /* unsigned + bool elements */
                    unsigned long long u = strtoull(p, &end, 0);
                    if (end == p) break;
                    if      (esz == 1) w[n] = (uint8_t)u;
                    else if (esz == 2) i_dart_le_w16(w + (size_t)n*2, (uint16_t)u);
                    else if (esz == 4) i_dart_le_w32(w + (size_t)n*4, (uint32_t)u);
                    else               i_dart_le_w64(w + (size_t)n*8, (uint64_t)u);
                }
                p = end; n++;
            }
            while (*p == ' ' || *p == ',' || *p == '\t') p++;
            val.bytes = dart_bytes(w, (size_t)n * esz);
            ok = (*p == '\0') && dart_set_value(buf, size, sch, i, &val);
            free(w);
            return ok;                                    /* leftovers = junk or too many elements */
        }
        default: return 0;
    }
    return dart_set_value(buf, size, sch, i, &val);
}

static int cap_publish_form_op(Capture *cap, const char *topic, const char *const *values,
                               int n_values, uint8_t var_op){
    CapSub *s; const DartSchema *sch; uint8_t *buf; size_t size; uint16_t i, nf; int ok = 1;
    if (!topic || !values) return 0;
    if (!cap_declare_publish(cap, topic)) return 0;
    s = cap_sub_find(topic);
    /* the form's shape: a function fills its REQUEST schema; a variable its value schema
       (the set channel carries it); a plain topic its adopted channel schema */
    sch = !s ? NULL
        : s->kind == CAP_KIND_FUNCTION ? s->req_schema
        : s->kind == CAP_KIND_VARIABLE ? (s->set_ch ? dart_topic_schema(s->set_ch) : NULL)
        : s->ch[s->reliable]           ? dart_topic_schema(s->ch[s->reliable]) : NULL;
    if (!sch){ cap_logf("PUBLISH %s refused: topic carries no schema", topic); return 0; }
    size = dart_schema_msg_min(sch) + CAP_FORM_SLACK;     /* room for the variable content */
    buf = (uint8_t *)malloc(size);
    if (!buf) return 0;
    dart_schema_message_default(sch, buf, size);
    nf = dart_schema_field_count(sch);
    for (i = 0; i < nf && ok; i++)
        ok = cap_form_set(sch, i, (i < (uint16_t)n_values && values[i]) ? values[i] : "", buf, size);
    if (!ok){
        cap_logf("PUBLISH %s refused: field %u value invalid", topic, (unsigned)(i ? i - 1u : 0u));
        free(buf);
        return 0;
    }
    {   uint32_t msg_len = dart_schema_msg_len(sch, buf, size);
        const DartSchema *echo;
        if (!msg_len || !cap_send_routed(s, buf, msg_len, var_op, &echo)){
            cap_logf("PUBLISH %s send failed", topic);
            free(buf);
            return 0;
        }
        cap_ring_push(s, dart_cstr(cap_cfg.name), buf, msg_len, 1, sch);   /* decoded local echo */
        if ((var_op & CAP_SET_OP_FORCE) && s->ring){   /* mark the echo: this write PINS the value */
            int newest = (s->head - 1 + CAP_FEED_MAX) % CAP_FEED_MAX;
            s->ring[newest].forced = 1;
        }
    }
    free(buf);
    return 1;
}

int cap_publish_form(Capture *cap, const char *topic, const char *const *values, int n_values){
    return cap_publish_form_op(cap, topic, values, n_values, 0);
}

int cap_variable_force_form(Capture *cap, const char *topic, const char *const *values, int n_values){
    CapSub *s = cap ? cap_sub_find(topic) : NULL;
    if ((s && s->kind != CAP_KIND_VARIABLE)
        || (!s && cap_entity_kind(topic, NULL) != CAP_KIND_VARIABLE)){
        cap_logf("FORCE %s refused: not a variable", topic);
        return 0;
    }
    if (!cap_publish_form_op(cap, topic, values, n_values, CAP_SET_OP_FORCE)) return 0;
    cap_logf("FORCE %s (an owner without allow_force absorbs it silently)", topic);
    return 1;
}

int cap_variable_unforce(Capture *cap, const char *topic){
    CapSub *s; const DartSchema *echo;
    if (!cap_declare_publish(cap, topic)) return 0;   /* ensures the set channel exists */
    s = cap_sub_find(topic);
    if (!s || s->kind != CAP_KIND_VARIABLE || !s->set_ch){
        cap_logf("UNFORCE %s refused: not a variable", topic);
        return 0;
    }
    /* an op-only message (empty payload): routed like any other set op so a racing
       owner match parks it in the pending-send slot instead of dropping it */
    if (!cap_send_routed(s, NULL, 0, CAP_SET_OP_UNFORCE, &echo)){
        cap_logf("UNFORCE %s send failed", topic);
        return 0;
    }
    cap_logf("UNFORCE %s", topic);
    return 1;
}

int cap_topic_send_pending(const Capture *cap, const char *topic){
    const CapSub *s = (cap && cap->rt && topic) ? cap_sub_find(topic) : NULL;
    return s ? (int)s->pend_used : 0;
}

/* fill one observer endpoint from an entity yielded by the canonical reflection walk */
static void cap_endpoint_fill(CapTopic *e, const DartEntityInfo *ei){
    if (ei->name.data) snprintf(e->name, sizeof e->name, "%.*s", (int)ei->name.len, ei->name.data);
    else               snprintf(e->name, sizeof e->name, "0x%08x", (unsigned)ei->hash);
    /* a placeholder name resolves itself: the arriving details bump the interest epoch */
    e->index      = ei->index;
    e->reliable   = ei->reliable;
    e->kind       = (uint8_t)ei->kind;
    e->writable   = ei->writable;
    e->forceable  = ei->forceable;
    e->incomplete = ei->incomplete;
}

/* copy one peer's facts out of the node's zero-copy view into its observer record:
   discovery-level name/addr + liveness, then the peer's advertised ENTITIES via the
   patterns layer's canonical reflection walk (dart_node_peer_entity_next), so pattern
   channels arrive already folded into functions/variables/signals and this TU never
   sees the @-mangling. Each direction's list grows to the peer's actual topic count
   (cap_grow), so there is no fixed ceiling. */
static void cap_peer_refresh(DartNode *node, CapPeer *p, const DartDiscoveryPeer *dp){
    DartEntityIter it; DartEntityInfo ei;
    uint16_t frag = dart_node_peer_frag(dp);
    p->seen_frame = 1;
    p->state      = (dp->liveness == DART_PEER_DROPPED) ? CAP_DROPPED : CAP_ACTIVE;
    p->have_meta  = (dp->meta.data && dp->meta.len) ? 1 : 0;
    p->meta_stale = (dp->adv_meta_version > dp->meta_version) ? 1 : 0;
    p->frag       = frag;
    p->meta_len   = (uint16_t)dp->meta.len;
    memcpy(p->ip, dp->addr.ip, 16);
    p->ip_len = dp->addr.ip_len;
    p->port   = dp->addr.port;
    snprintf(p->name, sizeof p->name, "%.*s", (int)dp->name.len, dp->name.data ? dp->name.data : "");

    /* Rebuilding the interest list re-fetches every topic name from the node's detail cache,
       which is O(topics) per peer PER FRAME -- crippling at many-thousand-topic scale. The
       node's interest EPOCH is the one cache key: it bumps on every reflected change (a
       re-advertise, an external interest assembling, names paging in from the detail
       exchange), so skip the walk while it is unchanged and keep the cached lists.
       cap_snapshot still copies them out each frame; only this fetch is throttled. */
    {   uint32_t epoch = dart_node_peer_interest_epoch(dp);
        if (epoch == p->topics_epoch) return;
        p->topics_epoch = epoch;
    }

    p->n_pub = p->n_sub = 0;
    memset(&it, 0, sizeof it);
    while (dart_node_peer_entity_next(node, dp->id, &it, &ei)){
        /* provides = the entity's source side (publisher / provider / owner / emitter);
           consumes = its sink side. An entity on both sides lands in both lists. Names
           come from the detail cache (fetch_details fills it within an RTT); the hash is
           the placeholder until then. */
        if (ei.provides && cap_grow(&p->pub, &p->pub_cap, p->n_pub, sizeof *p->pub))
            cap_endpoint_fill(&p->pub[p->n_pub++], &ei);
        if (ei.consumes && cap_grow(&p->sub, &p->sub_cap, p->n_sub, sizeof *p->sub))
            cap_endpoint_fill(&p->sub[p->n_sub++], &ei);
    }
}

void cap_snapshot(const Capture *cap, CapSnapshot *out){
    DartNode *node = (DartNode *)cap->rt;
    unsigned long long now = cap_now_ms();
    const DartDiscoveryPeer *peers;
    uint16_t slot, n_peers = 0;
    int i, k;

    /* Do NOT memset the whole snapshot: the per-node entity buffers are grown high-water
       heap and zeroing them every frame would be a large per-frame cost. It is fully
       count-delimited instead: n_nodes /
       n_log / n_subs below bound the reader, and every used node/sub/log slot is completely
       overwritten as it is filled, so the untouched tail never needs clearing. */
    out->n_nodes   = 0;
    out->n_log     = 0;
    out->n_subs    = 0;
    out->domain    = cap_cfg.domain;
    out->disc_port = cap_cfg.port;
    snprintf(out->group, sizeof out->group, "%s", cap_cfg.group ? cap_cfg.group : "");

    /* one bracket for the whole copy-out (NULL-safe when the capture never started): it
       pins the zero-copy peer view AND excludes the service thread's event callback,
       which writes the log ring, cap_peers, and the sub error counters this reads. Do
       not log inside (the bracket is not nestable). */
    dart_node_lock(node);

    {   /* copy the observer event ring, newest first, capped to CAP_SNAP_LOG */
        int want = cap_log_count < CAP_SNAP_LOG ? cap_log_count : CAP_SNAP_LOG;
        for (k = 0; k < want; k++){
            int idx = cap_log_head - 1 - k;          /* head = next write slot */
            idx %= CAP_LOG_LINES; if (idx < 0) idx += CAP_LOG_LINES;
            memcpy(out->log[k], cap_log[idx], CAP_LOG_LINE);
        }
        out->n_log = want;
    }

    /* subscription state, so the UI can colour each topic's status light */
    for (i = 0; i < CAP_MAX_SUBS && out->n_subs < CAP_SNAP_SUBS; i++){
        CapSub *s = &cap_subs[i];
        CapSubInfo *si;
        if (!s->used) continue;
        si = &out->subs[out->n_subs++];
        snprintf(si->name, sizeof si->name, "%s", s->name);
        si->active     = s->subscribed;
        si->publishing = s->publishing;
        si->reliable   = s->reliable;
        si->error      = (s->error || s->n_drops > 0) ? 1 : 0;
        si->n_msgs     = (uint32_t)s->n_msgs;
        si->n_drops    = (uint32_t)s->n_drops;
        /* recency from the ring: age of the newest stored message (a live count-up). */
        si->last_age_s = -1.0;
        if (s->count > 0){
            double now_s  = (double)(now - cap_start_ms) / 1000.0;
            int    newest = (s->head - 1 + CAP_FEED_MAX) % CAP_FEED_MAX;
            si->last_age_s = now_s - s->ring[newest].t_s;
            if (si->last_age_s < 0.0) si->last_age_s = 0.0;
        }
        /* publish rate: messages received over an accurately measured window (exact count /
           high-res elapsed). Counting over wall-clock rather than a ring timespan avoids the
           ~15.6 ms stamp quantization that snapped a steady 250 Hz reading to two values.
           The window is ADAPTIVE so it spans all rates: it closes once we have enough
           messages for a stable estimate (fast topics update ~1 Hz) OR a max time has passed
           (a slow, sub-Hz, or now-silent topic still reports, and silence decays to 0). */
        {   double t_hr = cap_now_hr(), dt = t_hr - s->rate_prev_hr;
            unsigned long long dmsgs = s->n_msgs - s->rate_prev_msgs;
            if (s->rate_prev_hr <= 0.0){             /* first sample on this slot: seed the baseline */
                s->rate_prev_hr = t_hr; s->rate_prev_msgs = s->n_msgs;
            } else if (dt >= 1.0 && (dmsgs >= 4 || dt >= 4.0)){
                s->rate_hz = (double)dmsgs / dt;
                s->rate_prev_hr = t_hr; s->rate_prev_msgs = s->n_msgs;
            }
        }
        si->rate_hz = s->rate_hz;
        si->jitter_p90_ms = s->jitter_n >= 4 ? s->jitter_p90_ms : -1.0;   /* a few intervals to warm up */
    }

    /* pull the node's zero-copy peer view (it decodes the overlay for us) and refresh each
       observer record; peers no longer in the node's table fold to GONE, kept de-emphasized. */
    for (i = 0; i < CAP_MAX_PEERS; i++) cap_peers[i].seen_frame = 0;
    peers = dart_node_peers(node, &n_peers);
    for (slot = 0; slot < n_peers; slot++)
        cap_peer_refresh(node, cap_peer_get(peers[slot].id), &peers[slot]);
    for (i = 0; i < CAP_MAX_PEERS; i++){
        CapPeer *p = &cap_peers[i];
        if (p->used && !p->seen_frame && p->state != CAP_GONE){   /* evicted from the node table */
            p->state = CAP_GONE;
            if (!p->down_ms) p->down_ms = now;
        }
    }

    /* A node stopped and brought back gets a fresh UUID (hence a new local id), so its old
       GONE record would linger beside the new live one under the same name. Drop any GONE
       peer whose name a present-this-frame peer now carries: the node returned, replace the
       stale twin. (Named nodes only: auto "node-XXXX" names differ every boot by design.) */
    for (i = 0; i < CAP_MAX_PEERS; i++){
        CapPeer *live = &cap_peers[i];
        int j;
        if (!live->used || !live->seen_frame || !live->name[0]) continue;
        for (j = 0; j < CAP_MAX_PEERS; j++){
            CapPeer *g = &cap_peers[j];
            if (j == i || !g->used || g->state != CAP_GONE) continue;
            if (!strcmp(g->name, live->name)) g->used = 0;   /* free the stale GONE twin */
        }
    }

    for (i = 0; i < CAP_MAX_PEERS && out->n_nodes < CAP_SNAP_NODES; i++){
        const CapPeer *p = &cap_peers[i];
        CapNode *n;
        if (!p->used) continue;
        n = &out->nodes[out->n_nodes++];
        n->state      = (CapState)p->state;   /* CAP_ACTIVE/DROPPED/GONE align with CapState */
        n->have_meta  = p->have_meta;
        n->meta_stale = p->meta_stale;
        n->frag       = p->frag;
        n->meta_len   = p->meta_len;
        n->port       = p->port;
        n->updates    = p->updates;
        n->observed_s = (double)(now - p->first_seen_ms) / 1000.0;
        n->age_s      = p->last_change_ms ? (double)(now - p->last_change_ms) / 1000.0 : -1.0;
        snprintf(n->name, sizeof n->name, "%s", p->name);
        cap_fmt_ip(n->ip, sizeof n->ip, p->ip, p->ip_len);

        /* grow the node's snapshot buffers to the peer's actual counts (high-water, reused
           across frames), then copy (memcpy, not snprintf: the name is already a NUL-
           terminated 65-byte field, and per-entry snprintf x thousands is slow) */
        for (k = 0; k < p->n_pub && cap_grow(&n->pub, &n->pub_cap, k, sizeof *n->pub); k++){
            memcpy(n->pub[k].name, p->pub[k].name, sizeof n->pub[k].name);
            n->pub[k].index      = p->pub[k].index;
            n->pub[k].reliable   = p->pub[k].reliable;
            n->pub[k].kind       = p->pub[k].kind;
            n->pub[k].writable   = p->pub[k].writable;
            n->pub[k].forceable  = p->pub[k].forceable;
            n->pub[k].incomplete = p->pub[k].incomplete;
        }
        n->n_pub = k;
        for (k = 0; k < p->n_sub && cap_grow(&n->sub, &n->sub_cap, k, sizeof *n->sub); k++){
            memcpy(n->sub[k].name, p->sub[k].name, sizeof n->sub[k].name);
            n->sub[k].index      = p->sub[k].index;
            n->sub[k].reliable   = p->sub[k].reliable;
            n->sub[k].kind       = p->sub[k].kind;
            n->sub[k].writable   = p->sub[k].writable;
            n->sub[k].forceable  = p->sub[k].forceable;
            n->sub[k].incomplete = p->sub[k].incomplete;
        }
        n->n_sub = k;
    }

    dart_node_unlock(node);
}

int cap_topic_feed(const Capture *cap, const char *topic, CapFeedItem *out, int max,
                   int *subscribed, int *error, int *reliable, uint32_t *n_msgs, uint32_t *n_drops){
    CapSub *s = (cap && cap->rt && topic) ? cap_sub_find(topic) : NULL;
    int i, n = 0, start;
    /* the ring itself is UI-thread-owned (queued delivery), but error/n_drops are
       written by the service thread's event callback: bracket the read */
    if (cap && cap->rt) dart_node_lock((DartNode *)cap->rt);
    if (subscribed) *subscribed = s ? s->subscribed : 0;
    if (error)      *error      = s ? ((s->error || s->n_drops > 0) ? 1 : 0) : 0;
    if (reliable)   *reliable   = s ? s->reliable : 0;
    if (n_msgs)     *n_msgs     = s ? (uint32_t)s->n_msgs : 0;
    if (n_drops)    *n_drops    = s ? (uint32_t)s->n_drops : 0;
    if (!s || !out || max <= 0){
        if (cap && cap->rt) dart_node_unlock((DartNode *)cap->rt);
        return 0;
    }

    /* walk the ring oldest first; with a full ring the oldest is at head */
    n     = s->count < max ? s->count : max;
    start = (s->head - n) % CAP_FEED_MAX; if (start < 0) start += CAP_FEED_MAX;
    for (i = 0; i < n; i++){
        const CapMsgRec *m = &s->ring[(start + i) % CAP_FEED_MAX];
        CapFeedItem *o = &out[i];
        int k;
        o->t_s         = m->t_s;
        o->uid         = m->uid;
        o->len         = m->len;
        o->preview_len = m->preview_len;
        o->mine        = m->mine;
        o->forced      = m->forced;
        o->decoded     = m->decoded;
        o->n_fields    = m->n_fields;
        o->total_fields= m->total_fields;
        for (k = 0; k < m->n_fields; k++) o->fields[k] = m->fields[k];
        snprintf(o->type_name, sizeof o->type_name, "%s", m->type_name);
        snprintf(o->sender, sizeof o->sender, "%s", m->sender);
        if (m->preview_len) memcpy(o->preview, m->preview, m->preview_len);
        if (m->preview_len < CAP_MSG_PREVIEW) o->preview[m->preview_len] = '\0';  /* terminate: %s reads it, slot is reused */
    }
    if (cap && cap->rt) dart_node_unlock((DartNode *)cap->rt);
    return n;
}

/* Discard everything accumulated for `topic`: the stored message ring, the message/drop
   counters, the error mark, and the derived rate/jitter estimators (which are lifetime
   state, not ring state, so they must reset with it). The live subscription and publish
   interest are untouched, so the feed simply restarts from the next message. uid_next is
   kept: recycling uids would let a fresh message match a pinned inspect copy. */
int cap_topic_clear(Capture *cap, const char *topic){
    DartNode *node = (cap && cap->rt) ? (DartNode *)cap->rt : NULL;
    CapSub   *s    = (node && topic) ? cap_sub_find(topic) : NULL;
    if (!s) return 0;
    /* the ring is UI-thread-owned (so is this call), but error/n_drops are written by the
       service thread's event callback: bracket the reset like cap_topic_feed brackets its read */
    dart_node_lock(node);
    free(s->ring); s->ring = NULL;          /* the next message reallocates it lazily */
    s->head = s->count = 0;
    s->n_msgs = s->n_drops = 0;
    s->error = 0;
    s->rate_hz = 0.0; s->rate_prev_hr = 0.0; s->rate_prev_msgs = 0;
    s->jitter_last_hr = 0.0; s->jitter_mean_ms = 0.0; s->jitter_p90_ms = 0.0; s->jitter_n = 0;
    dart_node_unlock(node);
    cap_logf("CLEAR %s (feed + counters)", topic);
    return 1;
}

/* ------------------------------------------------------------------ topic schema */

/* schema decode scratch: a realloc hook for the short-lived parsed copy */
static void *cap_schema_alloc(void *user, void *ptr, size_t size){
    (void)user;
    if (size == 0){ free(ptr); return NULL; }
    return realloc(ptr, size);
}

static const char *cap_kind_str(uint8_t kind){
    switch ((DartSchemaTypeKind)kind){
        case DART_U8:  return "u8";  case DART_U16: return "u16";
        case DART_U32: return "u32"; case DART_U64: return "u64";
        case DART_I8:  return "i8";  case DART_I16: return "i16";
        case DART_I32: return "i32"; case DART_I64: return "i64";
        case DART_F32: return "f32"; case DART_F64: return "f64";
        case DART_BOOL: return "bool";
        case DART_STR: case DART_VSTR: return "string";
        case DART_STRUCT: return "struct";
        case DART_MAP: return "map";
        default: return "?";
    }
}

/* a field's DSL type spelling: "u64", "string<33>", "f32[8]", "string<16>[]", "map",
   "enum<u8>". A struct is NOT spelled here: in the DSL it is `name: { members }`, handled
   by the caller. An enum's backing integer kind is reported in fi->elem. */
static void cap_field_type_str(char *dst, size_t cap, const DartSchemaFieldInfo *fi){
    if (fi->kind == DART_ENUM)
        snprintf(dst, cap, "enum<%s>", cap_kind_str(fi->elem));
    else if (fi->kind == DART_STR)
        snprintf(dst, cap, "string<%u>", fi->str_cap);
    else if (fi->kind == DART_ARR && fi->elem == DART_STR)
        snprintf(dst, cap, "string<%u>[%u]", fi->str_cap, fi->count);
    else if (fi->kind == DART_ARR)
        snprintf(dst, cap, "%s[%u]", cap_kind_str(fi->elem), fi->count);
    else if (fi->kind == DART_VARR && fi->elem == DART_STR)
        snprintf(dst, cap, "string<%u>[]", fi->str_cap);
    else if (fi->kind == DART_VARR)
        snprintf(dst, cap, "%s[]", cap_kind_str(fi->elem));
    else
        snprintf(dst, cap, "%s", cap_kind_str(fi->kind));
}

/* fill out->fields from a parsed schema (top-level fields, capped to the UI's bound) */
static void cap_schema_fields(CapSchema *out, const DartSchema *sch){
    DartSchemaFieldInfo fi; uint16_t i, nf = dart_schema_field_count(sch);
    out->inlined      = 1;
    out->msg_size     = dart_schema_msg_min(sch);   /* exact size, or the minimum with variable fields */
    out->total_fields = nf;
    cap_schema_type_name(out->type_name, sizeof out->type_name, sch);
    for (i = 0; i < nf && out->n_fields < CAP_SCHEMA_FIELDS; i++){
        CapSchemaField *f = &out->fields[out->n_fields];
        if (!dart_schema_field_at(sch, i, &fi)) break;
        cap_field_label(f->name, sizeof f->name, &fi);
        cap_field_type_str(f->type, sizeof f->type, &fi);
        f->kind    = fi.kind;                                        /* CAP_K_* == DartSchemaTypeKind */
        f->elem    = (uint8_t)((fi.kind == DART_ARR || fi.kind == DART_VARR
                                || fi.kind == DART_ENUM) ? fi.elem : 0);   /* ENUM: the backing kind */
        f->count   = (uint16_t)(fi.kind == DART_ARR ? fi.count : 0);
        f->str_cap = fi.str_cap;                                     /* set for STR + STR-element arrays */
        f->depth  = (uint8_t)(fi.depth > 255 ? 255 : fi.depth);
        f->offset = fi.offset;
        f->size   = fi.size;
        f->n_variants = 0;
        if (fi.kind == DART_ENUM){   /* copy the option NAMES so the UI can offer a dropdown (no DART here) */
            uint16_t nv = dart_schema_enum_count(sch, i), k;
            for (k = 0; k < nv && f->n_variants < CAP_ENUM_VARIANTS; k++){
                DartString vn;
                if (!dart_schema_enum_variant(sch, i, k, NULL, &vn)) break;
                snprintf(f->variants[f->n_variants], CAP_ENUM_NAME, "%.*s",
                         (int)vn.len, vn.data ? vn.data : "");
                f->n_variants++;
            }
        }
        out->n_fields++;
    }
}

/* Pick a topic's MAIN schema: the widest compatible one advertised by any endpoint
   (publisher OR subscriber), preferring a publisher since it owns the wire bytes. This is
   the SINGLE source of truth for a topic's schema, so the schema DISPLAY (cap_topic_schema),
   the TOPIC the explorer adopts to subscribe/publish (cap_sub_reconcile via
   cap_topic_schema_parse), and therefore the feed's decode + column set all agree. Picking
   the first-found schema instead let a subscriber advertising a SUBSET make the explorer
   adopt (and decode every message down to) the narrower schema, while the inspector showed
   the wider one -- the feed then offered only the subset's columns.
   Returns the node-owned parsed schema (valid while the node lock is held; NULL = hash-only
   or nobody advertises one). The node lock MUST be held by the caller. The optional
   out-params report the winning hash, the source endpoint name, and the advertiser count /
   structural-conflict flag (display only). */
static const DartSchema *cap_topic_pick_schema(DartNode *node, const char *topic,
                              uint64_t *out_hash, char *from, size_t from_cap,
                              int *n_advertisers, int *hash_conflict){
    const DartDiscoveryPeer *peers; uint16_t n_peers = 0, s;
    size_t tlen = topic ? strlen(topic) : 0;
    uint32_t want_hash;
    const DartSchema *best = NULL;
    int best_is_pub = 0, advertisers = 0, conflict = 0;
    uint64_t best_hash = 0;
    if (out_hash) *out_hash = 0;
    if (from && from_cap) from[0] = '\0';
    if (n_advertisers) *n_advertisers = 0;
    if (hash_conflict) *hash_conflict = 0;
    if (!node || tlen == 0) return NULL;
    want_hash = (uint32_t)dart_topic_id(topic);
    peers = dart_node_peers(node, &n_peers);
    for (s = 0; s < n_peers; s++){
        const DartDiscoveryPeer *dp = &peers[s];
        DartInterestIter it; DartTopicEntry t;
        uint16_t last_alias = 0xFFFF;   /* PUBSUB yields both directions: one schema per index */
        memset(&it, 0, sizeof it);
        while (dart_node_peer_interest_next(dp, &it, &t)){
            DartString nm; uint64_t hash = 0; const DartSchema *sch;
            int take = 0;
            if (t.hash != want_hash) continue;          /* cheap prefilter before the O(topics) name lookup */
            if (t.index == last_alias) continue;        /* second direction of a PUBSUB entry */
            last_alias = t.index;
            /* any endpoint (publisher OR subscriber) is authoritative about its schema:
               a subscriber-in-charge topic advertises the shape its generic publisher fills */
            nm = dart_node_peer_topic_name(node, dp->id, t.index);
            if (nm.len != tlen || memcmp(nm.data, topic, tlen) != 0) continue;
            sch = dart_node_peer_topic_schema(node, dp->id, t.index, &hash);
            if (!hash) continue;                        /* untyped endpoint */
            if (advertisers == 0){                      /* first advertiser: adopt it */
                advertisers = 1; take = 1;
            } else if (hash == best_hash){              /* identical schema: agree */
                advertisers++;
                /* upgrade the shown source to a publisher (owns the wire) or, if we only
                   had the hash before, to the parsed form so the field list can render */
                take = (t.is_pub && !best_is_pub) || (sch && !best);
            } else if (best && sch){
                /* DIFFERENT hash but structurally compatible is NOT a conflict: subset
                   binding lets a narrower reader consume a wider writer (dart_schema_subset,
                   the same gate the C matcher runs). Take the WIDER schema, preferring a
                   publisher since it owns the actual wire bytes. */
                int best_narrower = dart_schema_subset(best, sch);   /* best subset of sch: sch is wider */
                int new_narrower  = dart_schema_subset(sch, best);   /* sch subset of best: best is wider */
                if (!best_narrower && !new_narrower){   /* neither reads the other: real conflict */
                    conflict = 1;
                    continue;
                }
                advertisers++;
                take = (t.is_pub && !best_is_pub)                    /* publisher wins the wire */
                    || (t.is_pub == best_is_pub && best_narrower);   /* same role: the wider wins */
            } else {                                    /* one side hash-only: cannot prove subset */
                conflict = 1;
                continue;
            }
            if (take){
                best = sch; best_is_pub = t.is_pub; best_hash = hash;
                if (from && from_cap)
                    snprintf(from, from_cap, "%.*s", (int)dp->name.len, dp->name.data ? dp->name.data : "");
            }
        }
    }
    if (out_hash)      *out_hash      = best_hash;
    if (n_advertisers) *n_advertisers = advertisers;
    if (hash_conflict) *hash_conflict = conflict;
    return best;
}

/* the topic's MAIN schema (see cap_topic_pick_schema) as a freeable parsed copy: the
   cached one is node-owned, so hand back a reparse of its canonical wire. The caller frees
   it via cap_schema_alloc. NULL when nobody advertises one (or details are still in
   flight). Used for the TOPIC the explorer adopts, so it decodes at the same width the
   inspector shows. */
static DartSchema *cap_topic_schema_parse(DartNode *node, const char *topic){
    const DartSchema *best; DartSchema *copy = NULL;
    if (!node || !topic || !*topic) return NULL;
    dart_node_lock(node);   /* the zero-copy peer view + node-owned schema vs the service thread */
    best = cap_topic_pick_schema(node, topic, NULL, NULL, 0, NULL, NULL);
    if (best){
        DartBytes wire = dart_schema_wire(best);
        copy = dart_schema_parse(wire.data, wire.len, cap_schema_alloc, NULL);
    }
    dart_node_unlock(node);
    return copy;
}

/* the schema query for one of an entity's channels: the base name for topics/variables/
   signals (their primary channel IS the bare name), the mangled request or response
   channel for a function. `rsp` asks a function for its response schema. */
static int cap_entity_schema(const Capture *cap, const char *topic, int rsp, CapSchema *out){
    DartNode *node = cap ? (DartNode *)cap->rt : NULL;
    const DartSchema *best; int n_adv = 0;
    char cn[DART_TOPIC_NAME_MAX + 8];
    uint8_t kind;
    memset(out, 0, sizeof *out);
    if (!node || !topic || !*topic) return 0;
    kind = cap_entity_kind(topic, NULL);
    if (rsp && kind != CAP_KIND_FUNCTION) return 0;   /* only functions have a response shape */
    cap_primary_channel_name(cn, sizeof cn, topic, kind, rsp);
    dart_node_lock(node);   /* the zero-copy peer view vs the service thread */
    best = cap_topic_pick_schema(node, cn, &out->hash, out->from, sizeof out->from,
                                 &n_adv, &out->hash_conflict);
    out->n_advertisers = n_adv;
    if (best) cap_schema_fields(out, best);             /* cached parsed schema: field list */
    dart_node_unlock(node);
    return n_adv > 0;
}

int cap_topic_schema(const Capture *cap, const char *topic, CapSchema *out){
    return cap_entity_schema(cap, topic, 0, out);
}

int cap_topic_rsp_schema(const Capture *cap, const char *topic, CapSchema *out){
    return cap_entity_schema(cap, topic, 1, out);
}

int cap_topic_schema_dsl(const Capture *cap, const char *topic, int rsp, char *out, size_t out_cap){
    DartNode *node = cap ? (DartNode *)cap->rt : NULL;
    DartSchema *sch;
    char cn[DART_TOPIC_NAME_MAX + 8];
    uint8_t kind;
    uint32_t n;
    if (out && out_cap) out[0] = '\0';
    if (!node || !topic || !*topic || !out || out_cap == 0) return 0;
    kind = cap_entity_kind(topic, NULL);
    if (rsp && kind != CAP_KIND_FUNCTION) return 0;
    cap_primary_channel_name(cn, sizeof cn, topic, kind, rsp);
    sch = cap_topic_schema_parse(node, cn);      /* full parsed schema (takes the node lock) */
    if (!sch) return 0;
    n = dart_schema_print(sch, out, out_cap);    /* the library spells the DSL */
    dart_schema_free(sch, cap_schema_alloc, NULL);
    return (int)n;
}

/* Live per-field validity for the publish form. Parses the topic's advertised schema
   once (the same source the form is built from), then runs each value through cap_form_set
   against a scratch message buffer: the authoritative accept test cap_publish_form applies,
   with no side effects (no publisher declared, nothing sent). An empty value is valid (it
   keeps the field's default). */
int cap_form_validate(const Capture *cap, const char *topic, const char *const *values,
                      int n_values, unsigned char *valid){
    DartNode *node = cap ? (DartNode *)cap->rt : NULL;
    DartSchema *sch; uint8_t *buf; size_t size; uint16_t i, nf;
    char cn[DART_TOPIC_NAME_MAX + 8];
    int j;
    for (j = 0; j < n_values; j++) if (valid) valid[j] = 1;   /* default: don't flag */
    if (!node || !topic || !values) return 0;
    cap_primary_channel_name(cn, sizeof cn, topic, cap_entity_kind(topic, NULL), 0);
    sch = cap_topic_schema_parse(node, cn);   /* a function's form fills its REQUEST schema */
    if (!sch) return 0;                                       /* no schema: nothing to judge against */
    size = dart_schema_msg_min(sch) + CAP_FORM_SLACK;
    buf = (uint8_t *)malloc(size);
    if (!buf){ dart_schema_free(sch, cap_schema_alloc, NULL); return 0; }
    dart_schema_message_default(sch, buf, size);              /* per-field parse is independent of the rest */
    nf = dart_schema_field_count(sch);
    for (i = 0; i < nf && (int)i < n_values; i++){
        const char *v = values[i] ? values[i] : "", *p = v;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) continue;                                   /* empty: keeps the default, valid */
        if (valid) valid[i] = (unsigned char)(cap_form_set(sch, i, v, buf, size) ? 1 : 0);
    }
    free(buf);
    dart_schema_free(sch, cap_schema_alloc, NULL);
    return 1;
}
