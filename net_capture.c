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
#define CAP_MAX_TOPICS        32
#define CAP_OBSERVER_CHANNELS 64   /* node channel reserve: sizes the incoming overlay buffer and covers
                                      our own subscriptions (up to 2 channels per topic, see CAP_MAX_SUBS) */
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

static unsigned long long cap_now_ms(void);   /* defined below; used by cap_ring_push */

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
    uint32_t len;
    uint16_t preview_len;
    int      mine;                        /* 1 = we published it (local echo) */
    int      decoded;                     /* 1 = fields[] holds the reflected decode */
    int      n_fields, total_fields;
    char     type_name[DART_TOPIC_NAME_MAX + 1];   /* sender's schema root name when decoded */
    char     sender[DART_NODE_NAME_MAX + 1];
    char     preview[CAP_MSG_PREVIEW];    /* raw payload head (schema-less messages) */
    CapMsgField fields[CAP_MSG_FIELDS];   /* per-field rendered decode */
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
    CapMsgRec    ring[CAP_FEED_MAX];
    int          head, count;            /* ring write cursor + fill */
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
    for (i = 0; i < CAP_MAX_SUBS; i++) if (!cap_subs[i].used){
        s = &cap_subs[i]; memset(s, 0, sizeof *s); s->used = 1;
        snprintf(s->name, sizeof s->name, "%s", name); return s;
    }
    return NULL;
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
/* an array value: the first few elements decoded by their kind */
static void cap_fmt_arr(char *dst, int cap, DartBytes a, uint8_t elem, uint16_t count){
    uint32_t esz = dart_schema_scalar_size((DartSchemaTypeKind)elem);
    uint16_t j, show = count < 6 ? count : 6;
    int at = 0;
    dst[0] = '\0';
    at = cap_val_append(dst, cap, at, "[");
    for (j = 0; j < show && esz; j++){
        const uint8_t *p = a.data + (size_t)j * esz;
        if (j) at = cap_val_append(dst, cap, at, " ");
        switch ((DartSchemaTypeKind)elem){
            case DART_U8:  at = cap_val_append(dst,cap,at,"%u",  p[0]); break;
            case DART_U16: at = cap_val_append(dst,cap,at,"%u",  i_dart_le_r16(p)); break;
            case DART_U32: at = cap_val_append(dst,cap,at,"%lu", (unsigned long)i_dart_le_r32(p)); break;
            case DART_U64: at = cap_val_append(dst,cap,at,"%llu",(unsigned long long)i_dart_le_r64(p)); break;
            case DART_I8:  at = cap_val_append(dst,cap,at,"%d",  (int)(int8_t)p[0]); break;
            case DART_I16: at = cap_val_append(dst,cap,at,"%d",  (int)(int16_t)i_dart_le_r16(p)); break;
            case DART_I32: at = cap_val_append(dst,cap,at,"%ld", (long)(int32_t)i_dart_le_r32(p)); break;
            case DART_I64: at = cap_val_append(dst,cap,at,"%lld",(long long)(int64_t)i_dart_le_r64(p)); break;
            case DART_F32: { uint32_t b = i_dart_le_r32(p); float  v; memcpy(&v,&b,4);
                             at = cap_val_append(dst,cap,at,"%g",(double)v); } break;
            case DART_F64: { uint64_t b = i_dart_le_r64(p); double v; memcpy(&v,&b,8);
                             at = cap_val_append(dst,cap,at,"%g",v); } break;
            case DART_BOOL: at = cap_val_append(dst,cap,at,"%s", p[0] ? "true" : "false"); break;
            default: break;
        }
    }
    if (show < count) at = cap_val_append(dst, cap, at, " ..");
    cap_val_append(dst, cap, at, "]");
}
static void cap_decode_fields(CapMsgRec *m, DartBytes data, const DartSchema *s){
    uint16_t i, nf = dart_schema_field_count(s);
    m->n_fields = 0;
    m->total_fields = nf;
    for (i = 0; i < nf && m->n_fields < CAP_MSG_FIELDS; i++){
        DartSchemaFieldInfo fi; CapMsgField *f;
        if (!dart_schema_field_at(s, i, &fi)) break;
        f = &m->fields[m->n_fields++];
        snprintf(f->name, sizeof f->name, "%.*s", (int)fi.name.len, fi.name.data ? fi.name.data : "");
        switch ((DartSchemaTypeKind)fi.kind){
            case DART_BOOL: snprintf(f->value, sizeof f->value, "%s", dart_get_uint(data,s,i) ? "true" : "false"); break;
            case DART_U8: case DART_U16: case DART_U32: case DART_U64:
                snprintf(f->value, sizeof f->value, "%llu", (unsigned long long)dart_get_uint(data,s,i)); break;
            case DART_I8: case DART_I16: case DART_I32: case DART_I64:
                snprintf(f->value, sizeof f->value, "%lld", (long long)dart_get_int(data,s,i)); break;
            case DART_F32: snprintf(f->value, sizeof f->value, "%g", (double)dart_get_f32(data,s,i)); break;
            case DART_F64: snprintf(f->value, sizeof f->value, "%g", dart_get_f64(data,s,i)); break;
            case DART_ARR: cap_fmt_arr(f->value, sizeof f->value, dart_get_array(data,s,i), fi.elem, fi.count); break;
            case DART_STRUCT: snprintf(f->value, sizeof f->value, "{..}"); break;
            default:          snprintf(f->value, sizeof f->value, "?"); break;
        }
    }
}

/* append one message to a topic's ring (received or our own echo). Does not touch
   n_msgs. With a schema the message is reflected into field rows; raw bytes else. */
static void cap_ring_push(CapSub *s, DartString sender, const void *data, size_t len, int mine,
                          const DartSchema *schema){
    CapMsgRec *m = &s->ring[s->head];
    m->t_s = (double)(cap_now_ms() - cap_start_ms) / 1000.0;
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

static void cap_logf(const char *fmt, ...){
    unsigned long long t = cap_now_ms() - cap_start_ms;
    char *line = cap_log[cap_log_head];
    int n = snprintf(line, CAP_LOG_LINE, "[%6.2fs] ", (double)t / 1000.0);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, (size_t)(CAP_LOG_LINE - n), fmt, ap);
    va_end(ap);
    cap_log_head = (cap_log_head + 1) % CAP_LOG_LINES;
    if (cap_log_count < CAP_LOG_LINES) cap_log_count++;
    fputs(line, stdout); fputc('\n', stdout);
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

/* the node's message sink: append a delivered message to its topic ring. Single threaded
   with cap_poll, so no locking. We keep only a short preview of the payload. */
static void cap_on_message(const DartMsg *msg){
    CapSub *s = cap_sub_by_index(msg->channel_id);
    if (!s) return;
    cap_ring_push(s, msg->sender_name, msg->data.data, msg->data.len, 0, msg->schema);
    s->n_msgs++;
}

/* the node's app event sink: maintain the observer metrics + log. We never call back into
   dart_* here (the snapshot reads the facts); the cap_peers table is file-static. */
static void cap_on_event(const DartEvent *ev){
    switch (ev->kind){
    case DART_PEER_UP: {
        CapPeer *p = cap_peer_get(ev->peer);
        p->last_change_ms = cap_now_ms();
        if (ev->ip_len == 4)
            cap_logf("UP    id=%u  %u.%u.%u.%u:%u  (%s)", ev->peer,
                     ev->ip[0], ev->ip[1], ev->ip[2], ev->ip[3], ev->port, ev->detail ? ev->detail : "");
        else
            cap_logf("UP    id=%u  (%s)", ev->peer, ev->detail ? ev->detail : "");
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
        cap_logf("DOWN  id=%u  (%s)", ev->peer, ev->detail ? ev->detail : "");
        break; }
    case DART_PEER_REFUSED:
        if (ev->ip_len == 4)
            cap_logf("REFUSED %u.%u.%u.%u:%u  (peer table full of active peers)",
                     ev->ip[0], ev->ip[1], ev->ip[2], ev->ip[3], ev->port);
        else
            cap_logf("REFUSED  (peer table full of active peers)");
        break;
    case DART_MSG_LOST: {                            /* reliable subscriber skipped past a gap */
        CapSub *s = cap_sub_by_index(ev->channel);
        if (s) s->n_drops += ev->lost_count;
        cap_logf("        LOST  ch=%u peer=%u first=%llu count=%llu", ev->channel, ev->peer,
                 (unsigned long long)ev->lost_first, (unsigned long long)ev->lost_count);
        break; }
    case DART_QOS_INCOMPATIBLE: {                    /* our reliable sub refused a best-effort pub */
        CapSub *s = cap_sub_by_index(ev->channel);
        if (s) s->error = 1;
        cap_logf("        QOS_INCOMPATIBLE  ch=%u peer=%u (%s)", ev->channel, ev->peer,
                 ev->detail ? ev->detail : "");
        break; }
    case DART_MSG_TOO_BIG: {
        CapSub *s = cap_sub_by_index(ev->channel);
        if (s) s->error = 1;
        cap_logf("        MSG_TOO_BIG  ch=%u peer=%u bytes=%llu", ev->channel, ev->peer,
                 (unsigned long long)ev->too_big_bytes);
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
        .domain       = cfg->domain,
        .max_channels = CAP_OBSERVER_CHANNELS,
        .net          = { .discovery_group     = cfg->group,
                          .discovery_port      = cfg->port,
                          .multicast_interface = cfg->ifc },
        .discovery    = { .max_peers = CAP_MAX_PEERS },
    });
    if (!node){
        fprintf(stderr, "cap_start: dart_node_open failed (port %u in use? interface?)\n", cfg->port);
        return 0;
    }
    cap->rt  = node;
    cap->mem = NULL;         /* the node owns its memory now; close frees it */
    cap_logf("observer node \"%s\" started on domain %u (%s:%u)", cfg->name, cfg->domain, cfg->group, cfg->port);
    return 1;
}

int cap_poll(Capture *cap){
    if (!cap->rt) return 0;
    return dart_node_poll((DartNode *)cap->rt, 0);   /* one tick: drains RX, pumps discovery + timers */
}

void cap_stop(Capture *cap){
    if (cap->rt) dart_node_close((DartNode *)cap->rt, 1);   /* sends a BYE, frees the node's memory */
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
        DartChannel *ch = dart_node_create_channel(node, s->name, role, NULL,
                 &(DartChannelOpts){ .qos = { .reliability = want ? DART_RELIABLE : DART_BEST_EFFORT,
                                              .catch_up = 1 } });
        if (!ch) return 0;
        s->ch[want] = ch; s->index[want] = dart_channel_index(ch);
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

int cap_declare_publish(Capture *cap, const char *topic){
    DartNode *node = cap ? (DartNode *)cap->rt : NULL;
    CapSub *s = node ? cap_sub_get(topic) : NULL;
    int was, want;
    if (!node || !topic || !*topic) return 0;
    if (!s){ cap_logf("PUBLISH %s refused (topic table full)", topic); return 0; }
    was = s->publishing;
    s->publishing = 1;
    /* when also subscribed we must reuse the live channel (one identity, one live channel),
       so the publish rides its reliability; pub-only goes reliable to reach the most subs. */
    want = s->subscribed ? s->reliable : 1;
    if (!cap_sub_reconcile(s, node, want)){ s->publishing = was; cap_logf("PUBLISH %s failed (channel)", topic); return 0; }
    if (!was) cap_logf("PUBLISH %s declared (pub interest)", topic);
    return 1;
}

int cap_publish(Capture *cap, const char *topic, const void *data, size_t len){
    CapSub *s;
    if (!topic || (!data && len)) return 0;
    if (!cap_declare_publish(cap, topic)) return 0;   /* ensure the publisher channel is live */
    s = cap_sub_find(topic);
    if (!s) return 0;
    if (dart_channel_send(s->ch[s->reliable], dart_bytes(data, len)) < 0){ cap_logf("PUBLISH %s send failed", topic); return 0; }
    cap_ring_push(s, dart_cstr(cap_cfg.name), data, len, 1, NULL);   /* local echo: raw (our channels are schema-less) */
    return 1;
}

/* copy one peer's facts out of the node's zero-copy view into its observer record:
   discovery-level name/addr + liveness, then the announce overlay decoded via the node's
   helpers (so we never touch dart_meta_*). The interest list is walked in full; we keep up
   to CAP_MAX_TOPICS of each (the observer's own storage bound, not an API limit). */
static void cap_peer_refresh(CapPeer *p, const DartDiscoveryPeer *dp){
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

    p->n_pub = p->n_sub = 0;
    memset(&it, 0, sizeof it);
    while (dart_node_peer_interest_next(dp, &it, &t)){
        CapTopic *e; int *cnt;
        uint8_t c = t.name.len > DART_TOPIC_NAME_MAX ? (uint8_t)DART_TOPIC_NAME_MAX : (uint8_t)t.name.len;
        if (t.is_pub){ if (p->n_pub >= CAP_MAX_TOPICS) continue; e = &p->pub[p->n_pub]; cnt = &p->n_pub; }
        else         { if (p->n_sub >= CAP_MAX_TOPICS) continue; e = &p->sub[p->n_sub]; cnt = &p->n_sub; }
        memcpy(e->name, t.name.data, c); e->name[c] = '\0';
        e->alias = t.alias; e->reliable = t.reliable;
        (*cnt)++;
    }
}

void cap_snapshot(const Capture *cap, CapSnapshot *out){
    DartNode *node = (DartNode *)cap->rt;
    unsigned long long now = cap_now_ms();
    const DartDiscoveryPeer *peers;
    uint16_t slot, n_peers = 0;
    int i, k;

    memset(out, 0, sizeof *out);
    out->domain    = cap_cfg.domain;
    out->disc_port = cap_cfg.port;
    snprintf(out->group, sizeof out->group, "%s", cap_cfg.group ? cap_cfg.group : "");

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
        const CapSub *s = &cap_subs[i];
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
    }

    /* pull the node's zero-copy peer view (it decodes the overlay for us) and refresh each
       observer record; peers no longer in the node's table fold to GONE, kept de-emphasized. */
    for (i = 0; i < CAP_MAX_PEERS; i++) cap_peers[i].seen_frame = 0;
    peers = dart_node_peers(node, &n_peers);
    for (slot = 0; slot < n_peers; slot++)
        cap_peer_refresh(cap_peer_get(peers[slot].id), &peers[slot]);
    for (i = 0; i < CAP_MAX_PEERS; i++){
        CapPeer *p = &cap_peers[i];
        if (p->used && !p->seen_frame && p->state != CAP_GONE){   /* evicted from the node table */
            p->state = CAP_GONE;
            if (!p->down_ms) p->down_ms = now;
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

        for (k = 0; k < p->n_pub && k < CAP_MAX_EP; k++){
            snprintf(n->pub[k].name, sizeof n->pub[k].name, "%s", p->pub[k].name);
            n->pub[k].alias    = p->pub[k].alias;
            n->pub[k].reliable = p->pub[k].reliable;
        }
        n->n_pub = k;
        for (k = 0; k < p->n_sub && k < CAP_MAX_EP; k++){
            snprintf(n->sub[k].name, sizeof n->sub[k].name, "%s", p->sub[k].name);
            n->sub[k].alias    = p->sub[k].alias;
            n->sub[k].reliable = p->sub[k].reliable;
        }
        n->n_sub = k;
    }
}

int cap_topic_feed(const Capture *cap, const char *topic, CapFeedItem *out, int max,
                   int *subscribed, int *error, int *reliable, uint32_t *n_msgs, uint32_t *n_drops){
    CapSub *s = (cap && cap->rt && topic) ? cap_sub_find(topic) : NULL;
    int i, n = 0, start;
    if (subscribed) *subscribed = s ? s->subscribed : 0;
    if (error)      *error      = s ? ((s->error || s->n_drops > 0) ? 1 : 0) : 0;
    if (reliable)   *reliable   = s ? s->reliable : 0;
    if (n_msgs)     *n_msgs     = s ? (uint32_t)s->n_msgs : 0;
    if (n_drops)    *n_drops    = s ? (uint32_t)s->n_drops : 0;
    if (!s || !out || max <= 0) return 0;

    /* walk the ring oldest first; with a full ring the oldest is at head */
    n     = s->count < max ? s->count : max;
    start = (s->head - n) % CAP_FEED_MAX; if (start < 0) start += CAP_FEED_MAX;
    for (i = 0; i < n; i++){
        const CapMsgRec *m = &s->ring[(start + i) % CAP_FEED_MAX];
        CapFeedItem *o = &out[i];
        int k;
        o->t_s         = m->t_s;
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
    }
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
        if (fi.kind == DART_ARR) snprintf(f->type, sizeof f->type, "%s[%u]", cap_kind_str(fi.elem), fi.count);
        else                     snprintf(f->type, sizeof f->type, "%s", cap_kind_str(fi.kind));
        f->offset = fi.offset;
        f->size   = fi.size;
        out->n_fields++;
    }
}

int cap_topic_schema(const Capture *cap, const char *topic, CapSchema *out){
    DartNode *node = cap ? (DartNode *)cap->rt : NULL;
    const DartDiscoveryPeer *peers; uint16_t n_peers = 0, s;
    size_t tlen = topic ? strlen(topic) : 0;
    int found = 0;
    memset(out, 0, sizeof *out);
    if (!node || tlen == 0) return 0;
    peers = dart_node_peers(node, &n_peers);
    for (s = 0; s < n_peers; s++){
        const DartDiscoveryPeer *dp = &peers[s];
        DartInterestIter it; DartTopic t;
        memset(&it, 0, sizeof it);
        while (dart_node_peer_interest_next(dp, &it, &t)){
            uint64_t hash; DartBytes wire;
            if (!t.is_pub || t.name.len != tlen || memcmp(t.name.data, topic, tlen) != 0) continue;
            if (!dart_node_peer_schema(dp, t.alias, &hash, &wire)) continue;
            if (found){   /* another publisher: agreement check only */
                if (hash == out->hash) out->n_pubs_hash++;
                else out->hash_conflict = 1;
                continue;
            }
            found = 1;
            out->hash = hash;
            out->n_pubs_hash = 1;
            snprintf(out->from, sizeof out->from, "%.*s", (int)dp->name.len, dp->name.data ? dp->name.data : "");
            if (wire.data && wire.len){   /* inlined: decode the wire for the field list */
                DartSchema *sch = dart_schema_parse(wire.data, wire.len, cap_schema_alloc, NULL);
                if (sch){
                    cap_schema_fields(out, sch);
                    dart_schema_free(sch, cap_schema_alloc, NULL);
                }
            }
        }
    }
    return found;
}
