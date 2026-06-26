/* The single app-state struct. Every widget is re-emitted from this each frame;
   Clay is immediate-mode and holds no retained state of ours. */
#ifndef UI_APP_H
#define UI_APP_H

typedef enum { TAB_NODES, TAB_TOPICS, TAB_LOG } Tab;
typedef enum { DRW_INSPECT, DRW_PUBLISH } DrawerMode;

typedef struct {
    int  theme_dark;          /* 1 dark, 0 light */
    Tab  tab;

    int  sel_node;            /* index into data->nodes  */
    int  sel_topic;           /* index into data->topics */

    int        drawer_open;   /* Topics inspector drawer */
    DrawerMode drawer_mode;

    const Dataset *data;
} AppState;

static void app_init(AppState *a, const Dataset *data){
    a->theme_dark  = 1;
    a->tab         = TAB_TOPICS;
    a->sel_node    = 0;
    a->sel_topic   = 0;
    a->drawer_open = 1;
    a->drawer_mode = DRW_INSPECT;
    a->data        = data;
}

#endif /* UI_APP_H */
