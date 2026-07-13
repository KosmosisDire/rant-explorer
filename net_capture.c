/* Live DART observer, built on a real NODE (not bare discovery). The node owns the
   discovery->transport wiring and decodes every peer's announce-metadata overlay, so
   this TU reads peers back through the node's peer API (dart_node_peers) and never
   parses the overlay itself. It runs with no channels of its own (it publishes and
   subscribes nothing); a generous channel reserve only sizes discovery's incoming
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
#define CAP_MAX_TOPICS        16000  /* pub or sub interest entries captured per peer (>= wire ceiling) */
#define CAP_OBSERVER_CHANNELS 64     /* node channel reserve: our OWN subscriptions only (<=2 channels per
                                        topic, see CAP_MAX_SUBS = 32). Deliberately NOT sized to a big
                                        peer's topic count: max_channels also reserves per-channel history
                                        buffers, so a large value costs hundreds of MB. A many-topic peer
                                        whose announce blob exceeds our initial accept bound is handled by
                                        the node's self-heal instead (it reads the blob's true length, grows
                                        the bound + RX buffers up to the 65000-byte cap, and re-solicits),
                                        so the peer's full interest lands within an announce interval. */
#define CAP_LOG_LINES         400
/* CAP_LOG_LINE comes from net_capture.h (shared with the snapshot) */

typedef enum { CAP_ACTIVE = 0, CAP_DROPPED = 1, CAP_GONE = 2 } CapPeerState;

typedef struct {
    char     name[DART_TOPIC_NAME_MAX + 1];
    uint16_t alias;
    int      reliable;       /* offered (pub) / requested (sub): flags bit 0 */
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
    int      n_pub, n_sub;
    CapTopic pub[CAP_MAX_TOPICS];
    CapTopic sub[CAP_MAX_TOPICS];
    uint32_t topics_ver;     /* dp->meta_version the pub/sub lists were last built from */
    int      names_pending;  /* 1 = a topic name was still an unfetched placeholder last build */

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
   there: every explorer channel is QUEUED (consumer queues), and cap_poll drains the
   queues on the UI thread each frame (dart_node_dispatch -> cap_on_message), so the
   feed rings and their metrics stay UI-thread-owned with no locking. Only cap_on_event
   still fires on the service thread (peer/log/error bookkeeping); it always runs WITH
   the node lock held, so the shared bits (the log ring, cap_peers, the cap_subs
   channel bookkeeping) are serialized by bracketing the UI side's access with
   dart_node_lock/dart_node_unlock (cap_node below is that handle). The bracket is not
   nestable (an inner unlock releases the outer hold), so bracketed code never calls
   another bracketing helper and never logs. */
static DartNode *cap_node;

static unsigned long long cap_now_ms(void);   /* defined below; used by cap_ring_push */
static void *cap_schema_alloc(void *user, void *ptr, size_t size);          /* defined below */
static DartSchema *cap_topic_schema_parse(DartNode *node, const char *topic);/* defined below */

/* one observer-side topic the explorer is using: a ring of recent messages plus the DART
   channel(s) wiring it up. A channel's reliability is FIXED at creation, but our wanted
   reliability can change as publishers come and go, so a topic owns up to TWO channels
   (ch[0] best-effort, ch[1] reliable), created on demand, with exactly one live at a time.
   That live channel's ROLE is the union of what we want: SUB_ONLY (receive), PUB_ONLY
   (send), or PUBSUB (both) -- one channel does both directions, since two live channels of
   the same identity would misroute. cap_sub_reconcile keeps it in sync. The message/event
   callbacks find the topic by either channel's local index. */
#define CAP_MAX_SUBS 32              /* distinct topics in use (each may own 2 channels) */

typedef struct {
    double   t_s;
    uint32_t uid;                         /* per-topic message id (UI expand/collapse key) */
    uint32_t len;
    uint16_t preview_len;
    int      mine;                        /* 1 = we published it (local echo) */
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
    int          publishing;             /* user has sent here, so the channel stays PUB-capable */
    int          error;                  /* a QoS-incompatible / oversize event hit the live channel */
    int          reliable;               /* reliability of the currently live channel (0/1) */
    char         name[DART_TOPIC_NAME_MAX + 1];
    DartChannel *ch[2];                  /* [0] best-effort, [1] reliable; NULL until first needed */
    uint16_t     index[2];               /* their channel indices (valid where ch[i] != NULL) */
    unsigned long long n_msgs;
    unsigned long long n_drops;
    uint32_t     uid_next;               /* next message uid (expand/collapse key) */
    CapMsgRec    ring[CAP_FEED_MAX];
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
/* one field's value text, from its reflected DartValue */
static void cap_fmt_value(char *dst, int cap, const DartSchemaFieldInfo *fi, const DartValue *v){
    int at = 0;
    switch ((DartSchemaTypeKind)fi->kind){
        case DART_BOOL: snprintf(dst, (size_t)cap, "%s", v->v.u ? "true" : "false"); break;
        case DART_U8: case DART_U16: case DART_U32: case DART_U64:
            snprintf(dst, (size_t)cap, "%llu", (unsigned long long)v->v.u); break;
        case DART_I8: case DART_I16: case DART_I32: case DART_I64:
            snprintf(dst, (size_t)cap, "%lld", (long long)v->v.i); break;
        case DART_F32: case DART_F64:
            snprintf(dst, (size_t)cap, "%g", v->v.f); break;
        case DART_ARR:    dst[0] = '\0'; cap_fmt_arr(dst, cap, &at, v->bytes, fi->elem, fi->count, fi->str_cap, 0, 0); break;
        case DART_STR:    snprintf(dst, (size_t)cap, "\"%.*s\"", (int)v->bytes.len,
                                   v->bytes.data ? (const char *)v->bytes.data : ""); break;
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
                                  uint16_t nf, uint16_t field_idx, const DartSchemaFieldInfo *fi,
                                  const DartValue *v){
    if (fi->kind == DART_STRUCT){
        if (fi->depth < CAP_PREVIEW_MAX_DEPTH){
            uint16_t idx = (uint16_t)(field_idx + 1);
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
    } else if (fi->kind == DART_STR){
        *at = cap_val_append(dst, cap, *at, "\"%.*s\"", (int)v->bytes.len,
                             v->bytes.data ? (const char *)v->bytes.data : "");
    } else if (fi->kind == DART_F32 || fi->kind == DART_F64){
        cap_fmt_float(dst, cap, at, v->v.f);
    } else {
        char one[32];
        cap_fmt_value(one, (int)sizeof one, fi, v);
        *at = cap_val_append(dst, cap, *at, "%s", one);
    }
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
            snprintf(f->name, sizeof f->name, "%.*s", (int)fi.name.len, fi.name.data ? fi.name.data : "");
            f->depth = (uint8_t)(fi.depth > 255 ? 255 : fi.depth);
            cap_fmt_value(f->value, (int)sizeof f->value, &fi, &v);
        }
        if (fi.depth == 0){                                /* the collapsed summary line */
            at = cap_val_append(m->preview, CAP_MSG_PREVIEW, at, at ? "  %.*s=" : "%.*s=",
                                (int)fi.name.len, fi.name.data ? fi.name.data : "");
            cap_fmt_value_preview(m->preview, CAP_MSG_PREVIEW, &at, s, data, nf, i, &fi, &v);
        }
    }
    m->preview_len = (uint16_t)strlen(m->preview);
}

/* append one message to a topic's ring (received or our own echo). Does not touch
   n_msgs. With a schema the message is reflected into field rows; raw bytes else. */
static void cap_ring_push(CapSub *s, DartString sender, const void *data, size_t len, int mine,
                          const DartSchema *schema){
    CapMsgRec *m = &s->ring[s->head];
    m->t_s = (double)(cap_now_ms() - cap_start_ms) / 1000.0;
    m->uid = ++s->uid_next;
    m->len = (uint32_t)len;
    m->decoded = 0; m->type_name[0] = '\0';
    m->n_fields = m->total_fields = 0;
    m->preview_len = 0;
    if (schema){
        DartString tn = dart_schema_name(schema);
        cap_decode_fields(m, dart_bytes(data, len), schema);
        m->decoded = 1;
        snprintf(m->type_name, sizeof m->type_name, "%.*s", (int)tn.len, tn.data ? tn.data : "");
    } else {
        uint16_t c = len < CAP_MSG_PREVIEW ? (uint16_t)len : CAP_MSG_PREVIEW;
        if (c) memcpy(m->preview, data, c);
        m->preview_len = c;
    }
    m->mine = mine;
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
    memset(p, 0, sizeof *p);
    p->used = 1;
    p->local_id = id;
    p->first_seen_ms = cap_now_ms();
    return p;
}

/* the node's message sink: append a delivered message to its topic ring. Every explorer
   channel is queued, so this only ever runs on the UI thread (cap_poll's dispatch) --
   the rings and their metrics need no locking. We keep only a short payload preview. */
static void cap_on_message(const DartMsg *msg){
    CapSub *s = cap_sub_by_index(msg->channel_id);
    if (!s) return;
    /* jitter from the POLL-side arrival stamp, not from when this frame got around to
       dispatching: a frame-paced consumer must never quantize inter-arrival times */
    cap_jitter_update(s, (double)msg->recv_us / 1e6);
    cap_ring_push(s, msg->sender_name, msg->data.data, msg->data.len, 0, msg->schema);
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
        CapSub *s = cap_sub_by_index(ev->channel);
        if (s) s->n_drops += ev->lost_count;
        cap_logf("        LOST  ch=%u peer=%u first=%llu count=%llu", ev->channel, ev->peer,
                 (unsigned long long)ev->lost_first, (unsigned long long)ev->lost_count);
        break; }
    case DART_ERROR: {                               /* every failure funnels here; mark the sub if channel-scoped */
        char line[160];
        if (ev->error == DART_E_QOS_INCOMPATIBLE || ev->error == DART_E_SCHEMA_MISMATCH ||
            ev->error == DART_E_MSG_TOO_BIG){
            CapSub *s = cap_sub_by_index(ev->channel);
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
           "  --if    IP   multicast interface IP      (default auto; 127.0.0.1 = single-host)\n"
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

    /* a real node, owning its memory via a dynamic allocator. It starts with no channels but
       subscribes to topics on demand (cap_subscribe); CAP_OBSERVER_CHANNELS bounds those and
       sizes discovery's per-peer overlay buffer, so a peer advertising many topics is held in full. */
    mem  = dart_allocator_dynamic(i_dart_plat_realloc, 0);
    node = dart_node_open(&mem, cfg->name, cap_on_message, cap_on_event, &(DartNodeOpts){
        .domain        = cfg->domain,
        .max_channels  = CAP_OBSERVER_CHANNELS,
        .fetch_details = 1,   /* observer: fetch every peer topic's name + schema */
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
int cap_poll(Capture *cap){
    DartNode *node = (DartNode *)cap->rt;
    if (!node) return 0;
    if (!dart_node_is_started(node))
        dart_node_poll(node, 0);
    return dart_node_dispatch(node, 0, 0);
}

void cap_stop(Capture *cap){
    cap_node = NULL;
    if (cap->rt) dart_node_close((DartNode *)cap->rt, 1);   /* stops the service thread, sends a BYE, frees */
    cap->rt = NULL; cap->mem = NULL;
}

/* bring the topic's live channel in line with subscribed/publishing: make the channel for
   `want` reliability live with the combined role (SUB_ONLY/PUB_ONLY/PUBSUB), creating it once
   if needed, and turn the other one off. Data is unicast (point-to-point to each subscriber),
   so a publish reaches every subscriber and a subscribe hears every unicast publisher.
   Returns 1 on success. */
static int cap_sub_reconcile(CapSub *s, DartNode *node, int want){
    DartRole role = (s->subscribed && s->publishing) ? DART_PUBSUB
                  :  s->subscribed                   ? DART_SUB_ONLY
                  :  s->publishing                   ? DART_PUB_ONLY
                  :                                    DART_INACTIVE;
    want = want ? 1 : 0;
    if (role == DART_INACTIVE){
        if (s->ch[0]) dart_channel_set_role(s->ch[0], DART_INACTIVE);
        if (s->ch[1]) dart_channel_set_role(s->ch[1], DART_INACTIVE);
        return 1;
    }
    if (!s->ch[want]){
        /* a typed topic gets a typed channel: adopt the schema its advertisers carry, so
           our publishes reach typed readers and the schema gate matches us. Trade-off:
           this channel then matches only that schema (the drawer flags topics whose
           publishers disagree); a topic with no advertised schema stays generic. */
        DartSchema *sch = cap_topic_schema_parse(node, s->name);
        DartChannel *ch = dart_node_create_channel(node, s->name, role, sch,
                 &(DartChannelOpts){ .qos = { .reliability = want ? DART_RELIABLE : DART_BEST_EFFORT,
                                              .catch_up = 1 } });
        if (sch) dart_schema_free(sch, cap_schema_alloc, NULL);   /* the node keeps its own copy */
        if (!ch) return 0;
        /* switch it to QUEUED delivery now (lazy ring, grows to DART_QUEUE_CAP): its
           messages then arrive via cap_poll's dispatch on the UI thread, never on the
           service thread. An observer never stalls a publisher: at the cap a best-effort
           queue drops oldest, and a parked reliable one only throttles publishers that
           opted into backpressure_wait_us. */
        dart_channel_dispatch(ch, 0, 0);
        /* publish the (index, handle) pair under the node lock: the service thread's
           event callback maps events back to topics through it (cap_sub_by_index) */
        dart_node_lock(node);
        s->index[want] = dart_channel_index(ch);
        s->ch[want] = ch;
        dart_node_unlock(node);
    } else {
        dart_channel_set_role(s->ch[want], role);
    }
    if (s->ch[!want]) dart_channel_set_role(s->ch[!want], DART_INACTIVE);   /* one live at a time */
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

/* Scan the live peer interest for subscribers of `topic`, matched by its 32-bit interest
   hash (available the moment an announce arrives, no detail fetch needed). Sets *any_sub
   when some peer subscribes, and returns 1 if any of those subscribers requests reliable.
   A publisher must OFFER reliable to satisfy a RELIABLE subscriber (RxO: offered >=
   requested), so the explorer reads this to match a subscriber's QoS when it publishes,
   even with no other publisher on the network to derive reliability from. */
static int cap_topic_sub_reliable(DartNode *node, const char *topic, int *any_sub){
    const DartDiscoveryPeer *peers; uint16_t n_peers = 0, s;
    uint32_t want_hash;
    int reliable = 0, seen = 0;
    if (any_sub) *any_sub = 0;
    if (!node || !topic || !*topic) return 0;
    want_hash = (uint32_t)dart_topic_id(topic);
    dart_node_lock(node);   /* the zero-copy peer view vs the service thread */
    peers = dart_node_peers(node, &n_peers);
    for (s = 0; s < n_peers; s++){
        DartInterestIter it; DartTopic t;
        memset(&it, 0, sizeof it);
        while (dart_node_peer_interest_next(&peers[s], &it, &t)){
            if (t.is_pub || t.hash != want_hash) continue;   /* subscriber entries for this topic */
            seen = 1;
            if (t.reliable) reliable = 1;
        }
    }
    dart_node_unlock(node);
    if (any_sub) *any_sub = seen;
    return reliable;
}

int cap_declare_publish(Capture *cap, const char *topic){
    DartNode *node = cap ? (DartNode *)cap->rt : NULL;
    CapSub *s = node ? cap_sub_get(topic) : NULL;
    int was, want, any_sub = 0, sub_reliable;
    if (!node || !topic || !*topic) return 0;
    if (!s){ cap_logf("PUBLISH %s refused (topic table full)", topic); return 0; }
    was = s->publishing;
    s->publishing = 1;
    /* match the network subscribers' QoS: offer reliable if any subscriber requests it,
       else mirror their best-effort. With no subscriber yet, reliable is the safe default
       (it satisfies whoever shows up, and the next send re-reconciles). When we also
       subscribe, one identity means one live channel, so our own reliable subscribe forces
       reliable too. */
    sub_reliable = cap_topic_sub_reliable(node, topic, &any_sub);
    want = (s->subscribed && s->reliable) || sub_reliable || !any_sub;
    if (!cap_sub_reconcile(s, node, want)){ s->publishing = was; cap_logf("PUBLISH %s failed (channel)", topic); return 0; }
    if (!was) cap_logf("PUBLISH %s declared (%s pub interest)", topic, want ? "reliable" : "best-effort");
    return 1;
}

int cap_publish(Capture *cap, const char *topic, const void *data, size_t len){
    CapSub *s; const DartSchema *echo;
    if (!topic || (!data && len)) return 0;
    if (!cap_declare_publish(cap, topic)) return 0;   /* ensure the publisher channel is live */
    s = cap_sub_find(topic);
    if (!s) return 0;
    if (dart_channel_send(s->ch[s->reliable], dart_bytes(data, len)) < 0){ cap_logf("PUBLISH %s send failed", topic); return 0; }
    echo = s->ch[s->reliable] ? dart_channel_schema(s->ch[s->reliable]) : NULL;   /* decoded echo when typed */
    if (echo && !dart_schema_validate(echo, dart_bytes(data, len))) echo = NULL;
    cap_ring_push(s, dart_cstr(cap_cfg.name), data, len, 1, echo);
    return 1;
}

/* parse one form value into flat field i of the message being built (dart_set_value).
   Empty keeps the default; a struct row has no value of its own (its members do). */
static int cap_form_set(const DartSchema *sch, uint16_t i, const char *v, uint8_t *buf, size_t size){
    DartSchemaFieldInfo fi; DartValue val; char *end;
    if (!dart_schema_field_at(sch, i, &fi)) return 0;
    while (*v == ' ' || *v == '\t') v++;
    if (!*v || fi.kind == DART_STRUCT) return 1;          /* empty/struct: the default stays */
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
        case DART_STR:                                    /* the form text IS the content */
            val.bytes = dart_bytes(v, strlen(v));
            break;
        case DART_ARR: {                                  /* comma/space-separated elements */
            uint32_t esz = (fi.elem == DART_STR) ? 2u + fi.str_cap
                                                 : dart_schema_scalar_size((DartSchemaTypeKind)fi.elem);
            uint8_t *w = (uint8_t *)malloc(fi.size ? fi.size : 1);
            uint16_t n = 0; const char *p = v; int ok;
            if (!w || !esz){ free(w); return 0; }
            if (fi.elem == DART_STR){                     /* comma-separated strings */
                while (*p){
                    const char *q; size_t l;
                    while (*p == ' ' || *p == '\t') p++;
                    q = p; while (*q && *q != ',') q++;
                    l = (size_t)(q - p);
                    while (l && (p[l-1] == ' ' || p[l-1] == '\t')) l--;
                    if (n >= fi.count || l > fi.str_cap){ free(w); return 0; }
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
                if (!*p || n >= fi.count) break;
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

int cap_publish_form(Capture *cap, const char *topic, const char *const *values, int n_values){
    CapSub *s; const DartSchema *sch; uint8_t *buf; size_t size; uint16_t i, nf; int ok = 1;
    if (!topic || !values) return 0;
    if (!cap_declare_publish(cap, topic)) return 0;
    s = cap_sub_find(topic);
    sch = (s && s->ch[s->reliable]) ? dart_channel_schema(s->ch[s->reliable]) : NULL;
    if (!sch){ cap_logf("PUBLISH %s refused: channel carries no schema", topic); return 0; }
    size = dart_schema_size(sch);
    buf = (uint8_t *)malloc(size ? size : 1);
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
    if (dart_channel_send(s->ch[s->reliable], dart_bytes(buf, size)) < 0){
        cap_logf("PUBLISH %s send failed", topic);
        free(buf);
        return 0;
    }
    cap_ring_push(s, dart_cstr(cap_cfg.name), buf, size, 1, sch);   /* decoded local echo */
    free(buf);
    return 1;
}

/* copy one peer's facts out of the node's zero-copy view into its observer record:
   discovery-level name/addr + liveness, then the announce overlay decoded via the node's
   helpers (so we never touch dart_meta_*). The interest list is walked in full; we keep up
   to CAP_MAX_TOPICS of each (the observer's own storage bound, not an API limit). */
static void cap_peer_refresh(DartNode *node, CapPeer *p, const DartDiscoveryPeer *dp){
    DartInterestIter it; DartTopic t;
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
       which is O(topics) per peer PER FRAME -- crippling at many-thousand-topic scale. The list
       only changes when the peer re-advertises (meta_version bumps) or while its names are still
       paging in from the detail exchange (names_pending), so skip the walk otherwise and keep the
       cached lists. cap_snapshot still copies them out each frame; only this fetch is throttled. */
    if (dp->meta_version == p->topics_ver && !p->names_pending) return;

    p->n_pub = p->n_sub = 0;
    p->names_pending = 0;
    memset(&it, 0, sizeof it);
    while (dart_node_peer_interest_next(dp, &it, &t)){
        CapTopic *e; int *cnt;
        DartString nm = dart_node_peer_topic_name(node, dp->id, t.alias);
        if (t.is_pub){ if (p->n_pub >= CAP_MAX_TOPICS) continue; e = &p->pub[p->n_pub]; cnt = &p->n_pub; }
        else         { if (p->n_sub >= CAP_MAX_TOPICS) continue; e = &p->sub[p->n_sub]; cnt = &p->n_sub; }
        /* the fetched name (the announce carries only hashes; fetch_details fills the
           cache within an RTT), the hash as a placeholder until it lands */
        if (nm.data) snprintf(e->name, sizeof e->name, "%.*s", (int)nm.len, nm.data);
        else       { snprintf(e->name, sizeof e->name, "0x%08x", (unsigned)t.hash); p->names_pending = 1; }
        e->alias = t.alias; e->reliable = t.reliable;
        (*cnt)++;
    }
    p->topics_ver = dp->meta_version;   /* cache is now in sync with this announce version */
}

void cap_snapshot(const Capture *cap, CapSnapshot *out){
    DartNode *node = (DartNode *)cap->rt;
    unsigned long long now = cap_now_ms();
    const DartDiscoveryPeer *peers;
    uint16_t slot, n_peers = 0;
    int i, k;

    /* Do NOT memset the whole snapshot: at CAP_MAX_EP it is ~150 MB, and zeroing it every
       frame is the dominant per-frame cost. It is fully count-delimited instead: n_nodes /
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

        for (k = 0; k < p->n_pub && k < CAP_MAX_EP; k++){   /* memcpy, not snprintf: the name is
                                                               already a NUL-terminated 65-byte field,
                                                               and per-entry snprintf x thousands is slow */
            memcpy(n->pub[k].name, p->pub[k].name, sizeof n->pub[k].name);
            n->pub[k].alias    = p->pub[k].alias;
            n->pub[k].reliable = p->pub[k].reliable;
        }
        n->n_pub = k;
        for (k = 0; k < p->n_sub && k < CAP_MAX_EP; k++){
            memcpy(n->sub[k].name, p->sub[k].name, sizeof n->sub[k].name);
            n->sub[k].alias    = p->sub[k].alias;
            n->sub[k].reliable = p->sub[k].reliable;
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
        case DART_STR: return "string";
        case DART_STRUCT: return "struct";
        default: return "?";
    }
}

/* fill out->fields from a parsed schema (top-level fields, capped to the UI's bound) */
static void cap_schema_fields(CapSchema *out, const DartSchema *sch){
    DartSchemaFieldInfo fi; uint16_t i, nf = dart_schema_field_count(sch);
    DartString tn = dart_schema_name(sch);
    out->inlined      = 1;
    out->msg_size     = dart_schema_size(sch);
    out->total_fields = nf;
    snprintf(out->type_name, sizeof out->type_name, "%.*s", (int)tn.len, tn.data ? tn.data : "");
    for (i = 0; i < nf && out->n_fields < CAP_SCHEMA_FIELDS; i++){
        CapSchemaField *f = &out->fields[out->n_fields];
        if (!dart_schema_field_at(sch, i, &fi)) break;
        snprintf(f->name, sizeof f->name, "%.*s", (int)fi.name.len, fi.name.data ? fi.name.data : "");
        if (fi.kind == DART_STR)
            snprintf(f->type, sizeof f->type, "string<%u>", fi.str_cap);
        else if (fi.kind == DART_ARR && fi.elem == DART_STR)
            snprintf(f->type, sizeof f->type, "string<%u>[%u]", fi.str_cap, fi.count);
        else if (fi.kind == DART_ARR)
            snprintf(f->type, sizeof f->type, "%s[%u]", cap_kind_str(fi.elem), fi.count);
        else
            snprintf(f->type, sizeof f->type, "%s", cap_kind_str(fi.kind));
        f->kind    = fi.kind;                                        /* CAP_K_* == DartSchemaTypeKind */
        f->elem    = (uint8_t)(fi.kind == DART_ARR ? fi.elem : 0);
        f->count   = (uint16_t)(fi.kind == DART_ARR ? fi.count : 0);
        f->str_cap = fi.str_cap;                                     /* set for STR + STR-element arrays */
        f->depth  = (uint8_t)(fi.depth > 255 ? 255 : fi.depth);
        f->offset = fi.offset;
        f->size   = fi.size;
        out->n_fields++;
    }
}

/* the topic's advertised schema, from the node's greedy detail cache (fetch_details):
   walk peers x interest for an entry whose fetched name equals the topic and read the
   cached parsed schema. The caller frees the returned copy via cap_schema_alloc (the
   cached one is node-owned, so hand back a reparse of its canonical wire). NULL when
   nobody advertises one (or details are still in flight). */
static DartSchema *cap_topic_schema_parse(DartNode *node, const char *topic){
    const DartDiscoveryPeer *peers; uint16_t n_peers = 0, s;
    size_t tlen = topic ? strlen(topic) : 0;
    uint32_t want_hash;
    DartSchema *found = NULL;
    if (!node || tlen == 0) return NULL;
    want_hash = (uint32_t)dart_topic_id(topic);
    dart_node_lock(node);   /* the zero-copy peer view vs the service thread */
    peers = dart_node_peers(node, &n_peers);
    for (s = 0; s < n_peers && !found; s++){
        DartInterestIter it; DartTopic t;
        memset(&it, 0, sizeof it);
        while (dart_node_peer_interest_next(&peers[s], &it, &t)){
            DartString nm;
            const DartSchema *sch;
            if (t.hash != want_hash) continue;   /* cheap prefilter: the name lookup below is O(topics) */
            nm = dart_node_peer_topic_name(node, peers[s].id, t.alias);
            if (nm.len != tlen || memcmp(nm.data, topic, tlen) != 0) continue;   /* confirm vs a hash collision */
            sch = dart_node_peer_topic_schema(node, peers[s].id, t.alias, NULL);
            if (!sch) continue;
            {   DartBytes wire = dart_schema_wire(sch);
                found = dart_schema_parse(wire.data, wire.len, cap_schema_alloc, NULL);
                break;
            }
        }
    }
    dart_node_unlock(node);
    return found;
}

int cap_topic_schema(const Capture *cap, const char *topic, CapSchema *out){
    DartNode *node = cap ? (DartNode *)cap->rt : NULL;
    const DartDiscoveryPeer *peers; uint16_t n_peers = 0, s;
    size_t tlen = topic ? strlen(topic) : 0;
    uint32_t want_hash;
    const DartSchema *best = NULL;   /* parsed wire schema of the shown hash (NULL = hash-only) */
    int best_is_pub = 0, found = 0;
    memset(out, 0, sizeof *out);
    if (!node || tlen == 0) return 0;
    want_hash = (uint32_t)dart_topic_id(topic);
    dart_node_lock(node);   /* the zero-copy peer view vs the service thread */
    peers = dart_node_peers(node, &n_peers);
    for (s = 0; s < n_peers; s++){
        const DartDiscoveryPeer *dp = &peers[s];
        DartInterestIter it; DartTopic t;
        uint16_t last_alias = 0xFFFF;   /* PUBSUB yields both directions: one schema per alias */
        memset(&it, 0, sizeof it);
        while (dart_node_peer_interest_next(dp, &it, &t)){
            DartString nm; uint64_t hash = 0; const DartSchema *sch;
            int take = 0;
            if (t.hash != want_hash) continue;          /* cheap prefilter before the O(topics) name lookup */
            if (t.alias == last_alias) continue;        /* second direction of a PUBSUB entry */
            last_alias = t.alias;
            /* any endpoint (publisher OR subscriber) is authoritative about its schema:
               a subscriber-in-charge topic advertises the shape its generic publisher fills */
            nm = dart_node_peer_topic_name(node, dp->id, t.alias);
            if (nm.len != tlen || memcmp(nm.data, topic, tlen) != 0) continue;
            sch = dart_node_peer_topic_schema(node, dp->id, t.alias, &hash);
            if (!hash) continue;                        /* untyped endpoint */
            if (!found){                                /* first advertiser: adopt it */
                found = 1; out->n_advertisers = 1; take = 1;
            } else if (hash == out->hash){              /* identical schema: agree */
                out->n_advertisers++;
                /* upgrade the shown source to a publisher (owns the wire) or, if we only
                   had the hash before, to the parsed form so the field list can render */
                take = (t.is_pub && !best_is_pub) || (sch && !best);
            } else if (best && sch){
                /* DIFFERENT hash but structurally compatible is NOT a conflict: subset
                   binding lets a narrower reader consume a wider writer (dart_schema_subset,
                   the same gate the C matcher runs). Show the WIDER schema, preferring a
                   publisher since it owns the actual wire bytes. */
                int best_narrower = dart_schema_subset(best, sch);   /* best subset of sch: sch is wider */
                int new_narrower  = dart_schema_subset(sch, best);   /* sch subset of best: best is wider */
                if (!best_narrower && !new_narrower){   /* neither reads the other: real conflict */
                    out->hash_conflict = 1;
                    continue;
                }
                out->n_advertisers++;
                take = (t.is_pub && !best_is_pub)                    /* publisher wins the wire */
                    || (t.is_pub == best_is_pub && best_narrower);   /* same role: the wider wins */
            } else {                                    /* one side hash-only: cannot prove subset */
                out->hash_conflict = 1;
                continue;
            }
            if (take){
                best = sch; best_is_pub = t.is_pub; out->hash = hash;
                snprintf(out->from, sizeof out->from, "%.*s", (int)dp->name.len,
                         dp->name.data ? dp->name.data : "");
            }
        }
    }
    if (best) cap_schema_fields(out, best);             /* cached parsed schema: field list */
    dart_node_unlock(node);
    return found;
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
    int j;
    for (j = 0; j < n_values; j++) if (valid) valid[j] = 1;   /* default: don't flag */
    if (!node || !topic || !values) return 0;
    sch = cap_topic_schema_parse(node, topic);
    if (!sch) return 0;                                       /* no schema: nothing to judge against */
    size = dart_schema_size(sch);
    buf = (uint8_t *)malloc(size ? size : 1);
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
