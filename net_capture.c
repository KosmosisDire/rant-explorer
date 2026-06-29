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
#define CAP_OBSERVER_CHANNELS 32   /* our (empty) channel reserve: sizes the incoming overlay buffer */
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

    /* a real node, owning its memory via a dynamic allocator. It creates no channels (the
       observer publishes/subscribes nothing); CAP_OBSERVER_CHANNELS only sizes discovery's
       per-peer incoming overlay buffer, so a peer advertising many topics is held in full. */
    mem  = dart_allocator_dynamic(1 << 20);
    node = dart_node_open(&mem, cfg->name, NULL, cap_on_event, &(DartNodeOpts){
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

/* copy one peer's facts out of the node's zero-copy view into its observer record:
   discovery-level name/addr + liveness, then the announce overlay decoded via the node's
   helpers (so we never touch dart_meta_*). The interest list is walked in full; we keep up
   to CAP_MAX_TOPICS of each (the observer's own storage bound, not an API limit). */
static void cap_peer_refresh(CapPeer *p, const DartDiscoveryPeer *dp){
    DartInterestIter it; DartTopic t;
    uint16_t frag = dart_node_peer_frag(dp);
    p->seen_frame = 1;
    p->state      = (dp->liveness == DART_PEER_DROPPED) ? CAP_DROPPED : CAP_ACTIVE;
    p->have_meta  = (dp->meta && dp->meta_len) ? 1 : 0;
    p->frag       = frag;
    p->meta_len   = dp->meta_len;
    memcpy(p->ip, dp->addr.ip, 16);
    p->ip_len = dp->addr.ip_len;
    p->port   = dp->addr.port;
    snprintf(p->name, sizeof p->name, "%s", dp->name ? dp->name : "");

    p->n_pub = p->n_sub = 0;
    memset(&it, 0, sizeof it);
    while (dart_node_peer_interest_next(dp, &it, &t)){
        CapTopic *e; int *cnt;
        uint8_t c = t.name_len > DART_TOPIC_NAME_MAX ? (uint8_t)DART_TOPIC_NAME_MAX : t.name_len;
        if (t.is_pub){ if (p->n_pub >= CAP_MAX_TOPICS) continue; e = &p->pub[p->n_pub]; cnt = &p->n_pub; }
        else         { if (p->n_sub >= CAP_MAX_TOPICS) continue; e = &p->sub[p->n_sub]; cnt = &p->n_sub; }
        memcpy(e->name, t.name, c); e->name[c] = '\0';
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
