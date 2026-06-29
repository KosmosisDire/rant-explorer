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

static Machine g_machines[UID_MAX_MACH];
static Node    g_nodes[UID_MAX_NODES];
static Topic   g_topics[UID_MAX_TOPICS];

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

        /* discovery state -> display state. A freshly-seen active peer (or one whose
           announce blob hasn't arrived yet) reads as JOINING; dropped/gone fold to
           GONE (kept visible, de-emphasized). */
        if (cn->state == CAP_ST_ACTIVE)
            n->state = (cn->observed_s < 2.5 || !cn->have_meta) ? NODE_JOINING : NODE_ALIVE;
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

    /* second pass: attach pub/sub node lists to each topic + derive its reliability
       (a topic is RELIABLE if any publisher offers reliable). */
    for (ni = 0; ni < nn; ni++){
        Node *n = &g_nodes[ni];
        for (k = 0; k < n->n_pubs; k++){
            Topic *t = &g_topics[n->pubs[k]];
            if (t->n_pubs < UI_MAX_ENDPOINTS) t->pubs[t->n_pubs++] = ni;
            if (n->pub_rel[k]){ t->qos.reliability = QOS_RELIABLE; t->reliable = 1; }
        }
        for (k = 0; k < n->n_subs; k++){
            Topic *t = &g_topics[n->subs[k]];
            if (t->n_subs < UI_MAX_ENDPOINTS) t->subs[t->n_subs++] = ni;
        }
    }

    /* third pass: our own live subscription state (the topic light), matched by name.
       active + clean = subscribed (green), active + error/drops = red, absent = grey. */
    for (k = 0; k < snap->n_subs; k++){
        const CapSubInfo *si = &snap->subs[k];
        int ti;
        for (ti = 0; ti < n_top; ti++) if (!strcmp(g_topics[ti].path, si->name)) break;
        if (ti >= n_top) continue;
        g_topics[ti].sub_state = si->active ? (si->error ? 2 : 1) : 0;
    }

    D->machines = g_machines; D->n_machines = n_mach;
    D->nodes    = g_nodes;    D->n_nodes    = nn;
    D->topics   = g_topics;   D->n_topics   = n_top;
    D->logs     = NULL;       D->n_logs     = snap->n_log;   /* Log tab body wired later */
}

#endif /* UI_DATA_H */
