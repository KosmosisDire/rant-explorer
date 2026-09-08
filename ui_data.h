/* Build the UI Dataset from a live net_capture snapshot. Owns the static backing arrays
   and is rebuilt every frame. Only wire derived facts are modeled. */
#ifndef UI_DATA_H
#define UI_DATA_H

#define UID_MAX_NODES   CAP_SNAP_NODES
#define UID_MAX_TOPICS  16000  /* distinct topics across the whole observed graph (tree size) */
#define UID_MAX_MACH    CAP_SNAP_NODES
#define UID_MAX_USER    16    /* user-added topics (via the + button), kept across frames */
#define UID_TOPIC_HASH  32768   /* a power of two over 2x UID_MAX_TOPICS, load under 0.5 */

static Machine g_machines[UID_MAX_MACH];
static Node    g_nodes[UID_MAX_NODES];
static Topic   g_topics[UID_MAX_TOPICS];
static char    g_user_topics[UID_MAX_USER][CAP_TOPIC_CAP];
static int     g_n_user_topics;

/* path to g_topics index, so uid_topic_for dedups in O(1). A scan was O(n squared) per
   frame and froze the UI at many thousand topics. Reset at the start of each build. */
static int g_topic_slot[UID_TOPIC_HASH];

static uint32_t uid_str_hash(const char *s){
    uint32_t h = 2166136261u;
    for (; *s; s++){ h ^= (unsigned char)*s; h *= 16777619u; }
    return h;
}

/* register a topic the user typed, so it appears in the tree before any peer advertises
   it and can be published to. Deduped and capped. */
static void ui_data_add_user_topic(const char *name){
    int i;
    if (!name || !*name) return;
    for (i = 0; i < g_n_user_topics; i++) if (!strcmp(g_user_topics[i], name)) return;
    if (g_n_user_topics < UID_MAX_USER)
        snprintf(g_user_topics[g_n_user_topics++], CAP_TOPIC_CAP, "%s", name);
}

/* a machine is one advertised IP, since the host name and OS are not on discovery */
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

/* Look up a topic by path without inserting, the read only twin of uid_topic_for, so the
   self pub and sub passes stay O(1) per subscription. */
static int uid_topic_find(const char *path){
    uint32_t h = uid_str_hash(path) & (UID_TOPIC_HASH - 1);
    int idx;
    while ((idx = g_topic_slot[h]) >= 0){
        if (!strcmp(g_topics[idx].path, path)) return idx;
        h = (h + 1) & (UID_TOPIC_HASH - 1);
    }
    return -1;
}

/* topics are the union of every node's pub and sub names. Rate, age and jitter ride
   the data plane, real only once the explorer subscribes. */
static int uid_topic_for(const char *path, int *n_top){
    uint32_t h = uid_str_hash(path) & (UID_TOPIC_HASH - 1);
    int idx;
    while ((idx = g_topic_slot[h]) >= 0){   /* linear probe to an existing entry or a hole */
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
        t->jitter_p90_ms = -1.0;   /* not enough samples until a live subscription warms it */
    }
    g_topic_slot[h] = idx;                              /* the probe stopped on this empty slot */
    return idx;
}

static void ui_data_build(Dataset *D, const CapSnapshot *snap){
    int n_mach = 0, n_top = 0, ni, k;
    int nn = snap->n_nodes < UID_MAX_NODES ? snap->n_nodes : UID_MAX_NODES;

    memset(g_topic_slot, 0xFF, sizeof g_topic_slot);   /* 0xFF bytes = -1 ints, an empty map */

    /* seed user-added topics first so they exist (and merge with any peer that advertises
       the same name later via uid_topic_for's dedup) even with no publishers/subscribers */
    for (k = 0; k < g_n_user_topics; k++) uid_topic_for(g_user_topics[k], &n_top);

    for (ni = 0; ni < nn; ni++){
        const CapNode *cn = &snap->nodes[ni];
        Node *n = &g_nodes[ni];
        const char *nm = cn->name[0] ? cn->name : "(unnamed)";
        n->n_pubs = 0; n->n_subs = 0;   /* not memset: every other field is overwritten below and
                                           the tails past the counts are never read */

        snprintf(n->id,   sizeof n->id,   "%s", nm);
        snprintf(n->name, sizeof n->name, "%s", nm);
        snprintf(n->ip,   sizeof n->ip,   "%s", cn->ip);
        n->machine = uid_machine_for(cn->ip, &n_mach);

        /* display state: a peer whose announce blob has not arrived, or whose held blob is
           behind the version it advertises, reads as JOINING */
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
        n->disc.rtt_us = cn->rtt_us; n->disc.rtt_jitter_us = cn->rtt_jitter_us;
        n->disc.rtt_min_us = cn->rtt_min_us; n->disc.rtt_samples = cn->rtt_samples;

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
                /* task attrs come from the PROVIDES side only (the definition declares them) */
                if (cn->pub[k].cancellable) g_topics[ti].cancellable = 1;
                if (cn->pub[k].exclusive)   g_topics[ti].exclusive = 1;
                if (cn->pub[k].multi)       g_topics[ti].multi = 1;
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

    /* Two reliability rollups per topic from the announced interest: reliable_recommend is
       the reliability to subscribe as, reliable the displayed badge (spec/explorer.md). */
    /* our own live subscription and publish state, matched by name in O(1) in one pass.
       Runs before the reliability rollup so it counts our own publish as a publisher. */
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
        g_topics[ti].jitter_p90_ms = si->jitter_p90_ms;   /* live: the smoothed p90 jitter */
        g_topics[ti].mini       = si->mini;        /* live: newest value, compact (VALUE column) */
    }

    for (k = 0; k < n_top; k++){
        Topic *t = &g_topics[k];
        int j;
        int n_pub = t->n_pubs, all_rel = 1;         /* publishers */
        int s_all_rel = 1;                          /* subscribers */
        for (j = 0; j < t->n_pubs; j++) if (!t->pub_rel[j]) all_rel = 0;
        for (j = 0; j < t->n_subs; j++) if (!t->sub_rel[j]) s_all_rel = 0;
        /* the explorer publishing this topic counts as a publisher too, so a topic we
           publish reliably reads RELIABLE. self_pub was set in the pass above. */
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
}

#endif /* UI_DATA_H */
