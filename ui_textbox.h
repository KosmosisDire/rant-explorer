/* A general text box: retained per-field state (UiTbState) + an immediate-mode
   draw call (ui_textbox), built on Clay + SDL3_ttf.

   Feature set: caret + selection (mouse drag, double-click word, triple-click
   line, shift+click, shift+arrows/Home/End/Page), clipboard (ctrl+C/X/V plus
   shift+Del, ctrl+Ins, shift+Ins, and a right-click menu), word hops
   (ctrl+arrows) and word deletes (ctrl+Backspace/Delete), line and document
   Home/End, PageUp/Down, ctrl+A, undo/redo (ctrl+Z, ctrl+Y, ctrl+shift+Z),
   UTF-8-safe editing throughout. On macOS the GUI key stands in for ctrl.

   Sizing: fill_w fills the parent, else the box FITS its text between w_min and
   w_max. Text wider than the box scrolls horizontally under the caret (wrap=0)
   or wraps at the box width (wrap=1). A multiline box grows with its lines from
   h_min to h_max, then scrolls vertically (mouse wheel included). bare=1 strips
   the chrome (background/border/padding) so a caller's styled row can host it.

   Plumbing: the SDL event loop calls ui_tb_events_reset() before polling and
   feeds ui_tb_feed_key / ui_tb_feed_text / ui_tb_click_count; once the pointer
   state is known, main routes the wheel (ui_tb_wheel_hit) and calls
   ui_tb_frame(). The focused box consumes the queue when its ui_textbox call
   draws; unconsumed keys stay available to callers via ui_tb_take_key. Focus is
   one global slot: clicking a box takes it, clicking anywhere else drops it,
   ui_tb_focus / ui_tb_blur move it by hand. The caller owns the byte buffer
   (NUL-terminated; len maintained through the in/out param) and it must outlive
   the frame (Clay stores string views into it).
   Requires clay.h, SDL3, SDL3_ttf, ui_theme.h, ui_fonts.h, ui_widgets.h. */
#ifndef UI_TEXTBOX_H
#define UI_TEXTBOX_H

#define UI_TB_EVQ        64      /* key/text events buffered per frame */
#define UI_TB_UNDO_MAX   48      /* history snapshots per stack */
#define UI_TB_UNDO_BYTES 16384   /* snapshot text arena per stack */
#define UI_TB_MAX_LINES  2048    /* per-draw line scratch (one box draws at a time) */

/* action bits returned by ui_textbox */
enum {
    UI_TB_CHANGED = 1 << 0,   /* the buffer was edited this frame */
    UI_TB_SUBMIT  = 1 << 1,   /* Enter (single-line, or enter_submits multiline) */
    UI_TB_CANCEL  = 1 << 2,   /* Escape (the caller decides: clear, close, ...) */
    UI_TB_TAB     = 1 << 3,   /* Tab (unless tab_inserts) */
    UI_TB_BACKTAB = 1 << 4,   /* shift+Tab */
};

typedef struct {
    Clay_String placeholder;  /* faint hint while the buffer is empty */
    FontFamily fam; FontWeight wt; FontSize sz;
    int   multiline;          /* newlines allowed */
    int   wrap;               /* wrap at the box width instead of scrolling horizontally */
    int   enter_submits;      /* multiline: Enter = SUBMIT, shift+Enter = newline */
    int   tab_inserts;        /* Tab types spaces instead of returning UI_TB_TAB */
    int   bare;               /* no chrome: the caller's container is the visual box */
    int   read_only;          /* caret/selection/copy work, edits are refused */
    int   fill_w;             /* width fills the parent; else FIT the text in w_min..w_max */
    float w_min, w_max;       /* css px (fit mode); w_max 0 = unbounded */
    float h_min, h_max;       /* css px; multiline grows h_min..h_max then scrolls (0 = one line) */
    float pad_x, pad_y;       /* css px inner padding (chrome mode) */
    float radius;             /* css px corner radius (chrome mode) */
    Clay_Color bg, border, border_focus;   /* chrome colors; alpha 0 = palette defaults */
} UiTextBoxOpts;

/* undo/redo: two snapshot stacks in fixed arenas, allocated on first edit.
   Oldest snapshots fall off the front when an arena fills. */
typedef struct { int off, len, caret, anchor; } UiTbSnap;
typedef struct {
    UiTbSnap past[UI_TB_UNDO_MAX], redo[UI_TB_UNDO_MAX];
    int  n_past, n_redo, past_bytes, redo_bytes;
    char pbuf[UI_TB_UNDO_BYTES], rbuf[UI_TB_UNDO_BYTES];
} UiTbUndo;

typedef struct {
    int   caret, anchor;       /* byte offsets; equal = no selection */
    int   last_caret;          /* caret after the last edit (undo run coalescing) */
    float pref_x;              /* remembered x for up/down runs (<0 = derive from caret) */
    float scroll_x, scroll_y;  /* content px scrolled off the top-left */
    int   drag;                /* live mouse selection: 0 no, 1 char, 2 word, 3 line */
    int   drag_a, drag_b;      /* the press's word/line span (drag extends around it) */
    int   ensure;              /* scroll the caret into view on this draw */
    char  last_op;             /* undo coalescing: last edit kind... */
    uint32_t last_ms;          /* ...and when it happened */
    uint32_t blink_t0;         /* caret blink phase anchor */
    UiTbUndo *undo;            /* lazily allocated, app-lifetime */
} UiTbState;

/* ------------------------------------------------------------ frame plumbing */

typedef struct { SDL_Keycode key; SDL_Keymod mod; char text[24]; unsigned char used; } UiTbEv;
static UiTbEv g_tb_ev[UI_TB_EVQ];       /* key 0 = a typed-text entry */
static int    g_tb_nev    = 0;
static int    g_tb_clicks = 1;          /* click count of this frame's press */
static bool   g_tb_held   = false;      /* left button currently down */
static float  g_tb_wheel  = 0.0f;       /* wheel notches routed to text boxes this frame */
static UiTbState *g_tb_focus = NULL;    /* the one focused box (NULL = none) */
static struct { float x, y, w, h; } g_tb_scrollable[16];   /* boxes with vertical overflow */
static int    g_tb_n_scrollable = 0;    /* (last frame's set until ui_tb_frame resets it) */

static void ui_tb_events_reset(void){ g_tb_nev = 0; }

static void ui_tb_feed_key(SDL_Keycode k, SDL_Keymod m){
    if (g_tb_nev < UI_TB_EVQ){
        g_tb_ev[g_tb_nev].key = k; g_tb_ev[g_tb_nev].mod = m;
        g_tb_ev[g_tb_nev].text[0] = '\0'; g_tb_ev[g_tb_nev].used = 0;
        g_tb_nev++;
    }
}

static void ui_tb_feed_text(const char *t){
    while (t && *t && g_tb_nev < UI_TB_EVQ){
        UiTbEv *e = &g_tb_ev[g_tb_nev++];
        size_t n = strlen(t), take = n < sizeof e->text - 1 ? n : sizeof e->text - 1;
        while (take > 0 && take < n && ((unsigned char)t[take] & 0xC0) == 0x80) take--;  /* char boundary */
        if (take == 0) take = 1;
        memcpy(e->text, t, take); e->text[take] = '\0';
        e->key = 0; e->mod = 0; e->used = 0;
        t += take;
    }
}

static void ui_tb_click_count(int clicks){ g_tb_clicks = clicks > 0 ? clicks : 1; }

/* per frame, after the pointer state is known and the wheel has been routed */
static void ui_tb_frame(bool held, float wheel){
    g_tb_held = held;
    g_tb_wheel = wheel;
    g_tb_n_scrollable = 0;   /* refilled as the boxes draw */
}

/* is the pointer over a box that scrolls vertically? (main then gives it the
   wheel instead of the Clay scroll containers; uses last frame's set) */
static int ui_tb_wheel_hit(float x, float y){
    int i;
    for (i = 0; i < g_tb_n_scrollable; i++)
        if (x >= g_tb_scrollable[i].x && x < g_tb_scrollable[i].x + g_tb_scrollable[i].w &&
            y >= g_tb_scrollable[i].y && y < g_tb_scrollable[i].y + g_tb_scrollable[i].h)
            return 1;
    return 0;
}

static int  ui_tb_focused(const UiTbState *st){ return g_tb_focus == st; }
static void ui_tb_focus(UiTbState *st){
    if (g_tb_focus != st){ g_tb_focus = st; st->blink_t0 = g_now_ms; st->last_op = 0; }
}
static void ui_tb_blur(UiTbState *st){
    if (g_tb_focus == st) g_tb_focus = NULL;
    if (ui_menu_is_open(st)) ui_menu_close();
    st->drag = 0;
}
static void ui_tb_blur_all(void){ if (g_tb_focus) ui_tb_blur(g_tb_focus); }

/* after the caller rewrote the buffer under the box (resets, prefills) */
static void ui_tb_reset(UiTbState *st){
    st->caret = st->anchor = st->last_caret = 0;
    st->pref_x = -1.0f;
    st->scroll_x = st->scroll_y = 0.0f;
    st->drag = 0; st->ensure = 0; st->last_op = 0;
    if (st->undo){
        st->undo->n_past = st->undo->n_redo = 0;
        st->undo->past_bytes = st->undo->redo_bytes = 0;
    }
}

/* a key the focused box didn't consume this frame (bool toggles, tab cycling) */
static int ui_tb_take_key(SDL_Keycode key, SDL_Keymod need){
    int i;
    for (i = 0; i < g_tb_nev; i++)
        if (!g_tb_ev[i].used && g_tb_ev[i].key == key && (g_tb_ev[i].mod & need) == need){
            g_tb_ev[i].used = 1;
            return 1;
        }
    return 0;
}

/* the platform's shortcut modifier; AltGr (ctrl+alt) types text, not shortcuts */
static int ui_tb_prim(SDL_Keymod m){
#ifdef __APPLE__
    return (m & SDL_KMOD_GUI) != 0;
#else
    return (m & SDL_KMOD_CTRL) != 0 && (m & SDL_KMOD_ALT) == 0;
#endif
}

/* ------------------------------------------------------------ text utilities */

static int ui_tb_prev(const char *s, int i){
    if (i > 0){ i--; while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--; }
    return i;
}
static int ui_tb_next(const char *s, int n, int i){
    if (i < n){ i++; while (i < n && ((unsigned char)s[i] & 0xC0) == 0x80) i++; }
    return i;
}

/* char class for word ops: 0 space, 1 word (alnum/_/non-ASCII), 2 punct, 3 newline */
static int ui_tb_cls(unsigned char c){
    if (c == ' ' || c == '\t') return 0;
    if (c == '\n') return 3;
    if (c == '_' || c >= 0x80 || (c >= '0' && c <= '9') ||
        (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return 1;
    return 2;
}

static int ui_tb_prev_word(const char *s, int i){
    while (i > 0){ int p = ui_tb_prev(s, i); if (ui_tb_cls((unsigned char)s[p]) != 0) break; i = p; }
    if (i > 0){
        int p = ui_tb_prev(s, i), cls = ui_tb_cls((unsigned char)s[p]);
        while (i > 0){ p = ui_tb_prev(s, i); if (ui_tb_cls((unsigned char)s[p]) != cls) break; i = p; }
    }
    return i;
}
static int ui_tb_next_word(const char *s, int n, int i){
    if (i < n){
        int cls = ui_tb_cls((unsigned char)s[i]);
        while (i < n && ui_tb_cls((unsigned char)s[i]) == cls) i = ui_tb_next(s, n, i);
    }
    while (i < n && ui_tb_cls((unsigned char)s[i]) == 0) i = ui_tb_next(s, n, i);
    return i;
}

/* the same-class run around `at`, for double-click selection */
static void ui_tb_word_at(const char *s, int n, int at, int *a, int *b){
    int cls;
    if (n == 0){ *a = *b = 0; return; }
    if (at >= n) at = ui_tb_prev(s, n);
    cls = ui_tb_cls((unsigned char)s[at]);
    *a = at;
    while (*a > 0){ int p = ui_tb_prev(s, *a); if (ui_tb_cls((unsigned char)s[p]) != cls) break; *a = p; }
    *b = ui_tb_next(s, n, at);
    while (*b < n && ui_tb_cls((unsigned char)s[*b]) == cls) *b = ui_tb_next(s, n, *b);
}

/* ------------------------------------------------------------ undo machinery */

static void ui_tb_stack_push(UiTbSnap *snaps, int *n, int *bytes, char *arena,
                             const char *txt, int len, int caret, int anchor){
    if (len > UI_TB_UNDO_BYTES){ *n = 0; *bytes = 0; return; }   /* too big to snapshot */
    while (*n >= UI_TB_UNDO_MAX || *bytes + len > UI_TB_UNDO_BYTES){
        int drop = snaps[0].len, i;                              /* shed the oldest */
        memmove(arena, arena + drop, (size_t)(*bytes - drop));
        for (i = 1; i < *n; i++){ snaps[i - 1] = snaps[i]; snaps[i - 1].off -= drop; }
        (*n)--; *bytes -= drop;
    }
    snaps[*n].off = *bytes; snaps[*n].len = len;
    snaps[*n].caret = caret; snaps[*n].anchor = anchor;
    memcpy(arena + *bytes, txt, (size_t)len);
    *bytes += len; (*n)++;
}

static void ui_tb_stack_pop(UiTbSnap *snaps, int *n, int *bytes, const char *arena,
                            char *buf, int cap, int *len, int *caret, int *anchor){
    UiTbSnap s = snaps[--(*n)];
    int m = s.len < cap - 1 ? s.len : cap - 1;
    *bytes -= s.len;
    memcpy(buf, arena + s.off, (size_t)m); buf[m] = '\0'; *len = m;
    *caret  = s.caret  <= m ? s.caret  : m;
    *anchor = s.anchor <= m ? s.anchor : m;
}

/* snapshot the pre-edit state. Same-kind edits at the caret within 900 ms
   coalesce into one undo step ('t' typing, 'b' backspace, 'd' delete; 'p'
   paste/cut never coalesces). */
static void ui_tb_record(UiTbState *st, const char *buf, int len, char op){
    UiTbUndo *u = st->undo;
    int coalesce;
    if (!u){ u = st->undo = (UiTbUndo *)calloc(1, sizeof *u); if (!u) return; }
    coalesce = op != 'p' && op == st->last_op &&
               (uint32_t)(g_now_ms - st->last_ms) < 900 &&
               st->caret == st->anchor && st->caret == st->last_caret;
    if (!coalesce)
        ui_tb_stack_push(u->past, &u->n_past, &u->past_bytes, u->pbuf, buf, len, st->caret, st->anchor);
    u->n_redo = 0; u->redo_bytes = 0;
    st->last_op = op; st->last_ms = g_now_ms;
}

static int ui_tb_undo_redo(UiTbState *st, char *buf, int cap, int *len, int redo){
    UiTbUndo *u = st->undo;
    if (!u) return 0;
    if (!redo){
        if (!u->n_past) return 0;
        ui_tb_stack_push(u->redo, &u->n_redo, &u->redo_bytes, u->rbuf, buf, *len, st->caret, st->anchor);
        ui_tb_stack_pop(u->past, &u->n_past, &u->past_bytes, u->pbuf, buf, cap, len, &st->caret, &st->anchor);
    } else {
        if (!u->n_redo) return 0;
        ui_tb_stack_push(u->past, &u->n_past, &u->past_bytes, u->pbuf, buf, *len, st->caret, st->anchor);
        ui_tb_stack_pop(u->redo, &u->n_redo, &u->redo_bytes, u->rbuf, buf, cap, len, &st->caret, &st->anchor);
    }
    st->last_op = 0;
    return 1;
}

/* ------------------------------------------------------------ edit primitives */

static void ui_tb_del_range(UiTbState *st, char *buf, int *len, int a, int b){
    memmove(buf + a, buf + b, (size_t)(*len - b));
    *len -= b - a; buf[*len] = '\0';
    st->caret = st->anchor = a;
}

/* filter + splice text in at the caret (any selection already removed). \r is
   dropped, \t becomes a space, \n survives only in multiline, other control
   bytes are dropped. Clamps to cap-1 at a UTF-8 boundary. */
static int ui_tb_do_insert(UiTbState *st, char *buf, int cap, int *len,
                           const char *src, int n, int multiline){
    char sb[128], *tmp = sb;
    int m = 0, i, room;
    if (n > (int)sizeof sb){ tmp = (char *)malloc((size_t)n); if (!tmp) return 0; }
    for (i = 0; i < n; i++){
        unsigned char c = (unsigned char)src[i];
        if (c == '\r') continue;
        if (c == '\t') c = ' ';
        if (c == '\n' && !multiline) c = ' ';
        if (c < 0x20 && c != '\n') continue;
        tmp[m++] = (char)c;
    }
    room = cap - 1 - *len;
    if (m > room){
        m = room > 0 ? room : 0;
        while (m > 0 && ((unsigned char)tmp[m] & 0xC0) == 0x80) m--;   /* keep whole chars */
    }
    if (m > 0){
        memmove(buf + st->caret + m, buf + st->caret, (size_t)(*len - st->caret));
        memcpy(buf + st->caret, tmp, (size_t)m);
        *len += m; buf[*len] = '\0';
        st->caret += m; st->anchor = st->caret;
    }
    if (tmp != sb) free(tmp);
    return m;
}

static void ui_tb_copy_range(const char *buf, int a, int b){
    char *t;
    if (b <= a) return;
    t = (char *)malloc((size_t)(b - a) + 1);
    if (!t) return;
    memcpy(t, buf + a, (size_t)(b - a)); t[b - a] = '\0';
    SDL_SetClipboardText(t);
    free(t);
}

/* shared by ctrl+V / shift+Ins / the context menu; sel already resolved */
static int ui_tb_do_paste(UiTbState *st, char *buf, int cap, int *len, int s0, int s1, int multiline){
    char *ct = SDL_GetClipboardText();
    int did = 0;
    if (ct && *ct){
        ui_tb_record(st, buf, *len, 'p');
        if (s1 > s0) ui_tb_del_range(st, buf, len, s0, s1);
        ui_tb_do_insert(st, buf, cap, len, ct, (int)strlen(ct), multiline);
        did = 1;
    }
    if (ct) SDL_free(ct);
    return did;
}

/* ------------------------------------------------------------ line layout */

typedef struct { int start, end; float w; } UiTbLine;   /* [start,end), '\n' excluded */
static UiTbLine g_tb_lines[UI_TB_MAX_LINES];
static int      g_tb_nlines = 0;

static float ui_tb_w(TTF_Font *f, const char *s, int n){
    int w = 0, h = 0;
    if (n > 0) TTF_GetStringSize(f, s, (size_t)n, &w, &h);
    return (float)w;
}

static void ui_tb_layout(TTF_Font *f, const char *buf, int len, int wrap, float wrap_w){
    int pos = 0;
    g_tb_nlines = 0;
    if (wrap_w < UISC(24)) wrap_w = UISC(24);
    for (;;){
        int eol = pos, start;
        while (eol < len && buf[eol] != '\n') eol++;
        start = pos;
        do {
            int end = eol;
            if (wrap && end > start){
                int seg_w = 0; size_t fit = 0;
                TTF_MeasureString(f, buf + start, (size_t)(end - start), (int)wrap_w, &seg_w, &fit);
                if ((int)fit < end - start){
                    int br = start + (int)fit, sp;
                    while (br > start && ((unsigned char)buf[br] & 0xC0) == 0x80) br--;
                    sp = br;                                   /* prefer the last space break */
                    while (sp > start && buf[sp - 1] != ' ') sp--;
                    if (sp > start) br = sp;
                    if (br <= start) br = ui_tb_next(buf, end, start);   /* one char minimum */
                    end = br;
                }
            }
            if (g_tb_nlines >= UI_TB_MAX_LINES){               /* scratch full: lump the rest */
                g_tb_lines[UI_TB_MAX_LINES - 1].end = len;
                return;
            }
            g_tb_lines[g_tb_nlines].start = start;
            g_tb_lines[g_tb_nlines].end   = end;
            g_tb_lines[g_tb_nlines].w     = ui_tb_w(f, buf + start, end - start);
            g_tb_nlines++;
            start = end;
        } while (start < eol);
        if (eol >= len) break;
        pos = eol + 1;                                         /* past the newline */
    }
}

static int ui_tb_line_of(int caret){
    int i;
    for (i = 0; i < g_tb_nlines - 1; i++)
        if (caret <= g_tb_lines[i].end) return i;
    return g_tb_nlines - 1;
}

/* nearest char boundary to x within line li */
static int ui_tb_hit_line(TTF_Font *f, const char *buf, int li, float x){
    UiTbLine L = g_tb_lines[li];
    int a, b, mw = 0; size_t fit = 0;
    if (x <= 0.0f || L.end <= L.start) return L.start;
    TTF_MeasureString(f, buf + L.start, (size_t)(L.end - L.start), (int)x, &mw, &fit);
    a = L.start + (int)fit;
    while (a > L.start && ((unsigned char)buf[a] & 0xC0) == 0x80) a--;
    if (a >= L.end) return L.end;
    b = ui_tb_next(buf, L.end, a);
    {   float wa = ui_tb_w(f, buf + L.start, a - L.start);
        float wb = ui_tb_w(f, buf + L.start, b - L.start);
        return (x - wa > (wb - wa) * 0.5f) ? b : a;
    }
}

static int ui_tb_hit(TTF_Font *f, const char *buf, float x, float y, float line_h){
    int li = (int)(y / line_h);
    if (y < 0.0f) li = 0;
    if (li < 0) li = 0;
    if (li >= g_tb_nlines) li = g_tb_nlines - 1;
    return ui_tb_hit_line(f, buf, li, x);
}

/* the box's right-click menu: the generic ui_menu with the clipboard actions.
   Runs BEFORE the box lays out its lines: its actions edit the buffer, and
   Clay keeps views into it, so all edits must land before any text is
   declared. Returns 1 when it edited the buffer. */
static int ui_tb_menu_draw(const Palette *P, UiTbState *st, char *buf, int cap, int *len,
                           const UiTextBoxOpts *o){
    int s0 = st->caret < st->anchor ? st->caret : st->anchor;
    int s1 = st->caret > st->anchor ? st->caret : st->anchor;
    int sel = s1 > s0, changed = 0;
    UiMenuItem items[4] = {
        { CLAY_STRING("Cut"),        CLAY_STRING("Ctrl+X"), 0 },
        { CLAY_STRING("Copy"),       CLAY_STRING("Ctrl+C"), 0 },
        { CLAY_STRING("Paste"),      CLAY_STRING("Ctrl+V"), 0 },
        { CLAY_STRING("Select all"), CLAY_STRING("Ctrl+A"), 0 },
    };
    items[0].enabled = sel && !o->read_only;
    items[1].enabled = sel;
    items[2].enabled = !o->read_only && SDL_HasClipboardText();
    items[3].enabled = *len > 0;
    switch (ui_menu(P, st, items, 4, 160)){
        case 0:                                            /* cut */
            ui_tb_copy_range(buf, s0, s1);
            ui_tb_record(st, buf, *len, 'p');
            ui_tb_del_range(st, buf, len, s0, s1);
            changed = 1; st->ensure = 1;
            break;
        case 1:                                            /* copy */
            ui_tb_copy_range(buf, s0, s1);
            break;
        case 2:                                            /* paste */
            if (ui_tb_do_paste(st, buf, cap, len, s0, s1, o->multiline)) changed = 1;
            st->ensure = 1;
            break;
        case 3:                                            /* select all */
            st->anchor = 0; st->caret = *len;
            break;
        default: break;
    }
    return changed;
}

/* ------------------------------------------------------------ the widget */

static int ui_textbox(const Palette *P, Clay_ElementId id, UiTbState *st,
                      char *buf, int cap, int *io_len, const UiTextBoxOpts *o)
{
    int len = *io_len, act = 0, changed = 0, dirty = 0, focused, i;
    TTF_Font *font = g_fonts[ui_font_id(o->fam, o->wt, o->sz)];
    float line_h, pad_x, pad_y, view_w, view_h, view_h_prev, wrap_w;
    float content_w = 0.0f, content_h, box_w = 0.0f, box_h;
    Clay_ElementData ed = Clay_GetElementData(id);

    if (!font) return 0;
    line_h = (float)TTF_GetFontHeight(font);
    if (line_h <= 0.0f) line_h = UISC(15);
    pad_x = o->bare ? 0.0f : UISC(o->pad_x);
    pad_y = o->bare ? 0.0f : UISC(o->pad_y);

    if (st->caret  < 0 || st->caret  > len) st->caret  = len;   /* buffer changed under us */
    if (st->anchor < 0 || st->anchor > len) st->anchor = len;

    /* last frame's box gives the wrap width and the mouse mapping (absent frame one) */
    view_w = (ed.found ? ed.boundingBox.width
                       : UISC(o->fill_w ? 240.0f : (o->w_min > 0.0f ? o->w_min : 120.0f))) - 2.0f * pad_x;
    if (view_w < UISC(12)) view_w = UISC(12);
    view_h_prev = (ed.found ? ed.boundingBox.height : line_h) - 2.0f * pad_y;
    if (view_h_prev < line_h) view_h_prev = line_h;
    wrap_w = o->wrap ? view_w : 1.0e9f;

    ui_tb_layout(font, buf, len, o->wrap, wrap_w);

    /* ---- mouse: focus, caret placement, selection drags, the context menu ---- */
    {
        int over = ed.found && Clay_PointerOver(id);
        float px = g_pointer_x - (ed.found ? ed.boundingBox.x : 0.0f) - pad_x + st->scroll_x;
        float py = g_pointer_y - (ed.found ? ed.boundingBox.y : 0.0f) - pad_y + st->scroll_y;
        if (g_pointer_pressed){
            int on_menu = ui_menu_is_open(st) && ui_menu_pointer_over();
            if (over){
                int at = ui_tb_hit(font, buf, px, py, line_h);
                ui_tb_focus(st);
                if (g_tb_clicks >= 3){                  /* line, its newline included */
                    int li = ui_tb_line_of(at);
                    st->drag_a = g_tb_lines[li].start;
                    st->drag_b = g_tb_lines[li].end;
                    if (st->drag_b < len && buf[st->drag_b] == '\n') st->drag_b++;
                    st->anchor = st->drag_a; st->caret = st->drag_b; st->drag = 3;
                } else if (g_tb_clicks == 2){           /* word */
                    ui_tb_word_at(buf, len, at, &st->drag_a, &st->drag_b);
                    st->anchor = st->drag_a; st->caret = st->drag_b; st->drag = 2;
                } else {
                    if (SDL_GetModState() & SDL_KMOD_SHIFT) st->caret = at;
                    else st->caret = st->anchor = at;
                    st->drag = 1; st->drag_a = st->drag_b = at;
                }
                st->pref_x = -1.0f; st->ensure = 1; st->blink_t0 = g_now_ms;
                g_pointer_pressed = false;              /* consumed: outer click handlers skip it */
            } else if (ui_tb_focused(st) && !on_menu){  /* a menu-item click must not blur its box */
                ui_tb_blur(st);
            }
        }
        if (g_right_pressed && over){                   /* right click: caret here + menu */
            int s0 = st->caret < st->anchor ? st->caret : st->anchor;
            int s1 = st->caret > st->anchor ? st->caret : st->anchor;
            int at = ui_tb_hit(font, buf, px, py, line_h);
            ui_tb_focus(st);
            if (at < s0 || at > s1 || s0 == s1) st->caret = st->anchor = at;   /* keep a hit selection */
            ui_menu_open(st, g_pointer_x, g_pointer_y);
            st->blink_t0 = g_now_ms;
            g_right_pressed = false;
        }
        if (st->drag && g_tb_held && ui_tb_focused(st)){
            int at = ui_tb_hit(font, buf, px, py, line_h);
            if (st->drag == 1) st->caret = at;
            else {
                int a, b;
                if (st->drag == 2) ui_tb_word_at(buf, len, at, &a, &b);
                else {
                    int li = ui_tb_line_of(at);
                    a = g_tb_lines[li].start; b = g_tb_lines[li].end;
                    if (b < len && buf[b] == '\n') b++;
                }
                if (a < st->drag_a){ st->anchor = st->drag_b; st->caret = a; }
                else               { st->anchor = st->drag_a; st->caret = b; }
            }
            st->ensure = 1; st->blink_t0 = g_now_ms;
        } else if (!g_tb_held) st->drag = 0;
    }
    focused = ui_tb_focused(st);

    /* ---- keyboard: the focused box consumes the frame's key/text queue ---- */
    if (focused) for (i = 0; i < g_tb_nev; i++){
        UiTbEv *e = &g_tb_ev[i];
        int s0 = st->caret < st->anchor ? st->caret : st->anchor;
        int s1 = st->caret > st->anchor ? st->caret : st->anchor;
        int sel = s1 > s0, used = 1, edited = 0;
        if (e->used) continue;
        if (e->key == 0){                                     /* typed text */
            if (!o->read_only && e->text[0]){
                ui_tb_record(st, buf, len, 't');
                if (sel) ui_tb_del_range(st, buf, &len, s0, s1);
                ui_tb_do_insert(st, buf, cap, &len, e->text, (int)strlen(e->text), o->multiline);
                edited = 1;
            }
        } else {
            SDL_Keymod m = e->mod;
            int prim = ui_tb_prim(m), shift = (m & SDL_KMOD_SHIFT) != 0;
            switch (e->key){
            case SDLK_LEFT: case SDLK_RIGHT: {
                int right = e->key == SDLK_RIGHT, to;
                if (prim)               to = right ? ui_tb_next_word(buf, len, st->caret) : ui_tb_prev_word(buf, st->caret);
                else if (sel && !shift) to = right ? s1 : s0;   /* collapse to the edge */
                else                    to = right ? ui_tb_next(buf, len, st->caret) : ui_tb_prev(buf, st->caret);
                st->caret = to;
                if (!shift) st->anchor = to;
                st->pref_x = -1.0f;
            } break;
            case SDLK_UP: case SDLK_DOWN: case SDLK_PAGEUP: case SDLK_PAGEDOWN: {
                int up = e->key == SDLK_UP || e->key == SDLK_PAGEUP;
                if (dirty){ ui_tb_layout(font, buf, len, o->wrap, wrap_w); dirty = 0; }
                if (g_tb_nlines <= 1){                        /* one line: to the ends */
                    st->caret = up ? 0 : len;
                } else {
                    int li = ui_tb_line_of(st->caret);
                    int step = (e->key == SDLK_PAGEUP || e->key == SDLK_PAGEDOWN)
                               ? (int)(view_h_prev / line_h) - 1 : 1;
                    if (step < 1) step = 1;
                    if (st->pref_x < 0.0f)
                        st->pref_x = ui_tb_w(font, buf + g_tb_lines[li].start, st->caret - g_tb_lines[li].start);
                    li += up ? -step : step;
                    if (li < 0) li = 0;
                    if (li >= g_tb_nlines) li = g_tb_nlines - 1;
                    st->caret = ui_tb_hit_line(font, buf, li, st->pref_x);
                }
                if (!shift) st->anchor = st->caret;
            } break;
            case SDLK_HOME: case SDLK_END:
                if (dirty){ ui_tb_layout(font, buf, len, o->wrap, wrap_w); dirty = 0; }
                if (prim) st->caret = e->key == SDLK_HOME ? 0 : len;
                else {
                    int li = ui_tb_line_of(st->caret);
                    st->caret = e->key == SDLK_HOME ? g_tb_lines[li].start : g_tb_lines[li].end;
                }
                if (!shift) st->anchor = st->caret;
                st->pref_x = -1.0f;
                break;
            case SDLK_BACKSPACE:
                if (o->read_only) break;
                if (sel){
                    ui_tb_record(st, buf, len, 'b');
                    ui_tb_del_range(st, buf, &len, s0, s1);
                    edited = 1;
                } else if (st->caret > 0){
                    int to = prim ? ui_tb_prev_word(buf, st->caret) : ui_tb_prev(buf, st->caret);
                    ui_tb_record(st, buf, len, 'b');
                    ui_tb_del_range(st, buf, &len, to, st->caret);
                    edited = 1;
                }
                st->pref_x = -1.0f;
                break;
            case SDLK_DELETE:
                if (o->read_only) break;
                if (shift){                                   /* shift+Del = cut */
                    if (sel){
                        ui_tb_copy_range(buf, s0, s1);
                        ui_tb_record(st, buf, len, 'p');
                        ui_tb_del_range(st, buf, &len, s0, s1);
                        edited = 1;
                    }
                } else if (sel){
                    ui_tb_record(st, buf, len, 'd');
                    ui_tb_del_range(st, buf, &len, s0, s1);
                    edited = 1;
                } else if (st->caret < len){
                    int to = prim ? ui_tb_next_word(buf, len, st->caret) : ui_tb_next(buf, len, st->caret);
                    ui_tb_record(st, buf, len, 'd');
                    ui_tb_del_range(st, buf, &len, st->caret, to);
                    edited = 1;
                }
                st->pref_x = -1.0f;
                break;
            case SDLK_RETURN: case SDLK_KP_ENTER:
                if (o->multiline && !o->read_only && (o->enter_submits ? shift : !prim)){
                    ui_tb_record(st, buf, len, 't');
                    if (sel) ui_tb_del_range(st, buf, &len, s0, s1);
                    ui_tb_do_insert(st, buf, cap, &len, "\n", 1, 1);
                    edited = 1;
                } else act |= UI_TB_SUBMIT;
                st->pref_x = -1.0f;
                break;
            case SDLK_ESCAPE:
                if (ui_menu_is_open(st)) ui_menu_close();
                else act |= UI_TB_CANCEL;
                break;
            case SDLK_TAB:
                if (o->tab_inserts && !o->read_only){
                    ui_tb_record(st, buf, len, 't');
                    if (sel) ui_tb_del_range(st, buf, &len, s0, s1);
                    ui_tb_do_insert(st, buf, cap, &len, "    ", 4, o->multiline);
                    edited = 1;
                } else act |= shift ? UI_TB_BACKTAB : UI_TB_TAB;
                break;
            case SDLK_A:
                if (prim){ st->anchor = 0; st->caret = len; st->pref_x = -1.0f; }
                else used = 0;
                break;
            case SDLK_C:
                if (prim){ if (sel) ui_tb_copy_range(buf, s0, s1); }
                else used = 0;
                break;
            case SDLK_X:
                if (prim){
                    if (sel && !o->read_only){
                        ui_tb_copy_range(buf, s0, s1);
                        ui_tb_record(st, buf, len, 'p');
                        ui_tb_del_range(st, buf, &len, s0, s1);
                        edited = 1;
                    }
                } else used = 0;
                break;
            case SDLK_V:
                if (prim){ if (!o->read_only && ui_tb_do_paste(st, buf, cap, &len, s0, s1, o->multiline)) edited = 1; }
                else used = 0;
                break;
            case SDLK_INSERT:
                if (prim){ if (sel) ui_tb_copy_range(buf, s0, s1); }
                else if (shift){ if (!o->read_only && ui_tb_do_paste(st, buf, cap, &len, s0, s1, o->multiline)) edited = 1; }
                else used = 0;
                break;
            case SDLK_Z:
                if (prim){ if (!o->read_only && ui_tb_undo_redo(st, buf, cap, &len, shift)) edited = 1; }
                else used = 0;
                break;
            case SDLK_Y:
                if (prim){ if (!o->read_only && ui_tb_undo_redo(st, buf, cap, &len, 1)) edited = 1; }
                else used = 0;
                break;
            default: used = 0; break;
            }
        }
        if (edited){ changed = 1; dirty = 1; st->last_caret = st->caret; }
        if (used){ e->used = 1; st->ensure = 1; st->blink_t0 = g_now_ms; }
    }
    if (ui_menu_is_open(st) && ui_tb_menu_draw(P, st, buf, cap, &len, o)){ changed = 1; dirty = 1; }
    if (dirty) ui_tb_layout(font, buf, len, o->wrap, wrap_w);

    /* ---- geometry: this frame's box + viewport, scroll the caret into view ---- */
    for (i = 0; i < g_tb_nlines; i++)
        if (g_tb_lines[i].w > content_w) content_w = g_tb_lines[i].w;
    content_h = (float)g_tb_nlines * line_h;

    if (!o->fill_w){                                   /* fit the text between w_min..w_max */
        float lo = UISC(o->w_min) - 2.0f * pad_x, hi = o->w_max > 0.0f ? UISC(o->w_max) - 2.0f * pad_x : 1.0e9f;
        if (lo < UISC(16)) lo = UISC(16);
        if (hi < lo) hi = lo;
        view_w = content_w + UISC(2);
        if (view_w < lo) view_w = lo;
        if (view_w > hi) view_w = hi;
        box_w = view_w + 2.0f * pad_x;
    }
    if (o->multiline){
        float lo = UISC(o->h_min), hi = o->h_max > 0.0f ? UISC(o->h_max) : 1.0e9f;
        box_h = content_h + 2.0f * pad_y;
        if (box_h < lo) box_h = lo;
        if (box_h > hi) box_h = hi;
        view_h = box_h - 2.0f * pad_y;
    } else {
        box_h = o->h_min > 0.0f && !o->bare ? UISC(o->h_min) : line_h + 2.0f * pad_y;
        view_h = line_h;
    }

    if (g_tb_wheel != 0.0f && ed.found && content_h > view_h + 0.5f && Clay_PointerOver(id)){
        st->scroll_y -= g_tb_wheel * line_h * 3.0f;    /* the wheel main routed to us */
        g_tb_wheel = 0.0f;
    }
    if (focused && st->ensure){
        int li = ui_tb_line_of(st->caret);
        float cx = ui_tb_w(font, buf + g_tb_lines[li].start, st->caret - g_tb_lines[li].start);
        float cy = (float)li * line_h;
        if (cx < st->scroll_x + UISC(2))          st->scroll_x = cx - UISC(8);
        if (cx > st->scroll_x + view_w - UISC(2)) st->scroll_x = cx - view_w + UISC(8);
        if (cy < st->scroll_y)                    st->scroll_y = cy;
        if (cy + line_h > st->scroll_y + view_h)  st->scroll_y = cy + line_h - view_h;
        st->ensure = 0;
    }
    {   float mx = o->wrap ? 0.0f : content_w + UISC(2) - view_w;
        float my = content_h - view_h;
        if (mx < 0.0f) mx = 0.0f;
        if (my < 0.0f) my = 0.0f;
        if (st->scroll_x < 0.0f) st->scroll_x = 0.0f;
        if (st->scroll_x > mx)   st->scroll_x = mx;
        if (st->scroll_y < 0.0f) st->scroll_y = 0.0f;
        if (st->scroll_y > my)   st->scroll_y = my;
    }
    if (ed.found && content_h > view_h + 0.5f &&
        g_tb_n_scrollable < (int)(sizeof g_tb_scrollable / sizeof g_tb_scrollable[0])){
        g_tb_scrollable[g_tb_n_scrollable].x = ed.boundingBox.x;
        g_tb_scrollable[g_tb_n_scrollable].y = ed.boundingBox.y;
        g_tb_scrollable[g_tb_n_scrollable].w = ed.boundingBox.width;
        g_tb_scrollable[g_tb_n_scrollable].h = ed.boundingBox.height;
        g_tb_n_scrollable++;
    }

    /* ---- emit: chrome box > clip > line rows; selection + caret float over ---- */
    {
        int s0 = st->caret < st->anchor ? st->caret : st->anchor;
        int s1 = st->caret > st->anchor ? st->caret : st->anchor;
        int caret_li = ui_tb_line_of(st->caret);
        float caret_x = ui_tb_w(font, buf + g_tb_lines[caret_li].start, st->caret - g_tb_lines[caret_li].start);
        int caret_on = focused && ((g_now_ms - st->blink_t0) / 530) % 2 == 0;
        float caret_w = UISC(1.2f) < 1.0f ? 1.0f : UISC(1.2f);
        float nl_w = ui_tb_w(font, " ", 1);
        Clay_Color selc = focused ? (Clay_Color){ P->accent.r, P->accent.g, P->accent.b, 80 }
                                  : (Clay_Color){ P->gray.r, P->gray.g, P->gray.b, 60 };
        Clay_Color bgc = o->bg.a > 0 ? o->bg : P->panel2;
        Clay_Color brc = focused ? (o->border_focus.a > 0 ? o->border_focus : P->accent)
                                 : (o->border.a > 0 ? o->border : P->border);
        uint16_t bw = (uint16_t)(o->bare ? 0 : 1);

        CLAY({ .id = id,
               .layout = { .sizing = { .width  = o->fill_w ? CLAY_SIZING_GROW(0) : CLAY_SIZING_FIXED(box_w),
                                       .height = CLAY_SIZING_FIXED(box_h) },
                           .padding = { (uint16_t)pad_x, (uint16_t)pad_x, (uint16_t)pad_y, (uint16_t)pad_y },
                           .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
               .backgroundColor = o->bare ? UI_NONE : bgc,
               .cornerRadius = CLAY_CORNER_RADIUS(o->bare ? 0.0f : UISC(o->radius)),
               .border = { .width = { bw, bw, bw, bw, 0 }, .color = brc } }) {
            CLAY({ .layout = { .sizing = { .width  = CLAY_SIZING_GROW(0),
                                           .height = o->multiline ? CLAY_SIZING_GROW(0) : CLAY_SIZING_FIT(0) },
                               .layoutDirection = CLAY_TOP_TO_BOTTOM },
                   .clip = { .horizontal = true, .vertical = true,
                             .childOffset = { -st->scroll_x, -st->scroll_y } } }) {
                for (i = 0; i < g_tb_nlines; i++){
                    UiTbLine L = g_tb_lines[i];
                    CLAY({ .layout = { .sizing = { .height = CLAY_SIZING_FIXED(line_h) } } }) {
                        if (s1 > s0 && s0 <= L.end && s1 >= L.start){       /* selection band */
                            int a = s0 > L.start ? s0 : L.start;
                            int b = s1 < L.end ? s1 : L.end;
                            if (b >= a){
                                float x0 = ui_tb_w(font, buf + L.start, a - L.start);
                                float x1 = b >= L.end ? L.w : ui_tb_w(font, buf + L.start, b - L.start);
                                if (s1 > L.end && i + 1 < g_tb_nlines) x1 += nl_w * 0.5f;   /* the newline */
                                if (x1 > x0 + 0.5f)
                                    CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_FIXED(x1 - x0),
                                                                   .height = CLAY_SIZING_FIXED(line_h) } },
                                           .floating = { .offset = { x0, 0 },
                                                         .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH,
                                                         .attachTo = CLAY_ATTACH_TO_PARENT,
                                                         .clipTo = CLAY_CLIP_TO_ATTACHED_PARENT },
                                           .backgroundColor = selc }) {}
                            }
                        }
                        if (caret_on && caret_li == i)
                            CLAY({ .layout = { .sizing = { .width = CLAY_SIZING_FIXED(caret_w),
                                                           .height = CLAY_SIZING_FIXED(line_h) } },
                                   .floating = { .offset = { caret_x, 0 },
                                                 .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH,
                                                 .attachTo = CLAY_ATTACH_TO_PARENT,
                                                 .clipTo = CLAY_CLIP_TO_ATTACHED_PARENT },
                                   .backgroundColor = P->text }) {}
                        if (i == 0 && len == 0 && o->placeholder.length > 0)
                            CLAY_TEXT(o->placeholder,
                                      CLAY_TEXT_CONFIG({ UI_FONT(o->fam, o->wt, o->sz), .textColor = P->faint,
                                                         .wrapMode = CLAY_TEXT_WRAP_NONE }));
                        else if (L.end > L.start)
                            CLAY_TEXT(((Clay_String){ .isStaticallyAllocated = false,
                                                      .length = L.end - L.start, .chars = buf + L.start }),
                                      CLAY_TEXT_CONFIG({ UI_FONT(o->fam, o->wt, o->sz), .textColor = P->text,
                                                         .wrapMode = CLAY_TEXT_WRAP_NONE }));
                    }
                }
            }
        }
    }

    *io_len = len;
    if (changed){ act |= UI_TB_CHANGED; st->last_caret = st->caret; }
    return act;
}

#endif /* UI_TEXTBOX_H */
