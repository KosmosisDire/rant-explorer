/* Build the UI Dataset from a live net_capture snapshot. Owns the static backing
   arrays the Dataset points into and is rebuilt every frame (cheap: <= 64 nodes).

   The snapshot already separates origins (net_capture sources peers through a real
   node). Discovery-level facts become real data: node name, state, advertised unicast
   locator, and how long we've observed each peer. Announce-metadata facts (the overlay
   the node decoded) are also real: UDP fragment size + the pub/sub interest list (with
   per-endpoint reliability). What never rides the wire (CPU, memory, msg counts, real
   uptime, a peer's transport AckNack counters, the peer table IT holds) is left as a
   placeholder, marked < 0 / empty here and drawn as a dim em dash at the draw site.

   Requires ui_model.h (display structs) and net_capture.h (CapSnapshot). */
#ifndef UI_DATA_H
#define UI_DATA_H

#define UID_MAX_NODES   CAP_SNAP_NODES
#define UID_MAX_TOPICS  256
#define UID_MAX_MACH    CAP_SNAP_NODES
#define UID_MAX_USER    16    /* user-added topics (via the + button), kept across frames */

static Machine g_machines[UID_MAX_MACH];
static Node    g_nodes[UID_MAX_NODES];
static Topic   g_topics[UID_MAX_TOPICS];
static char    g_user_topics[UID_MAX_USER][CAP_TOPIC_CAP];
static int     g_n_user_topics;

/* register a topic the user typed in, so it appears in the tree even before any peer
   advertises it (so we can publish to a brand-new topic). Deduped; capped. */
static void ui_data_add_user_topic(const char *name){
    int i;
    if (!name || !*name) return;
    for (i = 0; i < g_n_user_topics; i++) if (!strcmp(g_user_topics[i], name)) return;
    if (g_n_user_topics < UID_MAX_USER)
        snprintf(g_user_topics[g_n_user_topics++], CAP_TOPIC_CAP, "%s", name);
}

/* a "machine" is one advertised IP; the host name / OS aren't on discovery. */
static int uid_machine_for(const char *ip, int *n_mach){
    int i;
    for (i = 0; i < *n_mach; i++) if (!strcmp(g_machines[i].ip, ip)) return i;
    if (*n_mach < UID_MAX_MACH){
        Machine *m = &g_machines[*n_mach];
        memset(m, 0, sizeof *m);
        snprintf(m->id,   sizeof m->id,   "%s", ip);
        snprintf(m->ip,   sizeof m->ip,   "%s", ip);
        m->host[0] = '\0';   /* host name: not observable */
        m->os[0]   = '\0';   /* OS: not observable */
        return (*n_mach)++;
    }
    return 0;
}

/* topics are the union of every node's pub/sub names; most fields are unknown to a
   discovery observer (rate/size/count/preview ride the data plane, not announces). */
static int uid_topic_for(const char *path, int *n_top){
    int i;
    for (i = 0; i < *n_top; i++) if (!strcmp(g_topics[i].path, path)) return i;
    if (*n_top < UID_MAX_TOPICS){
        Topic *t = &g_topics[*n_top];
        memset(t, 0, sizeof *t);
        snprintf(t->path, sizeof t->path, "%s", path);
        t->qos.reliability = QOS_BEST_EFFORT;   /* raised to RELIABLE if any publisher offers it */
        t->rate_on_event = 1;                /* rate unknown */
        t->last_age_s = -1.0;                /* placeholder */
        t->count = -1; t->size_bytes = -1;
        t->drops = -1;                       /* unknown / not tracked here */
        return (*n_top)++;
    }
    return -1;
}

static void ui_data_build(Dataset *D, const CapSnapshot *snap){
    int n_mach = 0, n_top = 0, ni, k;
    int nn = snap->n_nodes < UID_MAX_NODES ? snap->n_nodes : UID_MAX_NODES;

    /* seed user-added topics first so they exist (and merge with any peer that advertises
       the same name later via uid_topic_for's dedup) even with no publishers/subscribers */
    for (k = 0; k < g_n_user_topics; k++) uid_topic_for(g_user_topics[k], &n_top);

    for (ni = 0; ni < nn; ni++){
        const CapNode *cn = &snap->nodes[ni];
        Node *n = &g_nodes[ni];
        const char *nm = cn->name[0] ? cn->name : "(unnamed)";
        memset(n, 0, sizeof *n);

        snprintf(n->id,   sizeof n->id,   "%s", nm);
        snprintf(n->name, sizeof n->name, "%s", nm);
        snprintf(n->ip,   sizeof n->ip,   "%s", cn->ip);
        n->machine = uid_machine_for(cn->ip, &n_mach);
        n->pid = -1;                         /* placeholder */

        /* discovery state -> display state. An active peer whose announce blob hasn't arrived
           yet, or whose blob we hold is behind the version it now advertises (re-fetch pending),
           reads as JOINING. DROPPED = silent past peer_timeout but may still resume (kept
           prominent); GONE = BYE / gone-timeout / evicted, state freed (de-emphasized). */
        if (cn->state == CAP_ST_ACTIVE)
            n->state = (!cn->have_meta || cn->meta_stale) ? NODE_JOINING : NODE_ALIVE;
        else if (cn->state == CAP_ST_DROPPED)
            n->state = NODE_DROPPED;
        else
            n->state = NODE_GONE;

        /* runtime stats: none are observable from discovery */
        n->uptime_s = -1; n->heartbeat_age_s = -1; n->cpu_pct = -1;
        n->mem_bytes = -1; n->msgs_sent = -1; n->msgs_recv = -1;
        n->observed_s = cn->observed_s;      /* real (observer view) */
        n->updates    = cn->updates;         /* real */

        /* discovery-level: real where the announce header / protocol defaults carry it */
        n->disc.announce_period_s   = 1.0;   /* protocol default (DART_DISCOVERY announce interval) */
        n->disc.last_announce_age_s = cn->age_s;     /* last announce CHANGE we saw */
        n->disc.lease_s             = 3.5;   /* protocol default (peer_timeout = 3.5x announce) */
        snprintf(n->disc.unicast,         sizeof n->disc.unicast,         "%s:%u", cn->ip, cn->port);
        snprintf(n->disc.discovery_group, sizeof n->disc.discovery_group, "%s:%u", snap->group, snap->disc_port);

        /* announce metadata: real, decoded by our node from the peer's overlay */
        n->disc.frag_size_bytes     = cn->frag;       /* real (advertised) */
        n->disc.blob_bytes          = cn->meta_len;   /* real: observed overlay size */
        snprintf(n->disc.transport, sizeof n->disc.transport, "UDP");

        /* node-internal: never on the wire */
        n->disc.max_msg_bytes  = -1;
        n->disc.announces_sent = -1; n->disc.frags_tx = -1;
        n->disc.acknacks_rx    = -1; n->disc.nacks_rx = -1;

        for (k = 0; k < cn->n_pub && n->n_pubs < UI_MAX_ENDPOINTS; k++){
            int ti = uid_topic_for(cn->pub[k].name, &n_top);
            if (ti < 0) break;
            n->pubs[n->n_pubs]    = ti;
            n->pub_rel[n->n_pubs] = (unsigned char)cn->pub[k].reliable;
            n->n_pubs++;
        }
        for (k = 0; k < cn->n_sub && n->n_subs < UI_MAX_ENDPOINTS; k++){
            int ti = uid_topic_for(cn->sub[k].name, &n_top);
            if (ti < 0) break;
            n->subs[n->n_subs]    = ti;
            n->sub_rel[n->n_subs] = (unsigned char)cn->sub[k].reliable;
            n->n_subs++;
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
            if (t->n_subs < UI_MAX_ENDPOINTS) t->subs[t->n_subs++] = ni;
        }
    }

    /* derive each topic's reliability from its LIVE publishers: reliable only if every
       publisher we consider offers it (a mix downgrades to best-effort, since a reliable
       sub would refuse a best-effort publisher and get no data from it). Dropped/gone
       publishers are ignored unless they are the only ones. This drives both the displayed
       reliability and the recommended subscribe reliability (reliable_recommend; -1 = no
       publishers => best-effort). */
    for (k = 0; k < n_top; k++){
        Topic *t = &g_topics[k];
        int j, live = 0, live_all_rel = 1, any_rel = 1, any = 0;
        for (j = 0; j < t->n_pubs; j++){
            int rel = t->pub_rel[j];
            any = 1;
            if (!rel) any_rel = 0;
            if (g_nodes[t->pubs[j]].state != NODE_GONE && g_nodes[t->pubs[j]].state != NODE_DROPPED){ live++; if (!rel) live_all_rel = 0; }   /* silent peers aren't live publishers */
        }
        /* the explorer publishing this topic counts as a live publisher too, so a topic we
           publish reliably to reads RELIABLE (not the peer-only best-effort default) */
        for (j = 0; j < snap->n_subs; j++)
            if (snap->subs[j].publishing && !strcmp(snap->subs[j].name, t->path)){
                t->self_pub = 1; t->self_pub_reliable = snap->subs[j].reliable;
                any = 1; live++;
                if (!snap->subs[j].reliable){ any_rel = 0; live_all_rel = 0; }
                break;
            }
        t->reliable_recommend = !any ? -1 : (live ? live_all_rel : any_rel);
        t->reliable        = (t->reliable_recommend == 1);
        t->qos.reliability = t->reliable ? QOS_RELIABLE : QOS_BEST_EFFORT;
    }

    /* third pass: our own live subscription state (the topic light), matched by name.
       active + clean = subscribed (green), active + error/drops = red, absent = grey. */
    for (k = 0; k < snap->n_subs; k++){
        const CapSubInfo *si = &snap->subs[k];
        int ti;
        for (ti = 0; ti < n_top; ti++) if (!strcmp(g_topics[ti].path, si->name)) break;
        if (ti >= n_top) continue;
        g_topics[ti].sub_state = si->active ? (si->error ? 2 : 1) : 0;
        g_topics[ti].self_sub  = si->active;
        g_topics[ti].self_sub_reliable = si->reliable;
        g_topics[ti].rate_hz    = si->rate_hz;     /* live: publish rate over the stored window */
        g_topics[ti].last_age_s = si->last_age_s;  /* live: age of the newest received message */
    }

    D->machines = g_machines; D->n_machines = n_mach;
    D->nodes    = g_nodes;    D->n_nodes    = nn;
    D->topics   = g_topics;   D->n_topics   = n_top;
    D->logs     = NULL;       D->n_logs     = snap->n_log;   /* Log tab body wired later */
}

#endif /* UI_DATA_H */
