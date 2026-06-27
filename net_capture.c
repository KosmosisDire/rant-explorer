/* Live DART discovery observer: a bare discovery runtime (no transport, no
   channels) that decodes every peer's announce blob with the transport core's
   dart_meta_* codec. Compiled as its own TU (see net_capture.h for why) and owns
   the DART implementation. For now it runs and echoes events to the console; a
   later step exposes a peer snapshot to the UI. */

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

#define CAP_MAX_PEERS   64
#define CAP_MAX_TOPICS  32
#define CAP_RAW_META    256
#define CAP_LOG_LINES   400
/* CAP_LOG_LINE comes from net_capture.h (shared with the snapshot) */

typedef enum { CAP_ACTIVE = 0, CAP_DROPPED = 1, CAP_GONE = 2 } CapPeerState;

typedef struct {
    char     name[DART_TOPIC_NAME_MAX + 1];
    uint16_t alias;
    int      reliable;       /* offered (pub) / requested (sub): flags bit 0 */
} CapTopic;

typedef struct {
    int          used;
    uint32_t     local_id;
    CapPeerState state;

    uint8_t  ip[16];
    uint8_t  ip_len;
    uint16_t port;

    int      have_meta;
    int      meta_version;
    uint16_t frag;
    char     name[DART_NODE_NAME_MAX + 1];

    int      n_pub, n_sub;
    CapTopic pub[CAP_MAX_TOPICS];
    CapTopic sub[CAP_MAX_TOPICS];

    uint8_t  raw[CAP_RAW_META];
    uint16_t raw_len;

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

static uint8_t  cap_self_meta[16];   /* the overlay: 'D','N',ver, frag(2), npub(2), nsub(2) (no name) */
static uint16_t cap_self_meta_len;

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

static void cap_fmt_addr(char *buf, size_t n, const uint8_t *ip, uint8_t ip_len, uint16_t port){
    if (ip_len == 4)
        snprintf(buf, n, "%u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3], port);
    else if (ip_len == 16)
        snprintf(buf, n, "[%02x%02x:%02x%02x:..:%02x%02x]:%u",
                 ip[0], ip[1], ip[2], ip[3], ip[14], ip[15], port);
    else
        snprintf(buf, n, "(no address):%u", port);
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

static void cap_decode_meta(CapPeer *p, const uint8_t *meta, uint16_t meta_len){
    DartInterestIter it; DartTopic t;
    uint16_t rc;

    p->have_meta = 0;
    p->frag = 0; p->meta_version = 0; p->raw_len = 0;
    p->n_pub = p->n_sub = 0;
    if (!meta || meta_len < 3) return;        /* meta = the overlay (frag + interest); name is separate */

    p->have_meta = 1;
    p->meta_version = meta[2];
    p->frag = dart_meta_frag(meta, meta_len);

    /* decode the interest list via the transport codec's public iterator (no hand-parsing) */
    memset(&it, 0, sizeof it);
    while (dart_meta_interest_next(meta, meta_len, &it, &t)){
        CapTopic *arr; int idx;
        if (t.is_pub){ if (p->n_pub >= CAP_MAX_TOPICS) continue; arr = p->pub; idx = p->n_pub++; }
        else         { if (p->n_sub >= CAP_MAX_TOPICS) continue; arr = p->sub; idx = p->n_sub++; }
        {   uint8_t c = t.name_len > DART_TOPIC_NAME_MAX ? DART_TOPIC_NAME_MAX : t.name_len;
            memcpy(arr[idx].name, t.name, c); arr[idx].name[c] = '\0'; }
        arr[idx].alias    = t.alias;
        arr[idx].reliable = t.reliable;
    }

    rc = meta_len < CAP_RAW_META ? meta_len : CAP_RAW_META;
    memcpy(p->raw, meta, rc);
    p->raw_len = rc;
}

static void cap_topics_csv(const CapTopic *t, int n, char *buf, size_t cap){
    int i; size_t off = 0;
    if (!n){ snprintf(buf, cap, "-"); return; }
    for (i = 0; i < n && off + 1 < cap; i++)
        off += (size_t)snprintf(buf + off, cap - off, "%s%s", i ? "," : "", t[i].name);
}

static void cap_peer_up(uint32_t id, const DartDiscoveryAddr *addr, const char *name, uint8_t name_len,
                           const uint8_t *meta, uint16_t meta_len){
    CapPeer *p = cap_peer_get(id);
    CapPeerState was = p->state;
    char a[80];

    p->state = CAP_ACTIVE;
    memcpy(p->ip, addr->ip, 16);
    p->ip_len = addr->ip_len;
    p->port = addr->port;
    cap_decode_meta(p, meta, meta_len);
    {   uint8_t nl = name_len > DART_NODE_NAME_MAX ? (uint8_t)DART_NODE_NAME_MAX : name_len;  /* name: discovery-level now */
        if (name && nl) memcpy(p->name, name, nl);
        p->name[nl] = '\0'; }
    p->updates++;
    p->last_change_ms = cap_now_ms();

    cap_fmt_addr(a, sizeof a, p->ip, p->ip_len, p->port);
    cap_logf("UP    id=%u  %-16s %s%s", id, p->name[0] ? p->name : "(no name)", a,
             was == CAP_DROPPED ? "  (resumed)" : (p->updates > 1 ? "  (updated)" : ""));
    if (p->have_meta){
        char pubs[160], subs[160];
        cap_topics_csv(p->pub, p->n_pub, pubs, sizeof pubs);
        cap_topics_csv(p->sub, p->n_sub, subs, sizeof subs);
        cap_logf("        meta v%d, frag=%u   pub[%d]={%s}  sub[%d]={%s}",
                 p->meta_version, p->frag, p->n_pub, pubs, p->n_sub, subs);
    }
}

static void cap_peer_down(uint32_t id, DartDiscoveryDownReason reason){
    CapPeer *p = cap_peer_find(id);
    if (!p){
        cap_logf("DOWN  id=%u  [%s, peer unknown]", id, reason == DART_DISCOVERY_GONE ? "GONE" : "DROP");
        return;
    }
    p->state = (reason == DART_DISCOVERY_GONE) ? CAP_GONE : CAP_DROPPED;
    p->down_ms = cap_now_ms();
    cap_logf("DOWN  id=%u  %-16s -> %s", id, p->name[0] ? p->name : "(no name)",
             reason == DART_DISCOVERY_GONE ? "GONE (state freed)" : "DROPPED (silent, may return)");
}

static void cap_peer_refused(const DartDiscoveryAddr *addr){
    char a[80];
    cap_fmt_addr(a, sizeof a, addr->ip, addr->ip_len, addr->port);
    cap_logf("REFUSED %s  (peer table full of active peers)", a);
}

/* one discovery event sink (the generic DartDiscoveryEvent), demuxed to the handlers above */
static void cap_on_event(const DartDiscoveryEvent *ev){
    switch (ev->kind){
        case DART_DISCOVERY_PEER_UP:      cap_peer_up(ev->peer, &ev->addr, ev->name, ev->name_len, ev->meta, ev->meta_len); break;
        case DART_DISCOVERY_PEER_DOWN:    cap_peer_down(ev->peer, ev->reason); break;
        case DART_DISCOVERY_PEER_REFUSED: cap_peer_refused(&ev->addr); break;
        default: break;
    }
}

static void cap_build_self_meta(void){
    /* the OVERLAY a passive observer advertises: the v6 (no-SHM) prefix ['D','N',6,frag_lo,
       frag_hi] + an empty interest list [npub16=0][nsub16=0]. The name is NOT here: it rides
       discovery's own blob section (rcfg.discovery.name). An observer publishes nothing. */
    uint16_t frag = (uint16_t)dart_clamp_frag(0);
    uint8_t *o = cap_self_meta;
    o[0] = 'D'; o[1] = 'N'; o[2] = 6;
    o[3] = (uint8_t)(frag & 0xFF); o[4] = (uint8_t)(frag >> 8);
    o[5] = 0; o[6] = 0;              /* npub = 0 */
    o[7] = 0; o[8] = 0;              /* nsub = 0 */
    cap_self_meta_len = 9;
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
           "  --name  STR  this observer's name        (default dart-explorer)\n", argv0);
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
    DartDiscovery *d;

    memset(cap, 0, sizeof *cap);
    cap_start_ms = cap_now_ms();
    cap_cfg = *cfg;          /* group/name point into argv: stable for the run */

    cap_build_self_meta();   /* the opaque overlay we advertise (discovery auto-gens our uuid) */

    /* the defaults-first public open: discovery owns its memory via the allocator.
       meta_capacity is generous (a node sizes its blob to its own interest, but an
       observer wants to receive everyone's; the default 64 is too small past a couple
       of topics). The overlay we pass is opaque to discovery. */
    mem = dart_allocator_dynamic(1 << 16);
    d = dart_discovery_open(&mem, cfg->name, &(DartDiscoveryConfig){
        .domain              = cfg->domain,
        .discovery_group     = cfg->group,
        .discovery_port      = cfg->port,
        .multicast_interface = cfg->ifc,
        .max_peers           = CAP_MAX_PEERS,
        .on_event            = cap_on_event,
        .meta                = cap_self_meta,
        .meta_len            = cap_self_meta_len,
        .meta_capacity       = 1408,
    });
    if (!d){
        fprintf(stderr, "cap_start: dart_discovery_open failed (port %u in use? interface?)\n", cfg->port);
        return 0;
    }
    cap->rt  = d;
    cap->mem = NULL;         /* discovery owns its memory now; close frees it */
    cap_logf("observer started on domain %u (%s:%u)", cfg->domain, cfg->group, cfg->port);
    return 1;
}

int cap_poll(Capture *cap){
    int guard = 0;
    if (!cap->rt) return 0;
    while (dart_discovery_poll((DartDiscovery *)cap->rt, 0) > 0 && ++guard < 256){ }
    return guard;
}

void cap_stop(Capture *cap){
    if (cap->rt) dart_discovery_close((DartDiscovery *)cap->rt, 1);   /* frees discovery's own memory */
    cap->rt = NULL; cap->mem = NULL;
}

void cap_snapshot(const Capture *cap, CapSnapshot *out){
    unsigned long long now = cap_now_ms();
    int i, k;
    (void)cap;   /* the peer table is file-static; cap is kept for API symmetry */

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

    for (i = 0; i < CAP_MAX_PEERS && out->n_nodes < CAP_SNAP_NODES; i++){
        const CapPeer *p = &cap_peers[i];
        CapNode *n;
        if (!p->used) continue;
        n = &out->nodes[out->n_nodes++];
        n->id           = p->local_id;
        n->state        = (CapState)p->state;   /* CAP_ACTIVE/DROPPED/GONE align with CapState */
        n->have_meta    = p->have_meta;
        n->meta_version = p->meta_version;
        n->frag         = p->frag;
        n->meta_len     = p->raw_len;
        n->port         = p->port;
        n->updates      = p->updates;
        n->observed_s   = (double)(now - p->first_seen_ms) / 1000.0;
        n->age_s        = (double)(now - p->last_change_ms) / 1000.0;
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
