/* Build the UI Dataset from a live net_capture snapshot. Owns the static backing
   arrays the Dataset points into and is rebuilt every frame (cheap: <= 64 nodes).

   The snapshot already separates origins (net_capture sources peers through a real
   node). Discovery-level facts become real data: node name, state, advertised unicast
   locator, and how long we've observed each peer. Announce-metadata facts (the overlay
   the node decoded) are also real: UDP fragment size + the pub/sub interest list (with
   per-endpoint reliability). A peer's own runtime internals (CPU, memory, msg counts,
   uptime, its transport AckNack counters) never ride the wire, so this file does not
   model them at all.

   Requires ui_model.h (display structs) and net_capture.h (CapSnapshot). */
#ifndef UI_DATA_H
#define UI_DATA_H

#define UID_MAX_NODES   CAP_SNAP_NODES
#define UID_MAX_TOPICS  16000  /* distinct topics across the whole observed graph (tree size) */
#define UID_MAX_MACH    CAP_SNAP_NODES
#define UID_MAX_USER    16    /* user-added topics (via the + button), kept across frames */
#define UID_TOPIC_HASH  32768  /* power of two, >= 2*UID_MAX_TOPICS: keeps the path->index map < 0.5 load */

static Machine g_machines[UID_MAX_MACH];
static Node    g_nodes[UID_MAX_NODES];
static Topic   g_topics[UID_MAX_TOPICS];
static char    g_user_topics[UID_MAX_USER][CAP_TOPIC_CAP];
static int     g_n_user_topics;

/* path -> g_topics index, so uid_topic_for dedups in O(1) instead of scanning every existing
   topic (which was O(n^2) across a frame and froze the UI at many-thousand-topic scale). Slots
   hold an index or -1 (empty); reset to empty at the start of each ui_data_build. */
static int g_topic_slot[UID_TOPIC_HASH];

static uint32_t uid_str_hash(const char *s){
    uint32_t h = 2166136261u;
    for (; *s; s++){ h ^= (unsigned char)*s; h *= 16777619u; }
    return h;
}

/* register a topic the user typed in, so it appears in the tree even before any peer
   advertises it (so we can publish to a brand-new topic). Deduped; capped. */
static void ui_data_add_user_topic(const char *name){
    int i;
    if (!name || !*name) return;
    for (i = 0; i < g_n_user_topics; i++) if (!strcmp(g_user_topics[i], name)) return;
    if (g_n_user_topics < UID_MAX_USER)
        snprintf(g_user_topics[g_n_user_topics++], CAP_TOPIC_CAP, "%s", name);
}

/* a "machine" is one advertised IP; the host name / OS aren't on discovery, so a
   machine is identified by that IP alone. */
static int uid_machine_for(const char *ip, int *n_mach){
    int i;
    for (i = 0; i < *n_mach; i++) if (!strcmp(g_machines[i].ip, ip)) return i;
    if (*n_mach < UID_MAX_MACH){
        Machine *m = &g_machines[*n_mach];
        memset(m, 0, sizeof *m);
        snprintf(m->id,   sizeof m->id,   "%s", ip);
        snprintf(m->ip,   sizeof m->ip,   "%s", ip);
        return (*n_mach)++;
    }
    return 0;
}

/* Look up a topic by path WITHOUT inserting (the read-only twin of uid_topic_for): the
   same open-addressed probe, returning the g_topics index or -1. Lets the self pub/sub
   passes match a subscription name to its topic in O(1), so raising the subscription cap
   to the topic ceiling does not turn those passes into O(topics x subs) scans. */
static int uid_topic_find(const char *path){
    uint32_t h = uid_str_hash(path) & (UID_TOPIC_HASH - 1);
    int idx;
    while ((idx = g_topic_slot[h]) >= 0){
        if (!strcmp(g_topics[idx].path, path)) return idx;
        h = (h + 1) & (UID_TOPIC_HASH - 1);
    }
    return -1;
}

/* topics are the union of every node's pub/sub names; rate/last-age/jitter ride the
   data plane (real only once the explorer subscribes), not the discovery announce. */
static int uid_topic_for(const char *path, int *n_top){
    uint32_t h = uid_str_hash(path) & (UID_TOPIC_HASH - 1);
    int idx;
    while ((idx = g_topic_slot[h]) >= 0){               /* linear probe to an existing entry or a hole */
        if (!strcmp(g_topics[idx].path, path)) return idx;
        h = (h + 1) & (UID_TOPIC_HASH - 1);
    }
    if (*n_top >= UID_MAX_TOPICS) return -1;
    idx = (*n_top)++;
    {   Topic *t = &g_topics[idx];
        memset(t, 0, sizeof *t);
        snprintf(t->path, sizeof t->path, "%s", path);
        t->qos.reliability = QOS_BEST_EFFORT;   /* raised to RELIABLE if any publisher offers it */
        t->last_age_s = -1.0;                /* not yet observed */
        t->jitter_p90_ms = -1.0;              /* not enough samples yet, until a live subscription warms it up */
    }
    g_topic_slot[h] = idx;                              /* the probe stopped on this empty slot */
    return idx;
}

static void ui_data_build(Dataset *D, const CapSnapshot *snap){
    int n_mach = 0, n_top = 0, ni, k;
    int nn = snap->n_nodes < UID_MAX_NODES ? snap->n_nodes : UID_MAX_NODES;

    memset(g_topic_slot, 0xFF, sizeof g_topic_slot);   /* 0xFF bytes = -1 ints: empty the path->index map */

    /* seed user-added topics first so they exist (and merge with any peer that advertises
       the same name later via uid_topic_for's dedup) even with no publishers/subscribers */
    for (k = 0; k < g_n_user_topics; k++) uid_topic_for(g_user_topics[k], &n_top);

    for (ni = 0; ni < nn; ni++){
        const CapNode *cn = &snap->nodes[ni];
        Node *n = &g_nodes[ni];
        const char *nm = cn->name[0] ? cn->name : "(unnamed)";
        n->n_pubs = 0; n->n_subs = 0;   /* not memset: every other field is overwritten below, and the
                                           pubs[]/subs[] tails past n_pubs/n_subs are never read (see the
                                           count-bounded fills). Zeroing the 16000-wide arrays each frame
                                           would be the same per-frame tax the snapshot memset was. */

        snprintf(n->id,   sizeof n->id,   "%s", nm);
        snprintf(n->name, sizeof n->name, "%s", nm);
        snprintf(n->ip,   sizeof n->ip,   "%s", cn->ip);
        n->machine = uid_machine_for(cn->ip, &n_mach);

        /* display state (every snapshot node is a live peer): one whose announce blob hasn't
           arrived yet, or whose blob we hold is behind the version it now advertises
           (re-fetch pending), reads as JOINING. */
        n->state = (!cn->have_meta || cn->meta_stale) ? NODE_JOINING : NODE_ALIVE;

        n->observed_s = cn->observed_s;      /* real (observer view) */
        n->updates    = cn->updates;         /* real */

        /* discovery-level: real, from the announce header */
        n->disc.last_announce_age_s = cn->age_s;     /* last announce CHANGE we saw */
        snprintf(n->disc.unicast,         sizeof n->disc.unicast,         "%s:%u", cn->ip, cn->port);
        snprintf(n->disc.discovery_group, sizeof n->disc.discovery_group, "%s:%u", snap->group, snap->disc_port);

        /* announce metadata: real, decoded by our node from the peer's overlay */
        n->disc.frag_size_bytes     = cn->frag;       /* real (advertised) */
        n->disc.blob_bytes          = cn->meta_len;   /* real: observed overlay size */

        for (k = 0; k < cn->n_pub && n->n_pubs < UI_MAX_NODE_TOPICS; k++){
            int ti = uid_topic_for(cn->pub[k].name, &n_top);
            if (ti < 0) break;
            n->pubs[n->n_pubs]    = ti;
            n->pub_rel[n->n_pubs] = (unsigned char)cn->pub[k].reliable;
            n->n_pubs++;
            if (cn->pub[k].kind){   /* entity kind (agrees across peers) */
                g_topics[ti].kind = cn->pub[k].kind;
                if (cn->pub[k].writable)   g_topics[ti].writable = 1;
                if (cn->pub[k].forceable)  g_topics[ti].forceable = 1;
                if (cn->pub[k].incomplete) g_topics[ti].incomplete = 1;
            }
        }
        for (k = 0; k < cn->n_sub && n->n_subs < UI_MAX_NODE_TOPICS; k++){
            int ti = uid_topic_for(cn->sub[k].name, &n_top);
            if (ti < 0) break;
            n->subs[n->n_subs]    = ti;
            n->sub_rel[n->n_subs] = (unsigned char)cn->sub[k].reliable;
            n->n_subs++;
            if (cn->sub[k].kind){
                g_topics[ti].kind = cn->sub[k].kind;
                if (cn->sub[k].writable)   g_topics[ti].writable = 1;
                if (cn->sub[k].forceable)  g_topics[ti].forceable = 1;
                if (cn->sub[k].incomplete) g_topics[ti].incomplete = 1;
            }
        }
    }

    /* second pass: attach each topic's pub/sub node lists (with per-publisher reliability) */
    for (ni = 0; ni < nn; ni++){
        Node *n = &g_nodes[ni];
        for (k = 0; k < n->n_pubs; k++){
            Topic *t = &g_topics[n->pubs[k]];
            if (t->n_pubs < UI_MAX_ENDPOINTS){
                t->pub_rel[t->n_pubs] = n->pub_rel[k];
                t->pubs[t->n_pubs++]  = ni;
            }
        }
        for (k = 0; k < n->n_subs; k++){
            Topic *t = &g_topics[n->subs[k]];
            if (t->n_subs < UI_MAX_ENDPOINTS){
                t->sub_rel[t->n_subs] = n->sub_rel[k];
                t->subs[t->n_subs++]  = ni;
            }
        }
    }

    /* Two reliability rollups per topic, both from the peers' announced interest (so they
       are correct even when the explorer neither publishes nor subscribes):
         - reliable_recommend: the reliability to SUBSCRIBE AS. Publisher-driven, because it
           governs what we can receive (a reliable sub refuses a best-effort publisher).
           Reliable only if every publisher offers it. -1 = no publishers.
         - reliable (the DISPLAYED badge): the publishers define it when present; a topic
           with ONLY subscribers takes THEIR reliability instead, so a reliable
           subscriber-only topic still reads reliable. has_qos = any endpoint at all
           (else the badge is a dash). */
    /* our own live subscription/publish state, matched by name in O(1) via the topic hash
       (folded into ONE pass over the subscriptions, so it stays cheap at the topic-ceiling
       cap): the topic light + self_sub/self_pub flags + live rate/age/jitter. Runs BEFORE
       the reliability rollup so the rollup can count our own publish as a live publisher. */
    for (k = 0; k < snap->n_subs; k++){
        const CapSubInfo *si = &snap->subs[k];
        int ti = uid_topic_find(si->name);
        if (ti < 0) continue;
        if (si->publishing){ g_topics[ti].self_pub = 1; g_topics[ti].self_pub_reliable = si->reliable; }
        g_topics[ti].sub_state = si->active ? (si->error ? 2 : 1) : 0;
        g_topics[ti].self_sub  = si->active;
        g_topics[ti].self_sub_reliable = si->reliable;
        g_topics[ti].rate_hz    = si->rate_hz;     /* live: publish rate over the stored window */
        g_topics[ti].last_age_s = si->last_age_s;  /* live: age of the newest received message */
        g_topics[ti].jitter_p90_ms = si->jitter_p90_ms;  /* live: p90 inter-arrival jitter, smoothed */
    }

    for (k = 0; k < n_top; k++){
        Topic *t = &g_topics[k];
        int j;
        int n_pub = t->n_pubs, all_rel = 1;         /* publishers */
        int s_all_rel = 1;                          /* subscribers */
        for (j = 0; j < t->n_pubs; j++) if (!t->pub_rel[j]) all_rel = 0;
        for (j = 0; j < t->n_subs; j++) if (!t->sub_rel[j]) s_all_rel = 0;
        /* the explorer publishing this topic counts as a publisher too, so a topic we
           publish reliably to reads RELIABLE (not the peer-only best-effort default). self_pub
           was set in the O(1) subscription pass above. */
        if (t->self_pub){
            n_pub++;
            if (!t->self_pub_reliable) all_rel = 0;
        }
        t->reliable_recommend = n_pub ? all_rel : -1;
        if (n_pub){                             /* publishers define the topic's QoS */
            t->has_qos = 1; t->reliable = all_rel;
        } else if (t->n_subs){                  /* subscriber-only: take the subscribers' QoS */
            t->has_qos = 1; t->reliable = s_all_rel;
        } else {                                /* no endpoints yet */
            t->has_qos = 0; t->reliable = 0;
        }
        t->qos.reliability = t->reliable ? QOS_RELIABLE : QOS_BEST_EFFORT;
    }

    D->machines = g_machines; D->n_machines = n_mach;
    D->nodes    = g_nodes;    D->n_nodes    = nn;
    D->topics   = g_topics;   D->n_topics   = n_top;
    D->logs     = NULL;       D->n_logs     = snap->n_log;   /* Log tab body wired later */
}

#endif /* UI_DATA_H */
