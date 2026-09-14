/* One node of the sample mesh, by profile name. Run one per profile and the explorer shows
   a populated node list and topic tree. One node per process (spec/explorer.md). */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601          /* GetTickCount64 */
#endif
#define RAMBLE_IMPLEMENTATION
#include "ramble.h"

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
    RambleTopic       *ch;
    const char        *topic;
    unsigned           period_ms;
    unsigned           seq;
    unsigned long long next_ms;
} PubCh;

#define MAX_PUBS 8

/* per-topic publish cadence: motion fast, scans medium, status slow */
static unsigned topic_period_ms(const char *path){
    if (strstr(path, "pose") || strstr(path, "odom") || strstr(path, "cmd_vel")) return 100;
    if (strstr(path, "points") || strstr(path, "image"))                         return 200;
    if (strstr(path, "health") || strstr(path, "info") || strstr(path, "heartbeat")) return 1000;
    return 500;  /* 2 Hz: goal / path / diagnostics */
}

static volatile sig_atomic_t g_run = 1;
static void on_sig(int s){ (void)s; g_run = 0; }

/* The demo task: a slow lidar calibration defined on lidar-driver and called by perception
   every 10 s. A single threaded superloop: the handler defers, the main loop steps. */
#define TASK_NAME "sensors/lidar/calibrate"
static RambleSchema   *task_req_s, *task_prg_s, *task_rsp_s;
static RambleFunction *task_def, *task_remote;
static volatile uint64_t task_token;     /* the one live job (the handler answers "busy" else) */
static unsigned task_done, task_total;
static unsigned long long task_step_ms, task_next_call_ms;

static void on_calibrate(RambleRequest *req, void *user){
    (void)user;
    if (task_token){ ramble_request_fail(req, "busy: a calibration is running", ramble_bytes(NULL, 0)); return; }
    task_total = 20;
    if (req->schema){
        unsigned s = (unsigned)ramble_get_uint(req->data, req->schema, "sweeps");
        if (s) task_total = s > 60 ? 60 : s;
    }
    task_done  = 0;
    task_token = ramble_request_defer(req);   /* implies RUNNING to the caller */
}

/* one calibration step per tick: progress {done,total}, cancel honored, OK at the end */
static void task_def_step(unsigned long long t){
    uint8_t b[16];
    if (!task_def || !task_token || t < task_step_ms) return;
    if (ramble_function_cancelled(task_def, task_token)){
        char msg[48];
        snprintf(msg, sizeof msg, "stopped at %u/%u", task_done, task_total);
        ramble_function_complete(task_def, task_token, RAMBLE_CALL_CANCELLED, msg, ramble_bytes(NULL, 0));
        printf("calibrate: cancelled at %u/%u\n", task_done, task_total);
        task_token = 0;
        return;
    }
    task_done++;
    ramble_schema_message_default(task_prg_s, b, sizeof b);
    ramble_set_uint(b, sizeof b, task_prg_s, "done", task_done);
    ramble_set_uint(b, sizeof b, task_prg_s, "total", task_total);
    ramble_function_progress(task_def, task_token, ramble_bytes(b, ramble_schema_size(task_prg_s)));
    if (task_done >= task_total){
        ramble_schema_message_default(task_rsp_s, b, sizeof b);
        ramble_set_uint(b, sizeof b, task_rsp_s, "ok", 1);
        ramble_set_uint(b, sizeof b, task_rsp_s, "points", task_total * 360u);
        ramble_function_complete(task_def, task_token, RAMBLE_CALL_OK, NULL,
                               ramble_bytes(b, ramble_schema_size(task_rsp_s)));
        printf("calibrate: done (%u sweeps)\n", task_total);
        task_token = 0;
    }
    task_step_ms = t + 500;
}

static void on_calibrate_rsp(const RambleResponse *r){
    printf("calibrate call: status %d%s%.*s\n", (int)r->status,
           r->message.len ? " " : "", (int)r->message.len,
           r->message.data ? r->message.data : "");
}

/* the periodic caller (perception): one calibrate call every ~10s */
static void task_caller_step(unsigned long long t){
    uint8_t b[8];
    if (!task_remote || t < task_next_call_ms) return;
    ramble_schema_message_default(task_req_s, b, sizeof b);
    ramble_set_uint(b, sizeof b, task_req_s, "sweeps", 14);
    ramble_function_call_async(task_remote, ramble_bytes(b, ramble_schema_size(task_req_s)),
                             on_calibrate_rsp, NULL, NULL);
    task_next_call_ms = t + 10000;
}

static int task_setup(RambleNode *node, const char *profile){
    RambleAllocator *mem = (RambleAllocator *)malloc(sizeof *mem);
    int def = !strcmp(profile, "lidar-driver"), caller = !strcmp(profile, "perception");
    if (!mem || (!def && !caller)){ free(mem); return 1; }
    *mem = ramble_allocator_heap(0);
    task_req_s = ramble_schema_compile(ramble_allocator_alloc, mem, "CalibrateRequest { sweeps: u32 }", NULL);
    task_prg_s = ramble_schema_compile(ramble_allocator_alloc, mem, "CalibrateProgress { done: u32, total: u32 }", NULL);
    task_rsp_s = ramble_schema_compile(ramble_allocator_alloc, mem, "CalibrateResult { ok: bool, points: u32 }", NULL);
    if (!task_req_s || !task_prg_s || !task_rsp_s) return 0;
    if (def)
        task_def = ramble_node_create_task_definition(node, TASK_NAME, task_req_s, task_prg_s,
                                                    task_rsp_s, on_calibrate, NULL, NULL);
    else
        task_remote = ramble_node_create_remote_task(node, TASK_NAME, task_req_s, task_prg_s,
                                                   task_rsp_s, NULL);
    task_next_call_ms = now_ms() + 3000;   /* first call once matching settles */
    return def ? task_def != NULL : task_remote != NULL;
}

/* the reliable topics, matching data.js. Everything else is best effort. A topic's
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
static RambleNode *open_node(const Profile *p, uint16_t domain, const char *ifc,
                           PubCh *pubs, int *n_pubs){
    RambleAllocator mem = ramble_allocator_heap(0);
    RambleNode *n;
    int i;
    *n_pubs = 0;
    n = ramble_node_open(&mem, p->name, NULL, NULL, &(RambleNodeOpts){
        .domain = domain, .max_topics = 16,
        .net = { .multicast_interface = ifc } });
    if (!n){ fprintf(stderr, "  open %s failed\n", p->name); return NULL; }
    for (i = 0; p->pubs && p->pubs[i]; i++){
        RambleTopic *ch = ramble_node_create_topic(n, p->pubs[i], RAMBLE_PUB_ONLY, NULL, &(RambleTopicOpts){
            .qos = { .reliability = reliable(p->pubs[i]) ? RAMBLE_RELIABLE : RAMBLE_BEST_EFFORT,
                     .keep_last = 16 } });
        if (ch && *n_pubs < MAX_PUBS){
            PubCh *pc = &pubs[(*n_pubs)++];
            pc->ch = ch; pc->topic = p->pubs[i];
            pc->period_ms = topic_period_ms(p->pubs[i]); pc->seq = 0; pc->next_ms = 0;
        }
    }
    for (i = 0; p->subs && p->subs[i]; i++)
        ramble_node_create_topic(n, p->subs[i], RAMBLE_SUB_ONLY, NULL, &(RambleTopicOpts){
            .qos = { .reliability = reliable(p->subs[i]) ? RAMBLE_RELIABLE : RAMBLE_BEST_EFFORT } });
    return n;
}

int main(int argc, char **argv){
    uint16_t domain = 0;             /* Ramble default domain (matches the explorer's default) */
    const char *ifc = "127.0.0.1";   /* single-host loopback, matching the default observer */
    const char *want = NULL;         /* which profile to run */
    const Profile *prof = NULL;
    RambleNode *node;
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
    if (!task_setup(node, prof->name)){ fprintf(stderr, "  task setup failed\n"); return 1; }
    printf("%s: up on domain %u (interface %s), publishing %d topic(s) -- Ctrl-C to stop\n",
           prof->name, domain, ifc, n_pubs);

    start_ms = now_ms();
    while (g_run){
        unsigned long long t = now_ms();
        task_def_step(t);      /* lidar-driver: run the calibration job */
        task_caller_step(t);   /* perception: call it every ~10s */
        for (i = 0; i < n_pubs; i++){
            if (t >= pubs[i].next_ms){
                char buf[120];
                int len = snprintf(buf, sizeof buf, "%s #%u from %s @ %.2fs",
                                   pubs[i].topic, pubs[i].seq, prof->name,
                                   (double)(t - start_ms) / 1000.0);
                if (len < 0) len = 0;
                if (len > (int)sizeof buf) len = (int)sizeof buf;
                ramble_topic_send(pubs[i].ch, ramble_bytes(buf, (size_t)len), NULL);
                pubs[i].seq++;
                pubs[i].next_ms = t + pubs[i].period_ms;
            }
        }
        ramble_node_poll(node, 10);
    }

    ramble_node_close(node, 1);
    return 0;
}
