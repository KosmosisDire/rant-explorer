/* demo_scene: one node of the handoff sample mesh, by profile name. Run six of
   these (one per profile) and the DART Explorer shows a populated node list and a
   nice hierarchical topic tree (sensors/lidar/..., robot/..., planning/...,
   diagnostics/...). It only DECLARES pub/sub interest (the tree is built from the
   discovery announces); it publishes nothing.

   ONE NODE PER PROCESS on purpose: several DART discovery participants in a single
   process all share the rendezvous port (7400, SO_REUSEADDR), and unicast blob
   replies to that shared port get delivered to the wrong co-bound socket, so peers
   show up nameless. Separate processes (like dart_test does) avoid that.

   Build (from the repo root):
     gcc -std=c99 -Wall -Idist explore/demo_scene.c -o demo_scene.exe -lws2_32 -lbcrypt -lwinmm
     cc  -std=c99 -Wall -Idist explore/demo_scene.c -o demo_scene -lrt          (POSIX)
   Run one: ./demo_scene <profile> [--domain N] [--if IP]
     profiles: perception planner lidar-driver camera-driver controller logger */
#define DART_IMPLEMENTATION
#include "dart.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

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

static DartNode *open_node(const Profile *p, uint16_t domain, const char *ifc){
    DartAllocator mem = dart_allocator_dynamic(1u << 20);
    DartNode *n;
    int i;
    n = dart_node_open(&mem, p->name, NULL, &(DartNodeOpts){
        .domain = domain, .max_channels = 16,
        .net = { .multicast_interface = ifc } });
    if (!n){ fprintf(stderr, "  open %s failed\n", p->name); return NULL; }
    for (i = 0; p->pubs && p->pubs[i]; i++)
        dart_node_create_channel(n, p->pubs[i], DART_PUB_ONLY, &(DartChannelOpts){
            .qos = { .reliability = reliable(p->pubs[i]) ? DART_RELIABLE : DART_BEST_EFFORT } });
    for (i = 0; p->subs && p->subs[i]; i++)
        dart_node_create_channel(n, p->subs[i], DART_SUB_ONLY, &(DartChannelOpts){
            .qos = { .reliability = reliable(p->subs[i]) ? DART_RELIABLE : DART_BEST_EFFORT } });
    return n;
}

int main(int argc, char **argv){
    uint16_t domain = 0;             /* DART default domain (matches the explorer's default) */
    const char *ifc = "127.0.0.1";   /* single-host loopback, matching the default observer */
    const char *want = NULL;         /* which profile to run */
    const Profile *prof = NULL;
    DartNode *node;
    int i;

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
    node = open_node(prof, domain, ifc);
    if (!node) return 1;
    printf("%s: up on domain %u (interface %s), interest only -- Ctrl-C to stop\n",
           prof->name, domain, ifc);

    while (g_run) dart_node_poll(node, 20);

    dart_node_close(node, 1);
    return 0;
}
