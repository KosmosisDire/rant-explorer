/* DART Explorer: a debugger UI for a DART mesh (Discovery And Realtime
   Transport). Three tabs: Nodes, Topics, Log.

   This translation unit is the GUI: SDL3 for the window/input/renderer, SDL3_ttf
   (FreeType) for text, Clay for declarative (flexbox-style) layout. The UI is
   broken out into ui_*.h (theme, data model, widgets, per-tab views, shell). The
   live discovery observer lives in net_capture.c, a SEPARATE TU (it owns DART +
   winsock); keeping the split isolates that side from the GUI.

   The new UI renders from an (initially empty) ui_model Dataset; feeding it from
   net_capture's live peers is the next step.

   Build (cross-platform) with CMake; see CMakeLists.txt:
     cmake -S explore -B explore/build && cmake --build explore/build */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#define CLAY_IMPLEMENTATION
#include "clay.h"

#define SDL_MAIN_HANDLED                 /* we own main(); don't let SDL_main.h rename it */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>               /* for SDL_SetMainReady() */
#include <SDL3_ttf/SDL_ttf.h>
#include "ui_render.h"                   /* vendored Clay->SDL3 renderer (grayscale-AA text) */

/* nanosvg single-headers: implementation lives in this one TU. Used by ui_icons.h
   to rasterize the Lucide SVGs into textures. */
#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

#include "net_capture.h"
#include "ui_theme.h"
#include "ui_fonts.h"
#include "ui_icons.h"
#include "ui_model.h"
#include "ui_data.h"
#include "ui_widgets.h"
#include "ui_textbox.h"
#include "ui_app.h"
#include "ui_tree.h"
#include "ui_tab_nodes.h"
#include "ui_tab_topics.h"
#include "ui_tab_log.h"
#include "ui_shell.h"

/* mouse-wheel scroll speed: pixels (at 1x DPI) per wheel notch, scaled by UISC */
#define UI_SCROLL_SPEED 8

/* the dataset the UI draws from, rebuilt each frame from the live capture snapshot
   by ui_data_build(). g_snap is the plain-types view copied out of net_capture. */
static Dataset     g_data;
static CapSnapshot g_snap;

static void clay_error(Clay_ErrorData e){
    fprintf(stderr, "clay error: %.*s\n", (int)e.errorText.length, e.errorText.chars);
}

int main(int argc, char **argv){
    Config  cfg;
    Capture cap;
    AppState app;
    SDL_Window   *win;
    SDL_Renderer *ren;
    UiRenderer rdata;
    float dpi;
    uint64_t clay_mem, last_ticks;
    Clay_Arena arena;
    int ow = 0, oh = 0;
    bool mouse_held = false, debug_enabled = false;

    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: console echo stays live */

    cap_defaults(&cfg);
    {   int parsed = cap_parse_args(argc, argv, &cfg);
        if (parsed <= 0) return parsed < 0 ? 1 : 0;
    }
    printf("DART Explorer\n  observer \"%s\"  domain %u  group %s:%u  interface %s\n",
           cfg.name, cfg.domain, cfg.group, cfg.port, cfg.ifc ? cfg.ifc : "(auto)");
    printf("  hotkey: F12 = toggle the Clay layout inspector\n");

    SDL_SetMainReady();
    if (!SDL_Init(SDL_INIT_VIDEO)){
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    if (!TTF_Init()){
        fprintf(stderr, "TTF_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    if (!SDL_CreateWindowAndRenderer("DART Explorer", 1280, 800,
            SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY, &win, &ren)){
        fprintf(stderr, "SDL_CreateWindowAndRenderer failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(ren, 1);   /* the node's service thread owns the poll now, so the
                                     render loop no longer drives it: pace to the display */
    SDL_StartTextInput(win);   /* deliver SDL_EVENT_TEXT_INPUT for the text boxes */

    dpi = SDL_GetWindowPixelDensity(win);   /* physical px per logical px (1.0 = 100%) */
    if (dpi <= 0.0f) dpi = 1.0f;
    {   const char *z = getenv("DART_UI_ZOOM");   /* optional extra zoom on the CSS px */
        ui_scale = z ? (float)atof(z) : 1.0f;
        if (ui_scale < 0.5f) ui_scale = 0.5f;
        if (ui_scale > 4.0f) ui_scale = 4.0f;
    }
    printf("  display pixel density %.2f, UI zoom %.2f (override with DART_UI_ZOOM)\n",
           (double)dpi, (double)ui_scale);

    if (!ui_fonts_load(dpi)){
        fprintf(stderr, "failed to load system fonts (Segoe UI / Consolas; DejaVu on Linux)\n");
        return 1;
    }
    if (!ui_icons_load(ren))
        fprintf(stderr, "warning: some Lucide icons failed to rasterize (UI runs without them)\n");
    rdata = (UiRenderer){ .renderer = ren, .fonts = g_fonts };

    SDL_GetCurrentRenderOutputSize(ren, &ow, &oh);
    Clay_SetMaxElementCount(1024 * 1024);   /* default 64k is too small for the UI's flexbox tree */
    clay_mem = Clay_MinMemorySize();
    arena = Clay_CreateArenaWithCapacityAndMemory(clay_mem, malloc(clay_mem));
    Clay_Initialize(arena, (Clay_Dimensions){ (float)ow, (float)oh },
                    (Clay_ErrorHandler){ clay_error, 0 });
    Clay_SetMeasureTextFunction(ui_measure_text, g_fonts);

    app_init(&app, &g_data);
    app.snap = &g_snap;          /* stable global; the Log tab reads its event lines */
    {   const char *tab = getenv("DART_UI_TAB");   /* optional: open straight on a tab */
        if (tab){ if (!strcmp(tab, "topics")) app.tab = TAB_TOPICS;
                  else if (!strcmp(tab, "log")) app.tab = TAB_LOG;
                  else if (!strcmp(tab, "nodes")) app.tab = TAB_NODES; }
    }
    static char auto_sub[CAP_TOPIC_CAP];           /* DART_UI_TOPIC: also subscribe once it appears */
    {   const char *tp = getenv("DART_UI_TOPIC");  /* optional: select a topic (+drawer) once it appears */
        if (tp && *tp){ snprintf(app.select_topic, sizeof app.select_topic, "%s", tp); app.drawer_open = 1;
                        snprintf(auto_sub, sizeof auto_sub, "%s", tp); }
    }

    if (!cap_start(&cap, &cfg))
        fprintf(stderr, "running without live discovery (socket/interface issue)\n");
    app.cap = &cap;          /* lets the Topics tab subscribe and read the live feed */

    last_ticks = SDL_GetTicks();
    /* optional readout (set DART_UI_FPS=1): the service thread owns the poll, so this fps is
       just the (vsync-paced) render rate; render-ms is the per-frame layout+text work. */
    int      fps_show = getenv("DART_UI_FPS") != NULL;
    uint64_t fps_t0 = last_ticks; int fps_frames = 0; double fps_render_ms = 0.0;
    for (;;){
        SDL_Event ev;
        float wheel_x = 0.0f, wheel_y = 0.0f;
        bool  quit = false;
        float mx, my, dt;
        uint64_t now;
        float cur_dpi;
        Clay_Color bg;

        g_pointer_pressed = false;   /* edge-triggered: set only on a press this frame */
        g_right_pressed   = false;
        g_pointer_pressed_raw = false;   /* the unconsumed twins (menu dismissal) */
        g_right_pressed_raw   = false;
        ui_tb_events_reset();        /* text-box key/text queue is per frame */
        while (SDL_PollEvent(&ev)){
            switch (ev.type){
                case SDL_EVENT_QUIT: quit = true; break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    if (ev.button.button == SDL_BUTTON_LEFT){
                        mouse_held = true; g_pointer_pressed = true; g_pointer_pressed_raw = true;
                        ui_tb_click_count(ev.button.clicks);   /* double/triple-click selection */
                    } else if (ev.button.button == SDL_BUTTON_RIGHT){
                        g_right_pressed = true; g_right_pressed_raw = true;
                    }
                    break;
                case SDL_EVENT_MOUSE_BUTTON_UP:
                    if (ev.button.button == SDL_BUTTON_LEFT) mouse_held = false;
                    break;
                case SDL_EVENT_MOUSE_WHEEL:
                    wheel_x += ev.wheel.x; wheel_y += ev.wheel.y;
                    break;
                case SDL_EVENT_KEY_DOWN:
                    if (ev.key.key == SDLK_F12){              /* F12: toggle Clay's layout inspector */
                        if (!ev.key.repeat){ debug_enabled = !debug_enabled; Clay_SetDebugModeEnabled(debug_enabled); }
                        break;
                    }
                    ui_tb_feed_key(ev.key.key, ev.key.mod);   /* the focused text box consumes these */
                    break;
                case SDL_EVENT_TEXT_INPUT:                    /* typed characters -> the focused text box */
                    if (ev.text.text) ui_tb_feed_text(ev.text.text);
                    break;
                default: break;
            }
        }
        if (quit) break;

        cap_poll(&cap);

        cur_dpi = SDL_GetWindowPixelDensity(win);
        if (cur_dpi > 0.0f && fabsf(cur_dpi - g_atlas_scale) > 0.01f){   /* moved to a different-DPI monitor */
            ui_fonts_reload(cur_dpi);   /* sets ui_dpi first... */
            ui_icons_reload(ren);       /* ...so the icon raster picks up the new density */
        }

        SDL_GetMouseState(&mx, &my);     /* logical (point) coords; layout is physical */
        mx *= ui_dpi; my *= ui_dpi;
        SDL_GetCurrentRenderOutputSize(ren, &ow, &oh);
        g_pointer_x = mx; g_pointer_y = my;              /* for cursor-anchored context menus */
        g_view_w = (float)ow; g_view_h = (float)oh;      /* for clamping menus to the window */

        now = SDL_GetTicks();
        dt = (float)(now - last_ticks) / 1000.0f;
        last_ticks = now;
        if (dt <= 0.0f) dt = 1.0f / 60.0f;

        {   /* wheel over a scrolling text box goes to the box, not the Clay containers */
            float tb_wheel = 0.0f;
            if (wheel_y != 0.0f && ui_tb_wheel_hit(mx, my)){ tb_wheel = wheel_y; wheel_x = wheel_y = 0.0f; }
            ui_tb_frame(mouse_held, tb_wheel);
        }
        Clay_SetLayoutDimensions((Clay_Dimensions){ (float)ow, (float)oh });
        Clay_SetPointerState((Clay_Vector2){ mx, my }, mouse_held);
        Clay_UpdateScrollContainers(true, (Clay_Vector2){ wheel_x * UISC(UI_SCROLL_SPEED),
                                                          wheel_y * UISC(UI_SCROLL_SPEED) }, dt);

        cap_snapshot(&cap, &g_snap);     /* live discovery table -> plain view */
        ui_data_build(&g_data, &g_snap); /* -> the UI Dataset (rebuilt every frame) */
        if (app.select_topic[0]){        /* a just-added topic: select it once it appears */
            int ti;
            for (ti = 0; ti < g_data.n_topics; ti++)
                if (!strcmp(g_data.topics[ti].path, app.select_topic)){ app.sel_topic = ti; app.select_topic[0] = '\0'; break; }
        }
        if (auto_sub[0]){                /* the deep-linked topic: subscribe to it once it appears */
            int ti;
            for (ti = 0; ti < g_data.n_topics; ti++)
                if (!strcmp(g_data.topics[ti].path, auto_sub)){
                    cap_subscribe(&cap, auto_sub, g_data.topics[ti].reliable_recommend > 0);
                    auto_sub[0] = '\0';
                    break;
                }
        }
        if (app.sel_node >= g_data.n_nodes)
            app.sel_node = g_data.n_nodes ? g_data.n_nodes - 1 : 0;
        if (app.sel_topic >= g_data.n_topics)
            app.sel_topic = g_data.n_topics ? g_data.n_topics - 1 : 0;

        g_now_ms = (uint32_t)now;   /* caret blink, undo coalescing, transient UI feedback */
        ui_strpool_reset();
        uint64_t r0 = SDL_GetPerformanceCounter();
        Clay_BeginLayout();
        ui_frame(&app);
        Clay_RenderCommandArray cmds = Clay_EndLayout();
        topics_feed_autoscroll(&app);    /* pin the feed to the newest message (uses final layout) */

        bg = app.theme_dark ? UI_DARK.bg : UI_LIGHT.bg;
        SDL_SetRenderDrawColor(ren, (Uint8)bg.r, (Uint8)bg.g, (Uint8)bg.b, 255);
        SDL_RenderClear(ren);
        ui_render(&rdata, &cmds);
        uint64_t r1 = SDL_GetPerformanceCounter();   /* before present: render WORK, not vsync wait */
        SDL_RenderPresent(ren);

        if (fps_show){
            fps_frames++;
            fps_render_ms += (double)(r1 - r0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
            if (now - fps_t0 >= 1000){
                unsigned msgs = 0; int si;   /* live-feed intake across all subscriptions */
                for (si = 0; si < g_snap.n_subs; si++) msgs += g_snap.subs[si].n_msgs;
                printf("[fps] %d fps  (render %.1f ms/frame work, tab=%d, nodes=%d, msgs=%u)\n",
                       fps_frames, fps_render_ms / fps_frames, app.tab, g_data.n_nodes, msgs);
                fps_t0 = now; fps_frames = 0; fps_render_ms = 0.0;
            }
        }
    }

    cap_stop(&cap);
    ui_icons_unload();
    ui_fonts_unload();
    TTF_Quit();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    free(arena.memory);
    return 0;
}
