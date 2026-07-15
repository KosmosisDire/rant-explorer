/* demo_scene: one node of the handoff sample mesh, by profile name. Run six of
   these (one per profile) and the DART Explorer shows a populated node list and a
   nice hierarchical topic tree (sensors/lidar/..., robot/..., planning/...,
   diagnostics/...). It DECLARES pub/sub interest (the tree is built from the discovery
   announces) and also PUBLISHES a small readable payload on each of its pub topics at a
   per-topic rate, so the explorer's live feed shows real messages once you subscribe.

   ONE NODE PER PROCESS on purpose: several DART discovery participants in a single
   process all share the rendezvous port (7400, SO_REUSEADDR), and unicast blob
   replies to that shared port get delivered to the wrong co-bound socket, so peers
   show up nameless. Separate processes (like dart_test does) avoid that.

   Build (from the repo root):
     gcc -std=c99 -Wall -Idist explore/demo_scene.c -o demo_scene.exe -lws2_32 -lbcrypt -lwinmm
     cc  -std=c99 -Wall -Idist explore/demo_scene.c -o demo_scene -lrt          (POSIX)
   Run one: ./demo_scene <profile> [--domain N] [--if IP]
     profiles: perception planner lidar-driver camera-driver controller logger */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601          /* GetTickCount64 */
#endif
#define DART_IMPLEMENTATION
#include "dart.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#ifndef _WIN32
#include <time.h>
#endif

static unsigned long long now_ms(void){
#ifdef _WIN32
    return (unsigned long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)(ts.tv_nsec / 1000000);
#endif
}

/* a publishing topic: its handle, name, period, and running sequence number */
typedef struct {
    DartTopic       *ch;
    const char        *topic;
    unsigned           period_ms;
    unsigned           seq;
    unsigned long long next_ms;
} PubCh;

#define MAX_PUBS 8

/* per-topic publish cadence: motion fast, scans medium, status slow */
static unsigned topic_period_ms(const char *path){
    if (strstr(path, "pose") || strstr(path, "odom") || strstr(path, "cmd_vel")) return 100;  /* 10 Hz */
    if (strstr(path, "points") || strstr(path, "image"))                         return 200;  /*  5 Hz */
    if (strstr(path, "health") || strstr(path, "info") || strstr(path, "heartbeat")) return 1000; /* 1 Hz */
    return 500;  /* 2 Hz: goal / path / diagnostics */
}

static volatile sig_atomic_t g_run = 1;
static void on_sig(int s){ (void)s; g_run = 0; }

/* RELIABLE topics (matching data.js); everything else is best-effort. A topic's
   publisher and subscriber must agree, so this one lookup drives both. */
static int reliable(const char *path){
    static const char *rel[] = {
        "sensors/lidar/health", "sensors/camera/front/info", "robot/pose",
        "robot/cmd_vel", "robot/odom", "planning/goal", "planning/path",
        "diagnostics", "diagnostics/heartbeat", NULL };
    int i;
    for (i = 0; rel[i]; i++) if (!strcmp(rel[i], path)) return 1;
    return 0;
}

typedef struct { const char *name; const char **pubs; const char **subs; } Profile;

/* open the node, declare its interest, and record its pub topics into pubs[] (up to
   MAX_PUBS) so the main loop can publish to them. *n_pubs gets the count. */
static DartNode *open_node(const Profile *p, uint16_t domain, const char *ifc,
                           PubCh *pubs, int *n_pubs){
    DartAllocator mem = dart_allocator_dynamic(i_dart_plat_realloc, 0);
    DartNode *n;
    int i;
    *n_pubs = 0;
    n = dart_node_open(&mem, p->name, NULL, NULL, &(DartNodeOpts){
        .domain = domain, .max_topics = 16,
        .net = { .multicast_interface = ifc } });
    if (!n){ fprintf(stderr, "  open %s failed\n", p->name); return NULL; }
    for (i = 0; p->pubs && p->pubs[i]; i++){
        DartTopic *ch = dart_node_create_topic(n, p->pubs[i], DART_PUB_ONLY, NULL, &(DartTopicOpts){
            .qos = { .reliability = reliable(p->pubs[i]) ? DART_RELIABLE : DART_BEST_EFFORT,
                     .keep_last = 16 } });
        if (ch && *n_pubs < MAX_PUBS){
            PubCh *pc = &pubs[(*n_pubs)++];
            pc->ch = ch; pc->topic = p->pubs[i];
            pc->period_ms = topic_period_ms(p->pubs[i]); pc->seq = 0; pc->next_ms = 0;
        }
    }
    for (i = 0; p->subs && p->subs[i]; i++)
        dart_node_create_topic(n, p->subs[i], DART_SUB_ONLY, NULL, &(DartTopicOpts){
            .qos = { .reliability = reliable(p->subs[i]) ? DART_RELIABLE : DART_BEST_EFFORT } });
    return n;
}

int main(int argc, char **argv){
    uint16_t domain = 0;             /* DART default domain (matches the explorer's default) */
    const char *ifc = "127.0.0.1";   /* single-host loopback, matching the default observer */
    const char *want = NULL;         /* which profile to run */
    const Profile *prof = NULL;
    DartNode *node;
    PubCh pubs[MAX_PUBS];
    int n_pubs = 0, i;
    unsigned long long start_ms;

    /* topic sets per node (from the handoff's data.js sample) */
    static const char *perception_p[] = { "sensors/lidar/points", "robot/pose", NULL };
    static const char *perception_s[] = { "sensors/camera/front/image", "sensors/imu", NULL };
    static const char *planner_p[]    = { "planning/goal", "planning/path", NULL };
    static const char *planner_s[]    = { "robot/pose", "robot/odom", NULL };
    static const char *lidar_p[]      = { "sensors/lidar/points", "sensors/lidar/health", NULL };
    static const char *camera_p[]     = { "sensors/camera/front/image", "sensors/camera/front/info", NULL };
    static const char *controller_p[] = { "robot/cmd_vel", "robot/odom", "diagnostics", NULL };
    static const char *controller_s[] = { "planning/path", "robot/pose", NULL };
    static const char *logger_p[]     = { "diagnostics/heartbeat", NULL };
    static const char *logger_s[]     = { "sensors/lidar/points", "robot/pose", "robot/cmd_vel",
                                          "planning/path", "diagnostics", NULL };

    static const Profile profiles[] = {
        { "perception",    perception_p, perception_s },
        { "planner",       planner_p,    planner_s },
        { "lidar-driver",  lidar_p,      NULL },
        { "camera-driver", camera_p,     NULL },
        { "controller",    controller_p, controller_s },
        { "logger",        logger_p,     logger_s },
    };
    enum { N = (int)(sizeof profiles / sizeof profiles[0]) };

    for (i = 1; i < argc; i++){
        if      (!strcmp(argv[i], "--domain") && i+1 < argc) domain = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--if")     && i+1 < argc) ifc    = argv[++i];
        else if (argv[i][0] != '-' && !want)                 want   = argv[i];
    }
    for (i = 0; i < N; i++) if (want && !strcmp(want, profiles[i].name)) prof = &profiles[i];
    if (!prof){
        fprintf(stderr, "usage: %s <profile> [--domain N] [--if IP]\n  profiles:", argv[0]);
        for (i = 0; i < N; i++) fprintf(stderr, " %s", profiles[i].name);
        fprintf(stderr, "\n");
        return 2;
    }

    signal(SIGINT, on_sig);
    setvbuf(stdout, NULL, _IONBF, 0);
    node = open_node(prof, domain, ifc, pubs, &n_pubs);
    if (!node) return 1;
    printf("%s: up on domain %u (interface %s), publishing %d topic(s) -- Ctrl-C to stop\n",
           prof->name, domain, ifc, n_pubs);

    start_ms = now_ms();
    while (g_run){
        unsigned long long t = now_ms();
        for (i = 0; i < n_pubs; i++){
            if (t >= pubs[i].next_ms){
                char buf[120];
                int len = snprintf(buf, sizeof buf, "%s #%u from %s @ %.2fs",
                                   pubs[i].topic, pubs[i].seq, prof->name,
                                   (double)(t - start_ms) / 1000.0);
                if (len < 0) len = 0;
                if (len > (int)sizeof buf) len = (int)sizeof buf;
                dart_topic_send(pubs[i].ch, dart_bytes(buf, (size_t)len));
                pubs[i].seq++;
                pubs[i].next_ms = t + pubs[i].period_ms;
            }
        }
        dart_node_poll(node, 10);
    }

    dart_node_close(node, 1);
    return 0;
}
