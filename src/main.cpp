/* The explorer shell: an SDL3 window with an OpenGL 3 context, RmlUi laying out the
   documents in assets/, and one Rant node watching the mesh beside it. */

#include "assets.hpp"
#include "capture.hpp"
#include "clipboard.hpp"
#include "decorators.hpp"
#include "editor.hpp"
#include "format.hpp"
#include "frame.hpp"
#include "icons.hpp"
#include "image.hpp"
#include "reload.hpp"
#include "tree.hpp"
#include "values.hpp"
#include "tiling.hpp"
#include "viz.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Debugger.h>

#include <RmlUi_Platform_SDL.h>
#include <RmlUi_Renderer_GL3.h>
#include <RmlUi_Include_GL3.h>   /* the GL entry points the renderer loaded */

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <set>
#include <string>

namespace {

/* Every size in the stylesheet is a CSS px from the mockup, scaled by this times the
   display density, so RANT_UI_ZOOM zooms the whole UI the way a browser does. */
float read_zoom()
{
    const char* env = std::getenv("RANT_UI_ZOOM");
    if (!env) return 1.0f;
    float z = (float)std::atof(env);
    return (z >= 0.5f && z <= 4.0f) ? z : 1.0f;
}

/* The stylesheet names two families. Registering the OS files under our own names keeps
   one stylesheet across platforms, the way ui_fonts.h picks its files by path. */
bool load_fonts()
{
#ifdef _WIN32
    const char* sans_regular = "C:/Windows/Fonts/segoeui.ttf";
    const char* sans_semi    = "C:/Windows/Fonts/seguisb.ttf";
    const char* sans_bold    = "C:/Windows/Fonts/segoeuib.ttf";
    const char* mono_regular = "C:/Windows/Fonts/consola.ttf";
    const char* mono_bold    = "C:/Windows/Fonts/consolab.ttf";
#elif defined(__APPLE__)
    const char* sans_regular = "/System/Library/Fonts/SFNS.ttf";
    const char* sans_semi    = "/System/Library/Fonts/SFNS.ttf";
    const char* sans_bold    = "/System/Library/Fonts/SFNS.ttf";
    const char* mono_regular = "/System/Library/Fonts/SFNSMono.ttf";
    const char* mono_bold    = "/System/Library/Fonts/SFNSMono.ttf";
#else
    const char* sans_regular = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    const char* sans_semi    = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf";
    const char* sans_bold    = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf";
    const char* mono_regular = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf";
    const char* mono_bold    = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf";
#endif
    const Rml::Style::FontStyle  normal   = Rml::Style::FontStyle::Normal;
    const Rml::Style::FontWeight semibold = static_cast<Rml::Style::FontWeight>(600);

    bool ok = Rml::LoadFontFace(sans_regular, "ui-sans", normal, Rml::Style::FontWeight::Normal);
    ok &= Rml::LoadFontFace(sans_semi, "ui-sans", normal, semibold);
    ok &= Rml::LoadFontFace(sans_bold, "ui-sans", normal, Rml::Style::FontWeight::Bold);
    ok &= Rml::LoadFontFace(mono_regular, "ui-mono", normal, Rml::Style::FontWeight::Normal);
    ok &= Rml::LoadFontFace(mono_bold, "ui-mono", normal, Rml::Style::FontWeight::Bold);
    return ok;
}

/* RmlUi's OpenGL 3 renderer reads only TGA, so PNG sources go through stb instead. */
class ExplorerRenderInterface : public RenderInterface_GL3 {
public:
    Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override
    {
        std::string file;
        Image image = asset_read(source, file) ? image_decode(file) : Image();
        if (!image.valid()) return RenderInterface_GL3::LoadTexture(dimensions, source);
        image_premultiply(image);   /* RmlUi composites with premultiplied alpha */
        dimensions = Rml::Vector2i(image.width, image.height);
        Rml::TextureHandle handle =
            GenerateTexture(Rml::Span<const Rml::byte>(image.rgba.data(), image.rgba.size()), dimensions);

        /* The renderer samples without mipmaps, so a big source drawn small shimmers. */
        if (handle)
        {
            glBindTexture(GL_TEXTURE_2D, (GLuint)handle);
            glGenerateMipmap(GL_TEXTURE_2D);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        return handle;
    }
};

/* RmlUi reports a bad selector or a missing file through the log, which a windowed build
   has nowhere to show, so the newest complaint goes in the status bar. */
class ExplorerSystemInterface : public SystemInterface_SDL {
public:
    using SystemInterface_SDL::SystemInterface_SDL;
    ~ExplorerSystemInterface() { SDL_DestroyCursor(col_resize); }

    /* The SDL layer's one resize cursor is the diagonal one. A splitter wants horizontal. */
    void SetMouseCursor(const Rml::String& cursor_name) override
    {
        if (cursor_name == "col-resize") SDL_SetCursor(col_resize);
        else SystemInterface_SDL::SetMouseCursor(cursor_name);
    }

    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override
    {
        /* The default interface writes to the platform debugger, which a shell cannot see,
           so every message goes to stderr as well. */
        static const char* const names[] = { "always", "error", "assert", "warning", "info", "debug" };
        const int index = (int)type;
        std::fprintf(stderr, "rmlui %s: %s",
                     index >= 0 && index < 6 ? names[index] : "?", message.c_str());
        std::fputc(10, stderr);
        std::fflush(stderr);

        if (type == Rml::Log::LT_ERROR || type == Rml::Log::LT_ASSERT || type == Rml::Log::LT_WARNING)
            last_problem = message;
        return SystemInterface_SDL::LogMessage(type, message);
    }

    Rml::String last_problem;

private:
    SDL_Cursor* col_resize = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_EW_RESIZE);
};

/* One card in the detail: a field whose visual is on, from the selected topic or pinned
   from another one. */
struct CardRow {
    Rml::String topic;
    Rml::String path;
    Rml::String name;
    bool        pinned = false;
    bool        other = false;   /* from a topic that is not the selected one */
};

/* What one topic tree row shows beside its name this frame, parallel to the rows. */
struct TreeCell {
    bool        dot = false;       /* the row's own topic can be subscribed */
    bool        sub = false;       /* it is, kept or as a preview */
    bool        preview = false;   /* only while it is watched */
    bool        active = false;    /* messages are arriving */
    Rml::String value, rate, jitter;

    bool operator==(const TreeCell& o) const
    {
        return dot == o.dot && sub == o.sub && preview == o.preview && active == o.active && value == o.value &&
               rate == o.rate && jitter == o.jitter;
    }
};

/* One line of the popup menu: an action, a checkbox, a copy or a group heading. */
struct MenuItem {
    Rml::String label;
    Rml::String action;          /* what menu_pick acts on, empty for a heading or a copy */
    Rml::String copy;            /* the text a copy line puts on the clipboard */
    Rml::String hint;            /* that text shortened, shown faint beside the label */
    int         check = 0;       /* 0 no box, 1 clear, 2 checked */
    int         table = -1;      /* a Copy table line's index into the menu's tables */
    Rml::String path;            /* the field a raw copy line reads when picked */
    bool        header = false;
};

/* One line on one line, cut to fit a menu or the status bar. */
Rml::String shorten(const std::string& text, size_t most)
{
    std::string line = text.substr(0, text.find('\n'));
    if (line.size() > most || line.size() < text.size()) line = line.substr(0, most) + "...";
    std::replace(line.begin(), line.end(), '\t', ' ');
    return line;
}

/* A line that copies one text, which it shows faint beside the label. */
MenuItem copy_item(const Rml::String& label, const std::string& text)
{
    MenuItem item;
    item.label = label;
    item.copy  = text;
    item.hint  = shorten(text, 24);
    return item;
}

/* What an element shows: its text, without hidden children or icon glyphs. */
void shown_text(Rml::Element* element, std::string& out)
{
    if (!element->IsVisible() || element->GetTagName() == "icon") return;
    if (auto* text = rmlui_dynamic_cast<Rml::ElementText*>(element)) {
        out += text->GetText();
        return;
    }
    for (int i = 0; i < element->GetNumChildren(); i++) shown_text(element->GetChild(i), out);
}

/* A table cell: a th or td, a .k or .v, or an element marked cell. */
bool is_cell(Rml::Element* element)
{
    const Rml::String tag = element->GetTagName();
    return tag == "th" || tag == "td" || element->IsClassSet("k") || element->IsClassSet("v") ||
           element->HasAttribute("cell");
}

/* A table row: a tr, a .kv or an element marked row. */
bool is_row(Rml::Element* element)
{
    return element->GetTagName() == "tr" || element->IsClassSet("kv") || element->HasAttribute("row");
}

/* The cell of a row nearest a point, the one it is in when it is in one, so a press in the
   gap between two cells or beside short text still finds its cell. Only cells with the
   class when one is given. */
Rml::Element* nearest_cell(Rml::Element* row, Rml::Vector2f point, const char* only)
{
    Rml::Element* best = nullptr;
    float         best_distance = 0;
    std::function<void(Rml::Element*)> visit = [&](Rml::Element* at) {
        if (!at->IsVisible()) return;
        if (!is_cell(at) || (only && !at->IsClassSet(only))) {
            for (int i = 0; i < at->GetNumChildren(); i++) visit(at->GetChild(i));
            return;
        }
        const Rml::Vector2f low  = at->GetAbsoluteOffset(Rml::BoxArea::Border);
        const Rml::Vector2f high = low + at->GetBox().GetSize(Rml::BoxArea::Border);
        const float dx = std::max({ low.x - point.x, 0.f, point.x - high.x });
        const float dy = std::max({ low.y - point.y, 0.f, point.y - high.y });
        if (!best || dx + dy < best_distance) {
            best          = at;
            best_distance = dx + dy;
        }
    };
    for (int i = 0; i < row->GetNumChildren(); i++) visit(row->GetChild(i));
    return best;
}

/* The table an element shows, read from its elements: a row is a tr, a .kv or an element
   marked row, and its cells are its th and td, its .k and .v, or its elements marked cell.
   A first row of th names the columns. */
void dom_rows(Rml::Element* element, Table& table)
{
    if (!element->IsVisible()) return;
    if (!is_row(element)) {
        for (int i = 0; i < element->GetNumChildren(); i++) dom_rows(element->GetChild(i), table);
        return;
    }
    std::vector<std::string> cells;
    bool heads = false;
    std::function<void(Rml::Element*)> gather = [&](Rml::Element* at) {
        if (!at->IsVisible()) return;
        if (is_cell(at)) {
            std::string text;
            shown_text(at, text);
            cells.push_back(text);
            heads = heads || at->GetTagName() == "th";
            return;
        }
        for (int i = 0; i < at->GetNumChildren(); i++) gather(at->GetChild(i));
    };
    for (int i = 0; i < element->GetNumChildren(); i++) gather(element->GetChild(i));
    if (cells.empty()) return;
    if (heads && table.rows.empty()) table.header = true;
    table.rows.push_back(cells);
}

/* A table copy line, its table kept beside the pending lines until the menu opens. */
MenuItem table_item(std::vector<Table>& tables, Table table, const char* label)
{
    MenuItem item;
    item.label = label;
    item.table = (int)tables.size();
    tables.push_back(std::move(table));
    return item;
}

/* Runs one function for an event, since a listener is a class. */
class Listener : public Rml::EventListener {
public:
    explicit Listener(std::function<void(Rml::Event&)> fn) : fn_(std::move(fn)) {}
    void ProcessEvent(Rml::Event& event) override { fn_(event); }

private:
    std::function<void(Rml::Event&)> fn_;
};

/* Everything the documents bind to. Copied from Capture once per frame. */
struct Ui {
    /* shell */
    /* The status bar line: the newest log line off the mesh, or why our node failed. */
    Rml::String status, tab = "topics";
    int         status_level = 2, peer_count = 0;   /* 0 error, 1 warn, 2 info */

    /* nodes tab */
    Rml::Vector<Capture::MachineRow> machines;
    bool               has_selection = false;
    Capture::NodeRow   selected;
    Rml::String        discovery_group;
    Capture::MetaView  meta;
    Rml::Vector<Capture::EndpointRow> publishes, subscribes;
    Rml::Vector<Capture::LogRow>      log;
    bool log_error = true, log_warn = true, log_info = true;

    /* topics tab */
    Rml::String           topic_filter;
    int                   topic_cats = 0;          /* the funnel's TreeCategory bits */
    bool                  filtering = false;       /* the text or the funnel hides something */
    std::set<std::string> expanded;        /* the branches open, by tree path */
    Rml::String           sel_path;        /* the selected row's tree path, empty for none */
    std::string           sel_name;        /* its topic's name on the mesh */
    Rml::Vector<TreeRow>  tree_rows;       /* the rows near the viewport, see window_tree */
    Rml::Vector<TreeCell> tree_cells;      /* their columns, parallel to tree_rows */
    Rml::String           tree_top = "0dp", tree_bottom = "0dp";   /* the spacers for the rest */
    int                   tree_total = 0, topic_count = 0, topic_matched = 0;
    bool                  has_topic = false;
    Capture::TopicRow     topic;

    /* the value tree of the watched topic, windowed like the topic tree */
    Rml::Vector<ValueRow>    value_rows;
    Rml::Vector<ValueCell>   value_cells;     /* the value column, parallel to value_rows */
    Rml::String              value_top = "0dp", value_bottom = "0dp";
    int                      value_total = 0;
    Rml::String              value_type, value_status;
    bool                     value_root_toggle = false, value_root_on = false;
    bool                     value_writable = false;
    int                      value_edit_count = 0;
    bool                     value_bad = false;          /* an edit does not parse */
    Rml::String              value_note;                 /* the head's word: sent, edited or writing */
    Rml::String              value_problem;              /* a failed write or call, whole, below the tree */
    bool                     value_can_compose = false;  /* a topic with a typed value to start from */
    bool                     value_composing = false;
    bool                     value_repeat = false, value_repeating = false;
    Capture::Kind            value_kind = Capture::Kind::Topic;
    bool                     value_calling = false;      /* a call or a run is in flight */
    Rml::String              value_progress;             /* a run's bar as a width, empty while none */
    Rml::Vector<CardRow>     cards;           /* one per field whose visual is shown */

    /* The one popup menu: the funnel's checklist, or what a right click offers. The row it
       was opened on is kept by tree path, and by topic name when the row is a topic. */
    bool                  menu_open = false;
    std::string           menu_kind;               /* "filter", "columns", "tree" or "copy" */
    Rml::String           menu_left = "0dp", menu_top = "0dp";
    Rml::Vector<MenuItem> menu_items;
    std::string           menu_branch, menu_self;
    /* A right click's lines from the handlers it passes, opened once it reaches the document. */
    Rml::Vector<MenuItem> menu_pending;
    std::string           menu_pending_kind;
    Image                 menu_image;   /* the picture a Copy image line puts on the clipboard */
    /* The tables the menu's lines copy, gathered beside the pending lines. */
    std::vector<Table>    menu_tables, menu_pending_tables;
    std::string           menu_topic;   /* the value a raw copy line reads when picked */
    Rml::ObserverPtr<Rml::Element> menu_cell;   /* the cell the menu copies, held lit while it is open */

    /* The status bar's note: a reload, a copy or what RmlUi complained about, for a while. */
    Rml::String note;
    unsigned    note_frames = 0;

    /* Sidebar widths in dp once a splitter has been dragged, zero while the theme default
       stands. The drag_ fields hold one drag: where it began and how wide it may go. */
    float sidebar_left = 0, sidebar_right = 0;
    float drag_x = 0, drag_width = 0, drag_max = 0;
    /* Tree table columns by name: a dragged weight, zero for the theme default, and which
       are hidden. A drag of a header edge weighs its table's shown columns by their widths
       when it began, then moves width between the two either side, from drag_min to
       drag_max dp. */
    std::map<std::string, float> columns;
    std::map<std::string, bool>  hidden_columns;
    std::vector<std::pair<std::string, float>> drag_columns;
    int   drag_left = -1, drag_right = -1;
    float drag_min = 0;
    /* One scrub of a number by its name: the text it started from and where. */
    std::string scrub_from;
    float       scrub_x = 0;

    bool light     = false;   /* the light palette, a class on body */
    bool maximized = false;   /* picks the maximize or restore glyph */
};

/* What the ui keeps on body: the theme class, a dragged width inline where the sidebar
   and column rules read their token, and a hide class per hidden column. */
void apply_body(Rml::ElementDocument* document, const Ui& ui)
{
    if (!document) return;
    document->SetClass("light", ui.light);
    if (ui.sidebar_left > 0)
        document->SetProperty("--sidebar-left", Rml::CreateString("%.0fdp", ui.sidebar_left));
    if (ui.sidebar_right > 0)
        document->SetProperty("--sidebar-right", Rml::CreateString("%.0fdp", ui.sidebar_right));
    for (const auto& column : ui.columns) {
        if (column.second > 0) document->SetProperty("--c-" + column.first, Rml::CreateString("%.2f", column.second));
        else document->RemoveProperty("--c-" + column.first);
    }
    for (const auto& column : ui.hidden_columns) document->SetClass("hide-" + column.first, column.second);
}

/* A tree table header's columns, its cells marked col but not keep, as the checklist its
   right click opens, then a line that puts every width and column back. A hidden cell
   still has its text, so the label is read past its visibility. */
Rml::Vector<MenuItem> column_menu(Rml::Element* head, const Ui& ui)
{
    Rml::Vector<MenuItem> items;
    for (int i = 0; i < head->GetNumChildren(); i++) {
        Rml::Element* cell = head->GetChild(i);
        MenuItem item;
        item.action = cell->GetAttribute<Rml::String>("col", "");
        if (item.action.empty() || cell->HasAttribute("keep")) continue;
        for (int k = 0; k < cell->GetNumChildren(); k++)
            if (auto* text = rmlui_dynamic_cast<Rml::ElementText*>(cell->GetChild(k))) item.label += text->GetText();
        const auto hidden = ui.hidden_columns.find(item.action);
        item.check = hidden != ui.hidden_columns.end() && hidden->second ? 1 : 2;
        items.push_back(item);
    }
    MenuItem reset;
    reset.label  = "Reset columns";
    reset.action = "reset";
    items.push_back(reset);
    return items;
}

/* The menu's size in shell.rcss and components.rcss, which keeps it inside the window. */
constexpr float MENU_W_DP    = 200.f;
constexpr float MENU_ITEM_DP = 26.f;

/* The funnel's checklist, its boxes as the bits stand now. */
Rml::Vector<MenuItem> filter_menu(unsigned cats)
{
    Rml::Vector<MenuItem> items;
    auto header = [&items](const char* label) {
        MenuItem item;
        item.label  = label;
        item.header = true;
        items.push_back(item);
    };
    auto box = [&items, cats](const char* label, unsigned bit) {
        MenuItem item;
        item.label  = label;
        item.action = std::to_string(bit);
        item.check  = (cats & bit) ? 2 : 1;
        items.push_back(item);
    };
    header("Kind");
    box("Topics", CAT_TOPIC);
    box("Functions", CAT_FUNCTION);
    box("Variables", CAT_VARIABLE);
    box("Tasks", CAT_TASK);
    header("Reliability");
    box("Reliable", CAT_RELIABLE);
    box("Best effort", CAT_BEST_EFFORT);
    header("State");
    box("Subscribed", CAT_SUBSCRIBED);
    box("Active", CAT_ACTIVE);
    if (cats) {
        MenuItem clear;
        clear.label  = "Clear filters";
        clear.action = "clear";
        items.push_back(clear);
    }
    return items;
}

bool subscribable(const Capture::TopicRow& topic)
{
    return !Capture::is_call(topic.kind);
}

/* The subscribable topics under a tree path that pass the filter, not the path's own. */
std::vector<std::string> subtree(const Capture& capture, const std::string& branch,
                                 const std::string& text, unsigned cats)
{
    std::vector<std::string> names;
    const std::string prefix = branch + "/";
    for (const Capture::TopicRow& topic : capture.topics())
        if (subscribable(topic) && tree_path(topic.name).compare(0, prefix.size(), prefix) == 0 &&
            topic_passes(capture, topic, text, cats))
            names.push_back(topic.name);
    return names;
}

/* A topic's value, rate and jitter as its tree row shows them: a dash while it is not
   subscribed, nothing for a function or task. */
void topic_columns(const Capture& capture, const Capture::TopicRow& topic, std::string& value,
                   std::string& rate, std::string& jitter)
{
    if (!subscribable(topic)) return;
    rate = jitter = DASH;
    const Capture::WatchView* sub = capture.view(topic.name);
    if (sub && (capture.subscribed(topic.name) || topic.name == capture.watched().name)) {
        value  = preview_text(*sub);
        rate   = format_rate(sub->rate_hz);
        jitter = format_ms(sub->jitter_ms);
    }
}

/* The topics the filter shows under a tree path, every one when it is empty, as the tree's
   columns in tree order, folded or not. */
Table topic_table(const Capture& capture, const std::string& branch, const std::string& text, unsigned cats)
{
    std::vector<std::pair<std::string, const Capture::TopicRow*>> topics;
    for (const Capture::TopicRow& topic : capture.topics()) {
        const std::string path = tree_path(topic.name);
        const bool under = branch.empty() || path == branch || path.compare(0, branch.size() + 1, branch + "/") == 0;
        if (under && topic_passes(capture, topic, text, cats)) topics.emplace_back(path, &topic);
    }
    std::sort(topics.begin(), topics.end());
    Table table;
    table.header = true;
    table.rows.push_back({ "Topic", "Value", "Rate", "Jitter" });
    for (const auto& entry : topics) {
        std::string value, rate, jitter;
        topic_columns(capture, *entry.second, value, rate, jitter);
        table.rows.push_back({ entry.second->name, value, rate, jitter });
    }
    return table;
}

/* The shown column after a header edge, marked col, or null when the edge ends the row. */
Rml::Element* column_after(Rml::Element* edge)
{
    for (Rml::Element* next = edge->GetNextSibling(); next; next = next->GetNextSibling())
        if (next->IsVisible() && next->HasAttribute("col")) return next;
    return nullptr;
}

/* A tree table's header, just before its body, keeps the room of the body's scrollbar
   only while the body has one, which RmlUi shows only while it overflows. An edge with no
   shown column after it is the row's end, which never moves, so it hides. */
void fit_head(Rml::ElementDocument* document, const char* pane_id)
{
    Rml::Element* pane = document ? document->GetElementById(pane_id) : nullptr;
    Rml::Element* head = pane ? pane->GetPreviousSibling() : nullptr;
    if (!head || !head->IsClassSet("tt-head")) return;
    head->SetClass("gutter", pane->GetClientWidth() < pane->GetBox().GetSize(Rml::BoxArea::Padding).x);
    for (int i = 0; i < head->GetNumChildren(); i++) {
        Rml::Element* edge = head->GetChild(i);
        if (edge->IsClassSet("edge")) edge->SetClass("end", !column_after(edge));
    }
}

/* The cards tiled into the area under the topic head. Each card's visual says what it
   wants, and the boxes are written inline only when the area or a wish moved, since a
   written box lays out again. True when they were. */
bool tile_cards(Rml::Context* context, Rml::ElementDocument* document, std::string& key)
{
    Rml::Element* area = document ? document->QuerySelector(".viz-area") : nullptr;
    if (!area || !area->IsVisible(true)) return false;
    std::vector<Rml::Element*> cards;
    std::vector<Tile>          tiles;
    for (int i = 0; i < area->GetNumChildren(); i++) {
        Rml::Element* card = area->GetChild(i);
        if (!card->IsClassSet("viz-card") || !card->IsVisible()) continue;
        Rml::Element* viz = nullptr;
        for (int j = 0; j < card->GetNumChildren() && !viz; j++)
            if (card->GetChild(j)->GetTagName() == "viz") viz = card->GetChild(j);
        cards.push_back(card);
        tiles.push_back(viz_tile(viz));
    }
    const float W = area->GetClientWidth(), H = area->GetClientHeight();
    char buf[64];
    std::string now = std::to_string((int)W) + "x" + std::to_string((int)H);
    for (const Tile& t : tiles) {
        std::snprintf(buf, sizeof buf, ":%.3f/%.2f", t.aspect, t.stiff);
        now += buf;
    }
    if (now == key) return false;
    key = now;
    const float dp = context->GetDensityIndependentPixelRatio();
    const std::vector<TileBox> boxes = tile(tiles, W, H, 8 * dp, 16 * dp, 40 * dp, 150 * dp, 110 * dp);
    for (size_t i = 0; i < cards.size(); i++) {
        const TileBox b = i < boxes.size() ? boxes[i] : TileBox();
        cards[i]->SetProperty(Rml::PropertyId::Left, Rml::Property(std::round(b.x), Rml::Unit::PX));
        cards[i]->SetProperty(Rml::PropertyId::Top, Rml::Property(std::round(b.y), Rml::Unit::PX));
        cards[i]->SetProperty(Rml::PropertyId::Width, Rml::Property(std::round(b.w), Rml::Unit::PX));
        cards[i]->SetProperty(Rml::PropertyId::Height, Rml::Property(std::round(b.h), Rml::Unit::PX));
    }
    return true;
}

/* Only the rows near the viewport become elements, with a spacer standing in for the rest
   above and below, since the retained DOM has no clipper. The pane is found by id and a
   row of it measured, a guess until one exists. True when the window or the row height
   moved, so the caller rebinds the rows only then. */
bool window_rows(Rml::Context* context, Rml::ElementDocument* document, const char* pane_id,
                 int total, int& first_out, int& last_out, Rml::String& top_out, Rml::String& bottom_out)
{
    float top_dp = 0, height_dp = 0, row_dp = 28.f;
    if (Rml::Element* pane = document ? document->GetElementById(pane_id) : nullptr) {
        const float ratio = context->GetDensityIndependentPixelRatio();
        top_dp    = pane->GetScrollTop() / ratio;
        height_dp = pane->GetClientHeight() / ratio;
        if (Rml::Element* row = pane->QuerySelector(".tt-row")) {
            const float measured = row->GetBox().GetSize(Rml::BoxArea::Border).y / ratio;
            if (measured > 0) row_dp = measured;
        }
    }
    /* A hidden pane measures zero. A screenful makes the first show instant. */
    if (height_dp <= 0) height_dp = 40 * row_dp;

    int first = (int)(top_dp / row_dp) - 8;
    int last  = (int)((top_dp + height_dp) / row_dp) + 9;
    first = first < 0 ? 0 : first > total ? total : first;
    last  = last < first ? first : last > total ? total : last;
    const Rml::String top    = Rml::CreateString("%.2fdp", first * row_dp);
    const Rml::String bottom = Rml::CreateString("%.2fdp", (total - last) * row_dp);
    if (first == first_out && last == last_out && top == top_out && bottom == bottom_out) return false;

    first_out  = first;
    last_out   = last;
    top_out    = top;
    bottom_out = bottom;
    return true;
}

/* A binding takes the new value and is marked dirty only when it moved, since a dirty
   array re-evaluates every view in every element its data-for made. True when it moved. */
template <class T>
bool sync(Rml::DataModelHandle model, const char* name, T& bound, T value)
{
    if (bound == value) return false;
    bound = std::move(value);
    model.DirtyVariable(name);
    return true;
}

/* The node list's fields only, since a node row also counts seconds the list never shows.
   A field left out is one the list would never redraw. */
Rml::String machines_signature(const Rml::Vector<Capture::MachineRow>& machines)
{
    Rml::String out;
    for (const Capture::MachineRow& m : machines) {
        out += m.host;
        out += '|';
        for (const Capture::NodeRow& n : m.nodes)
            out += Rml::CreateString("%s;%d;%d;%s|", n.name.c_str(), (int)n.alive,
                                     n.rtt_samples, format_ms(n.rtt_ms).c_str());
    }
    return out;
}

/* Reload the document and every stylesheet it pulls in. The data model is owned by the
   context, not the document, so the fresh document rebinds to it. */
Rml::ElementDocument* reload_document(Rml::Context* context, Rml::ElementDocument* document,
                                      const Rml::String& path)
{
    if (document) document->Close();
    Rml::Factory::ClearStyleSheetCache();
    Rml::Factory::ClearTemplateCache();
    Rml::ReleaseTextures();
    Rml::ElementDocument* fresh = context->LoadDocument(path);
    if (fresh) fresh->Show();
    return fresh;
}

void usage(const char* exe)
{
    std::fprintf(stderr,
        "usage: %s [--domain N] [--group IP] [--port N] [--if IP] [--name STR]\n", exe);
}

} /* namespace */

int main(int argc, char** argv)
{
    Capture::Options cap_opts;
    for (int i = 1; i < argc; i++) {
        const char* arg  = argv[i];
        const char* next = (i + 1 < argc) ? argv[i + 1] : nullptr;
        if      (!std::strcmp(arg, "--domain") && next) cap_opts.domain              = (uint16_t)std::atoi(argv[++i]);
        else if (!std::strcmp(arg, "--group")  && next) cap_opts.discovery_group     = argv[++i];
        else if (!std::strcmp(arg, "--port")   && next) cap_opts.discovery_port      = (uint16_t)std::atoi(argv[++i]);
        else if (!std::strcmp(arg, "--if")     && next) cap_opts.multicast_interface = argv[++i];
        else if (!std::strcmp(arg, "--name")   && next) cap_opts.name                = argv[++i];
        else { usage(argv[0]); return 2; }
    }

    SDL_SetMainReady();
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    const SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                                  SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_BORDERLESS;
    SDL_Window* window = SDL_CreateWindow("Rant Explorer", 1400, 900, flags);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }

    assets_init();
    std::fprintf(stderr, "assets: %s", assets_dir().empty() ? "embedded" : assets_dir().c_str());
    std::fputc(10, stderr);

    /* The taskbar and title bar icon. The exe's own file icon is Logo.ico, through the
       resource script. Straight alpha here: SDL blends the surface itself. */
    std::string logo;
    Image icon = asset_read("Logo.png", logo) ? image_decode(logo) : Image();
    if (icon.valid()) {
        SDL_Surface* surface = SDL_CreateSurfaceFrom(icon.width, icon.height,
                                                     SDL_PIXELFORMAT_RGBA32,
                                                     icon.rgba.data(), icon.width * 4);
        if (surface) {
            SDL_SetWindowIcon(window, surface);
            SDL_DestroySurface(surface);
        }
    }

    SDL_GLContext gl = SDL_GL_CreateContext(window);
    if (!gl) {
        std::fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_MakeCurrent(window, gl);
    SDL_GL_SetSwapInterval(1);

    Rml::String gl_message;
    if (!RmlGL3::Initialize(&gl_message)) {
        std::fprintf(stderr, "OpenGL 3: %s\n", gl_message.c_str());
        return 1;
    }

    ExplorerRenderInterface render_interface;
    ExplorerSystemInterface system_interface(window);
    if (!render_interface) {
        std::fprintf(stderr, "the OpenGL 3 render interface failed to build\n");
        return 1;
    }

    Rml::SetSystemInterface(&system_interface);
    Rml::SetRenderInterface(&render_interface);
    Rml::SetFileInterface(&assets_file_interface());
    Rml::Initialise();

    if (!load_fonts())
        std::fprintf(stderr, "warning: a system font failed to load, text may be missing\n");
    if (!icons_init())
        std::fprintf(stderr, "warning: the icon font failed to load, icons will be missing\n");
    decorators_init();

    int width = 0, height = 0;
    SDL_GetWindowSizeInPixels(window, &width, &height);
    render_interface.SetViewport(width, height);

    Rml::Context* context = Rml::CreateContext("explorer", Rml::Vector2i(width, height));
    if (!context) {
        std::fprintf(stderr, "Rml::CreateContext failed\n");
        return 1;
    }
    float zoom = read_zoom();
    context->SetDensityIndependentPixelRatio(SDL_GetWindowPixelDensity(window) * zoom);
    Rml::Debugger::Initialise(context);
    frame_init(window, context);

    Capture capture(cap_opts);
    viz_init(capture);
    Editor  editor(capture);

    Ui ui;
    ui.discovery_group = capture.discovery_group();
    ui.status          = capture.ok() ? "" : ("node failed: " + Rml::String(capture.error()));
    ui.status_level    = capture.ok() ? 2 : 0;

    /* The whole topic tree, rebuilt when the mesh, the filter or the expanded set moves.
       The document holds a window of it, see window_tree. */
    std::vector<TreeRow> tree;
    bool                 tree_dirty = true;

    /* The value tree, rebuilt when the watched topic, its shape, a fold or a toggle moves.
       The document holds a window of it and only the value texts change per message. */
    std::vector<ValueRow> value_rows;
    bool                  value_dirty = true;

    /* Every name a card shows keeps its traces in line with those cards: the selected
       topic's by its shown set, another topic's by its pinned set. What each was last
       traced with, by name, so a trace moves only when its cards or its shape do. */
    std::map<std::string, std::string> traced;
    std::string tiling;   /* what the cards were last tiled for */

    Rml::DataModelHandle model;

    /* The popup opens at a point in dp, kept inside the window. */
    auto open_menu = [&ui, &model, context](const std::string& kind, float x, float y) {
        const float ratio = context->GetDensityIndependentPixelRatio();
        const float max_x = context->GetDimensions().x / ratio - MENU_W_DP - 4.f;
        const float max_y = context->GetDimensions().y / ratio - ui.menu_items.size() * MENU_ITEM_DP - 12.f;
        ui.menu_open = true;
        ui.menu_kind = kind;
        ui.menu_left = Rml::CreateString("%ddp", (int)std::max(0.f, std::min(x, max_x)));
        ui.menu_top  = Rml::CreateString("%ddp", (int)std::max(0.f, std::min(y, max_y)));
        model.DirtyVariable("menu_open");
        model.DirtyVariable("menu_left");
        model.DirtyVariable("menu_top");
        model.DirtyVariable("menu_items");
    };
    auto close_menu = [&ui, &model]() {
        if (ui.menu_cell) ui.menu_cell->SetClass("held", false);
        ui.menu_cell.reset();
        ui.menu_open = false;
        model.DirtyVariable("menu_open");
    };
    /* A topic's newest value, arrays whole, and the view it came with. Null when it has none. */
    auto value_of = [&capture](const std::string& name, const std::vector<Capture::ValueNode>*& nodes)
        -> const Capture::WatchView* {
        const Capture::WatchView* view = name == capture.watched().name ? &capture.watched() : capture.view(name);
        if (!view) return nullptr;
        const std::vector<Capture::ValueNode>& whole = capture.whole(name);
        nodes = whole.empty() ? &view->nodes : &whole;
        return view;
    };
    auto copy = [&ui](const std::string& text) {
        if (text.empty()) return;
        Rml::GetSystemInterface()->SetClipboardText(text);
        ui.note        = "copied " + shorten(text, 60);
        ui.note_frames = 150;
    };
    auto copy_image = [&ui, window](const Image& image) {
        if (!clipboard_set_image(window, image)) return;
        ui.note        = Rml::CreateString("copied a %d x %d image", image.width, image.height);
        ui.note_frames = 150;
    };
    auto copy_table = [&ui, window](const Table& table) {
        if (!clipboard_set_table(window, table)) return;
        ui.note        = Rml::CreateString("copied a table of %d rows", (int)table.rows.size());
        ui.note_frames = 150;
    };
    if (Rml::DataModelConstructor ctor = context->CreateDataModel("shell")) {
        ctor.RegisterTransformFunc("dur", [](const Rml::VariantList& a) -> Rml::Variant {
            return a.empty() ? Rml::Variant() : Rml::Variant(format_duration(a[0].Get<double>()));
        });
        ctor.RegisterTransformFunc("age", [](const Rml::VariantList& a) -> Rml::Variant {
            return a.empty() ? Rml::Variant() : Rml::Variant(format_age(a[0].Get<double>()));
        });
        ctor.RegisterTransformFunc("bytes", [](const Rml::VariantList& a) -> Rml::Variant {
            return a.empty() ? Rml::Variant() : Rml::Variant(format_bytes(a[0].Get<double>()));
        });
        ctor.RegisterTransformFunc("ms", [](const Rml::VariantList& a) -> Rml::Variant {
            return a.empty() ? Rml::Variant() : Rml::Variant(format_ms(a[0].Get<double>()));
        });
        ctor.RegisterTransformFunc("num", [](const Rml::VariantList& a) -> Rml::Variant {
            return a.empty() ? Rml::Variant() : Rml::Variant(format_number(a[0].Get<double>()));
        });
        ctor.RegisterTransformFunc("icon", [](const Rml::VariantList& a) -> Rml::Variant {
            return a.empty() ? Rml::Variant() : Rml::Variant(Rml::String(kind_icon(a[0].Get<Rml::String>())));
        });
        ctor.RegisterTransformFunc("pct", [](const Rml::VariantList& a) -> Rml::Variant {
            if (a.empty()) return {};
            const double v = a[0].Get<double>();
            return Rml::Variant(v < 0 ? Rml::String(DASH) : Rml::CreateString("%.1f %%", v));
        });

        /* before the structs that hold one */
        ctor.RegisterScalar<Capture::Kind>([](const Capture::Kind& kind, Rml::Variant& out) {
            out = Rml::String(Capture::kind_word(kind));
        });

        if (auto node_row = ctor.RegisterStruct<Capture::NodeRow>()) {
            node_row.RegisterMember("id",            &Capture::NodeRow::id);
            node_row.RegisterMember("name",          &Capture::NodeRow::name);
            node_row.RegisterMember("address",       &Capture::NodeRow::address);
            node_row.RegisterMember("alive",         &Capture::NodeRow::alive);
            node_row.RegisterMember("rtt_ms",        &Capture::NodeRow::rtt_ms);
            node_row.RegisterMember("jitter_ms",     &Capture::NodeRow::jitter_ms);
            node_row.RegisterMember("rtt_min_ms",    &Capture::NodeRow::rtt_min_ms);
            node_row.RegisterMember("rtt_samples",   &Capture::NodeRow::rtt_samples);
            node_row.RegisterMember("last_heard_s",  &Capture::NodeRow::last_heard_s);
            node_row.RegisterMember("observed_s",    &Capture::NodeRow::observed_s);
            node_row.RegisterMember("updates",       &Capture::NodeRow::updates);
            node_row.RegisterMember("fragment_size", &Capture::NodeRow::fragment_size);
            node_row.RegisterMember("publishes",     &Capture::NodeRow::publishes);
            node_row.RegisterMember("subscribes",    &Capture::NodeRow::subscribes);
        }
        ctor.RegisterArray<Rml::Vector<Capture::NodeRow>>();

        if (auto machine = ctor.RegisterStruct<Capture::MachineRow>()) {
            machine.RegisterMember("host",  &Capture::MachineRow::host);
            machine.RegisterMember("nodes", &Capture::MachineRow::nodes);
        }
        ctor.RegisterArray<Rml::Vector<Capture::MachineRow>>();

        if (auto endpoint = ctor.RegisterStruct<Capture::EndpointRow>()) {
            endpoint.RegisterMember("name",     &Capture::EndpointRow::name);
            endpoint.RegisterMember("kind",     &Capture::EndpointRow::kind);
            endpoint.RegisterMember("reliable", &Capture::EndpointRow::reliable);
            endpoint.RegisterMember("count",    &Capture::EndpointRow::count);
        }
        ctor.RegisterArray<Rml::Vector<Capture::EndpointRow>>();

        if (auto peer = ctor.RegisterStruct<Capture::MetaPeerRow>()) {
            peer.RegisterMember("name",         &Capture::MetaPeerRow::name);
            peer.RegisterMember("active",       &Capture::MetaPeerRow::active);
            peer.RegisterMember("rtt_ms",       &Capture::MetaPeerRow::rtt_ms);
            peer.RegisterMember("jitter_ms",    &Capture::MetaPeerRow::jitter_ms);
            peer.RegisterMember("rtt_min_ms",   &Capture::MetaPeerRow::rtt_min_ms);
            peer.RegisterMember("samples",      &Capture::MetaPeerRow::samples);
            peer.RegisterMember("publish_to",   &Capture::MetaPeerRow::publish_to);
            peer.RegisterMember("receive_from", &Capture::MetaPeerRow::receive_from);
        }
        ctor.RegisterArray<Rml::Vector<Capture::MetaPeerRow>>();

        if (auto meta = ctor.RegisterStruct<Capture::MetaView>()) {
            meta.RegisterMember("fresh",          &Capture::MetaView::fresh);
            meta.RegisterMember("status",         &Capture::MetaView::status);
            meta.RegisterMember("uptime_s",       &Capture::MetaView::uptime_s);
            meta.RegisterMember("age_s",          &Capture::MetaView::age_s);
            meta.RegisterMember("peers",          &Capture::MetaView::peers);
            meta.RegisterMember("max_peers",      &Capture::MetaView::max_peers);
            meta.RegisterMember("topics",         &Capture::MetaView::topics);
            meta.RegisterMember("max_topics",     &Capture::MetaView::max_topics);
            meta.RegisterMember("mem_in_use",     &Capture::MetaView::mem_in_use);
            meta.RegisterMember("mem_peak",       &Capture::MetaView::mem_peak);
            meta.RegisterMember("alloc_calls",    &Capture::MetaView::alloc_calls);
            meta.RegisterMember("evicted_unsent", &Capture::MetaView::evicted_unsent);
            meta.RegisterMember("bp_waits",       &Capture::MetaView::bp_waits);
            meta.RegisterMember("bp_waited_s",    &Capture::MetaView::bp_waited_s);
            meta.RegisterMember("shm_tx",         &Capture::MetaView::shm_tx);
            meta.RegisterMember("shm_rx",         &Capture::MetaView::shm_rx);
            meta.RegisterMember("last_error",     &Capture::MetaView::last_error);
            meta.RegisterMember("have_proc",      &Capture::MetaView::have_proc);
            meta.RegisterMember("cpu_pct",        &Capture::MetaView::cpu_pct);
            meta.RegisterMember("pid",            &Capture::MetaView::pid);
            meta.RegisterMember("rss",            &Capture::MetaView::rss);
            meta.RegisterMember("peak_rss",       &Capture::MetaView::peak_rss);
            meta.RegisterMember("heap_total",     &Capture::MetaView::heap_total);
            meta.RegisterMember("heap_free",      &Capture::MetaView::heap_free);
            meta.RegisterMember("heap_min_free",  &Capture::MetaView::heap_min_free);
            meta.RegisterMember("heap_largest",   &Capture::MetaView::heap_largest);
            meta.RegisterMember("peer_rows",      &Capture::MetaView::peer_rows);
        }

        if (auto topic = ctor.RegisterStruct<Capture::TopicRow>()) {
            topic.RegisterMember("name",      &Capture::TopicRow::name);
            topic.RegisterMember("kind",      &Capture::TopicRow::kind);
            topic.RegisterMember("note",      &Capture::TopicRow::note);
            topic.RegisterMember("from",      &Capture::TopicRow::from);
            topic.RegisterMember("type",      &Capture::TopicRow::type);
            topic.RegisterMember("reliable",  &Capture::TopicRow::reliable);
        }

        if (auto row = ctor.RegisterStruct<TreeRow>()) {
            row.RegisterMember("name",      &TreeRow::name);
            row.RegisterMember("path",      &TreeRow::path);
            row.RegisterMember("indent",    &TreeRow::indent);
            row.RegisterMember("kind",      &TreeRow::kind);
            row.RegisterMember("topic_name", &TreeRow::topic_name);
            row.RegisterMember("branch",    &TreeRow::branch);
            row.RegisterMember("open",      &TreeRow::open);
            row.RegisterMember("has_topic", &TreeRow::has_topic);
            row.RegisterMember("odd",       &TreeRow::odd);
        }
        ctor.RegisterArray<Rml::Vector<TreeRow>>();

        ctor.RegisterArray<Rml::Vector<Rml::String>>();   /* before the structs that hold one */
        if (auto row = ctor.RegisterStruct<ValueRow>()) {
            row.RegisterMember("name",   &ValueRow::name);
            row.RegisterMember("type",   &ValueRow::type);
            row.RegisterMember("indent", &ValueRow::indent);
            row.RegisterMember("path",   &ValueRow::path);
            row.RegisterMember("branch", &ValueRow::branch);
            row.RegisterMember("open",   &ValueRow::open);
            row.RegisterMember("gap",    &ValueRow::gap);
            row.RegisterMember("on",     &ValueRow::on);
            row.RegisterMember("odd",    &ValueRow::odd);
            row.RegisterMember("editor",  &ValueRow::editor);
            row.RegisterMember("options", &ValueRow::options);
            row.RegisterMember("width",   &ValueRow::width);
            row.RegisterMember("part",    &ValueRow::part);
            row.RegisterMember("blank",   &ValueRow::blank);
            row.RegisterMember("scrub",   &ValueRow::scrub);
        }
        ctor.RegisterArray<Rml::Vector<ValueRow>>();

        if (auto cell = ctor.RegisterStruct<ValueCell>()) {
            cell.RegisterMember("text",    &ValueCell::text);
            cell.RegisterMember("size",    &ValueCell::size);
            cell.RegisterMember("edited",  &ValueCell::edited);
            cell.RegisterMember("bad",     &ValueCell::bad);
            cell.RegisterMember("writing", &ValueCell::writing);
            cell.RegisterMember("tone",    &ValueCell::tone);
        }
        ctor.RegisterArray<Rml::Vector<ValueCell>>();

        if (auto card = ctor.RegisterStruct<CardRow>()) {
            card.RegisterMember("topic",  &CardRow::topic);
            card.RegisterMember("path",   &CardRow::path);
            card.RegisterMember("name",   &CardRow::name);
            card.RegisterMember("pinned", &CardRow::pinned);
            card.RegisterMember("other",  &CardRow::other);
        }
        ctor.RegisterArray<Rml::Vector<CardRow>>();

        if (auto cell = ctor.RegisterStruct<TreeCell>()) {
            cell.RegisterMember("dot",     &TreeCell::dot);
            cell.RegisterMember("sub",     &TreeCell::sub);
            cell.RegisterMember("preview", &TreeCell::preview);
            cell.RegisterMember("active",  &TreeCell::active);
            cell.RegisterMember("value",   &TreeCell::value);
            cell.RegisterMember("rate",    &TreeCell::rate);
            cell.RegisterMember("jitter",  &TreeCell::jitter);
        }
        ctor.RegisterArray<Rml::Vector<TreeCell>>();

        if (auto item = ctor.RegisterStruct<MenuItem>()) {
            item.RegisterMember("label",  &MenuItem::label);
            item.RegisterMember("hint",   &MenuItem::hint);
            item.RegisterMember("check",  &MenuItem::check);
            item.RegisterMember("header", &MenuItem::header);
        }
        ctor.RegisterArray<Rml::Vector<MenuItem>>();

        if (auto log_row = ctor.RegisterStruct<Capture::LogRow>()) {
            log_row.RegisterMember("level", &Capture::LogRow::level);
            log_row.RegisterMember("time",  &Capture::LogRow::time);
            log_row.RegisterMember("text",  &Capture::LogRow::text);
        }
        ctor.RegisterArray<Rml::Vector<Capture::LogRow>>();

        ctor.Bind("status",          &ui.status);
        ctor.Bind("status_level",    &ui.status_level);
        ctor.Bind("tab",             &ui.tab);
        ctor.Bind("peer_count",      &ui.peer_count);
        ctor.Bind("machines",        &ui.machines);
        ctor.Bind("has_selection",   &ui.has_selection);
        ctor.Bind("sel",             &ui.selected);
        ctor.Bind("discovery_group", &ui.discovery_group);
        ctor.Bind("meta",            &ui.meta);
        ctor.Bind("publishes",       &ui.publishes);
        ctor.Bind("subscribes",      &ui.subscribes);
        ctor.Bind("log",             &ui.log);
        ctor.Bind("log_error",       &ui.log_error);
        ctor.Bind("log_warn",        &ui.log_warn);
        ctor.Bind("log_info",        &ui.log_info);
        ctor.Bind("note",            &ui.note);
        ctor.Bind("topic_filter",    &ui.topic_filter);
        ctor.Bind("topic_cats",      &ui.topic_cats);
        ctor.Bind("filtering",       &ui.filtering);
        ctor.Bind("menu_open",       &ui.menu_open);
        ctor.Bind("menu_left",       &ui.menu_left);
        ctor.Bind("menu_top",        &ui.menu_top);
        ctor.Bind("menu_items",      &ui.menu_items);
        ctor.Bind("sel_path",        &ui.sel_path);
        ctor.Bind("tree_rows",       &ui.tree_rows);
        ctor.Bind("tree_cells",      &ui.tree_cells);
        ctor.Bind("value_kind",      &ui.value_kind);
        ctor.Bind("value_calling",   &ui.value_calling);
        ctor.Bind("value_progress",  &ui.value_progress);
        ctor.Bind("tree_top",        &ui.tree_top);
        ctor.Bind("tree_bottom",     &ui.tree_bottom);
        ctor.Bind("tree_total",      &ui.tree_total);
        ctor.Bind("topic_count",     &ui.topic_count);
        ctor.Bind("topic_matched",   &ui.topic_matched);
        ctor.Bind("has_topic",       &ui.has_topic);
        ctor.Bind("topic",           &ui.topic);
        ctor.Bind("value_rows",      &ui.value_rows);
        ctor.Bind("value_cells",     &ui.value_cells);
        ctor.Bind("value_writable",  &ui.value_writable);
        ctor.Bind("value_edit_count", &ui.value_edit_count);
        ctor.Bind("value_bad",       &ui.value_bad);
        ctor.Bind("value_note",      &ui.value_note);
        ctor.Bind("value_problem",   &ui.value_problem);
        ctor.Bind("value_can_compose", &ui.value_can_compose);
        ctor.Bind("value_composing", &ui.value_composing);
        ctor.Bind("value_repeat",    &ui.value_repeat);
        ctor.Bind("value_repeating", &ui.value_repeating);
        ctor.Bind("value_hz",        &editor.rate);
        ctor.Bind("value_top",       &ui.value_top);
        ctor.Bind("value_bottom",    &ui.value_bottom);
        ctor.Bind("value_total",     &ui.value_total);
        ctor.Bind("value_type",      &ui.value_type);
        ctor.Bind("value_status",    &ui.value_status);
        ctor.Bind("value_root_toggle", &ui.value_root_toggle);
        ctor.Bind("value_root_on",   &ui.value_root_on);
        ctor.Bind("cards",           &ui.cards);
        ctor.Bind("light",           &ui.light);
        ctor.Bind("maximized",       &ui.maximized);

        ctor.BindEventCallback("select_node",
            [&capture](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
                if (!args.empty()) capture.select(args[0].Get<Rml::String>());
            });
        ctor.BindEventCallback("toggle_theme",
            [&ui](Rml::DataModelHandle handle, Rml::Event& event, const Rml::VariantList&) {
                ui.light = !ui.light;
                handle.DirtyVariable("light");
                apply_body(event.GetTargetElement()->GetOwnerDocument(), ui);
            });
        ctor.BindEventCallback("minimize_window",
            [window](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { SDL_MinimizeWindow(window); });
        ctor.BindEventCallback("maximize_window",
            [window](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
                if (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) SDL_RestoreWindow(window);
                else SDL_MaximizeWindow(window);
            });
        ctor.BindEventCallback("close_window",
            [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
                SDL_Event quit = {};
                quit.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&quit);
            });
        ctor.BindEventCallback("set_tab",
            [&ui](Rml::DataModelHandle handle, Rml::Event&, const Rml::VariantList& args) {
                if (args.empty()) return;
                ui.tab = args[0].Get<Rml::String>();
                handle.DirtyVariable("tab");
            });
        /* One splitter's grip: dragstart measures, every drag event after it sets the width.
           The sidebar is the splitter's neighbour, since every tab has its own. */
        ctor.BindEventCallback("drag_sidebar",
            [&ui, context](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList& args) {
                if (args.empty()) return;
                const bool  left     = args[0].Get<Rml::String>() == "left";
                const float ratio    = context->GetDensityIndependentPixelRatio();
                const float mouse_x  = event.GetParameter<float>("mouse_x", 0.f) / ratio;
                Rml::Element* splitter = event.GetCurrentElement()->GetParentNode();
                if (event.GetId() == Rml::EventId::Dragstart) {
                    Rml::Element* mine = left ? splitter->GetPreviousSibling() : splitter->GetNextSibling();
                    Rml::Element* row  = splitter->GetParentNode();
                    if (!mine || !row) return;
                    /* Everything in the row but this sidebar and the detail pane keeps its width. */
                    float taken = 0;
                    for (int i = 0; i < row->GetNumChildren(); i++) {
                        Rml::Element* child = row->GetChild(i);
                        if (child != mine && !child->IsClassSet("detail"))
                            taken += child->GetBox().GetSize(Rml::BoxArea::Border).x / ratio;
                    }
                    ui.drag_x     = mouse_x;
                    ui.drag_width = mine->GetBox().GetSize(Rml::BoxArea::Border).x / ratio;
                    /* The detail pane keeps at least 320dp whatever the sidebars take. */
                    ui.drag_max   = context->GetDimensions().x / ratio - taken - 320.f;
                    return;
                }
                float width = ui.drag_width + (left ? mouse_x - ui.drag_x : ui.drag_x - mouse_x);
                width = width < 160.f ? 160.f : width > ui.drag_max ? ui.drag_max : width;
                (left ? ui.sidebar_left : ui.sidebar_right) = width;
                apply_body(splitter->GetOwnerDocument(), ui);
            });
        /* The grip of the edge between two header cells moves only that edge: the column
           before grows as the one after shrinks, each keeping 24dp of text. Every shown column
           is weighed by its border width, since a border box basis of zero shares it all. */
        ctor.BindEventCallback("drag_column",
            [&ui, context](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList&) {
                const float   ratio   = context->GetDensityIndependentPixelRatio();
                const float   mouse_x = event.GetParameter<float>("mouse_x", 0.f) / ratio;
                Rml::Element* edge    = event.GetCurrentElement()->GetParentNode();
                Rml::Element* left    = edge ? edge->GetPreviousSibling() : nullptr;
                Rml::Element* right   = edge ? column_after(edge) : nullptr;
                Rml::Element* head    = edge ? edge->GetParentNode() : nullptr;
                if (!left || !head) return;
                if (event.GetId() == Rml::EventId::Dragstart) {
                    ui.drag_columns.clear();
                    ui.drag_left = ui.drag_right = -1;
                    if (!right) return;
                    for (int i = 0; i < head->GetNumChildren(); i++) {
                        Rml::Element* cell = head->GetChild(i);
                        if (!cell->IsVisible() || !cell->HasAttribute("col")) continue;
                        if (cell == left) ui.drag_left = (int)ui.drag_columns.size();
                        if (cell == right) ui.drag_right = (int)ui.drag_columns.size();
                        ui.drag_columns.emplace_back(cell->GetAttribute<Rml::String>("col", ""),
                                                     cell->GetBox().GetSize(Rml::BoxArea::Border).x / ratio);
                    }
                    if (ui.drag_left < 0 || ui.drag_right < 0) return;
                    ui.drag_x   = mouse_x;
                    ui.drag_min = 24.f - left->GetBox().GetSize(Rml::BoxArea::Content).x / ratio;
                    ui.drag_max = right->GetBox().GetSize(Rml::BoxArea::Content).x / ratio - 24.f;
                    return;
                }
                if (ui.drag_left < 0 || ui.drag_right < 0) return;
                const float moved = std::max(ui.drag_min, std::min(mouse_x - ui.drag_x, ui.drag_max));
                for (int i = 0; i < (int)ui.drag_columns.size(); i++)
                    ui.columns[ui.drag_columns[i].first] = ui.drag_columns[i].second +
                                                           (i == ui.drag_left ? moved : i == ui.drag_right ? -moved : 0.f);
                apply_body(head->GetOwnerDocument(), ui);
            });
        /* The tree: the caret zone opens or closes a branch, the rest of the row selects
           the topic, or opens the branch when the row is only a namespace. */
        auto toggle_branch = [&ui, &tree_dirty](const Rml::String& path) {
            if (!ui.expanded.erase(path)) ui.expanded.insert(path);
            tree_dirty = true;
        };
        ctor.BindEventCallback("toggle_branch",
            [toggle_branch](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList& args) {
                if (args.empty()) return;
                toggle_branch(args[0].Get<Rml::String>());
                event.StopPropagation();   /* the row under the caret must not also select */
            });
        ctor.BindEventCallback("click_row",
            [&ui, &tree, &capture, &editor, toggle_branch](Rml::DataModelHandle handle, Rml::Event&,
                                                              const Rml::VariantList& args) {
                if (args.size() < 2) return;
                const Rml::String path = args[0].Get<Rml::String>();
                if (!args[1].Get<bool>()) {
                    toggle_branch(path);
                    return;
                }
                for (const TreeRow& row : tree)
                    if (row.path == path && row.topic >= 0) ui.sel_name = capture.topics()[row.topic].name;
                if (capture.watched().name != ui.sel_name) editor.end_draft();
                ui.sel_path = path;
                handle.DirtyVariable("sel_path");
                /* Selecting only previews: the watch lets go when another row is picked,
                   unless a subscribe or a pinned card keeps it. */
                capture.watch(ui.sel_name);
            });
        /* Letting a topic go drops its pinned cards, which would have nothing to show, and
           ends its watch, so a preview lets go too. Keeping the selected one watches it again. */
        auto set_sub = [&editor, &ui, &capture, &value_dirty](const std::string& name, bool on) {
            capture.subscribe(name, on);
            if (on) {
                if (name == ui.sel_name) capture.watch(name);
                return;
            }
            editor.view(name).pinned.clear();
            value_dirty = true;
            if (name == capture.watched().name) {
                editor.end_draft();
                capture.watch("");
            }
        };
        /* A row's dot is its own topic's alone. A click subscribes, or lets a kept
           subscription or a preview go. */
        ctor.BindEventCallback("toggle_sub",
            [&tree, &capture, set_sub](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList& args) {
                if (args.empty()) return;
                const Rml::String path = args[0].Get<Rml::String>();
                event.StopPropagation();
                for (const TreeRow& row : tree)
                    if (row.path == path && row.topic >= 0 && subscribable(capture.topics()[row.topic])) {
                        const std::string& name = capture.topics()[row.topic].name;
                        set_sub(name, !capture.subscribed(name) && !capture.previewing(name));
                    }
            });
        /* The funnel opens its checklist under itself. */
        ctor.BindEventCallback("open_filter_menu",
            [&ui, context, open_menu](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList&) {
                const float         ratio  = context->GetDensityIndependentPixelRatio();
                Rml::Element*       button = event.GetCurrentElement();
                const Rml::Vector2f at     = button->GetAbsoluteOffset(Rml::BoxArea::Border);
                const float         height = button->GetBox().GetSize(Rml::BoxArea::Border).y;
                ui.menu_items = filter_menu((unsigned)ui.topic_cats);
                open_menu("filter", at.x / ratio, (at.y + height) / ratio + 4.f);
            });
        /* A right click on a tree row offers its own topic, and for a branch everything
           under it the filter shows. */
        ctor.BindEventCallback("row_menu",
            [&ui, &tree, &capture](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList& args) {
                if (args.empty() || event.GetParameter<int>("button", 0) != 1) return;
                const Rml::String path = args[0].Get<Rml::String>();
                std::string self;
                for (const TreeRow& row : tree)
                    if (row.path == path && row.topic >= 0 && subscribable(capture.topics()[row.topic]))
                        self = capture.topics()[row.topic].name;
                std::vector<std::string> names = subtree(capture, path, ui.topic_filter, (unsigned)ui.topic_cats);
                const bool branch = !names.empty();
                if (!self.empty()) names.push_back(self);
                int on = 0;
                for (const std::string& name : names) on += capture.subscribed(name) ? 1 : 0;

                auto item = [&ui](const Rml::String& label, const char* action) {
                    MenuItem m;
                    m.label  = label;
                    m.action = action;
                    ui.menu_pending.push_back(m);
                };
                if (!self.empty() && !capture.subscribed(self)) item("Subscribe", "self_on");
                if (!self.empty() && (capture.subscribed(self) || capture.previewing(self)))
                    item("Unsubscribe", "self_off");
                if (branch && on < (int)names.size())
                    item(Rml::CreateString("Subscribe All (%d)", (int)names.size()), "all_on");
                if (branch && on > 0)
                    item(Rml::CreateString("Unsubscribe All (%d)", (int)names.size()), "all_off");
                ui.menu_branch       = path;
                ui.menu_self         = self;
                ui.menu_pending_kind = "tree";
            });
        /* A press outside the menu closes it. Presses inside bubble here too, so only the
           layer's own count. */
        ctor.BindEventCallback("close_menu",
            [close_menu, context](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList&) {
                Rml::Element* layer = event.GetCurrentElement();
                if (event.GetTargetElement() != layer) return;
                close_menu();
                /* A right click elsewhere opens a menu there, so the press goes on to what
                   is under the layer. */
                if (event.GetParameter<int>("button", 0) != 1) return;
                const Rml::Vector2f at(event.GetParameter<float>("mouse_x", 0.f), event.GetParameter<float>("mouse_y", 0.f));
                if (Rml::Element* under = context->GetElementAtPoint(at, layer))
                    under->DispatchEvent(Rml::EventId::Mousedown, event.GetParameters());
            });
        /* A checkbox flips and leaves the checklist open, every other line acts and closes. */
        ctor.BindEventCallback("menu_pick",
            [&ui, &capture, &tree_dirty, set_sub, close_menu, copy, copy_image, copy_table, value_of](
                Rml::DataModelHandle handle, Rml::Event& event, const Rml::VariantList& args) {
                if (args.empty()) return;
                const int at = args[0].Get<int>();
                if (at < 0 || at >= (int)ui.menu_items.size()) return;
                const MenuItem picked = ui.menu_items[at];
                if (!picked.copy.empty()) {
                    copy(picked.copy);
                    close_menu();
                    return;
                }
                if (picked.table >= 0 && picked.table < (int)ui.menu_tables.size()) {
                    copy_table(ui.menu_tables[picked.table]);
                    close_menu();
                    return;
                }
                const std::string action = picked.action;
                if (action.empty()) return;
                if (action == "image" || action == "json") {
                    const std::vector<Capture::ValueNode>* nodes = nullptr;
                    const Capture::WatchView* view = action == "json" ? value_of(ui.menu_topic, nodes) : nullptr;
                    if (action == "image") copy_image(ui.menu_image);
                    else if (view) copy(copy_raw(*nodes, *view, picked.path));
                    close_menu();
                    return;
                }
                if (ui.menu_kind == "columns") {
                    if (action == "reset") {
                        for (auto& column : ui.columns) column.second = 0;
                        for (auto& column : ui.hidden_columns) column.second = false;
                        close_menu();
                    } else {
                        MenuItem& item = ui.menu_items[at];
                        item.check = item.check == 2 ? 1 : 2;
                        ui.hidden_columns[action] = item.check == 1;
                        handle.DirtyVariable("menu_items");
                    }
                    apply_body(event.GetTargetElement()->GetOwnerDocument(), ui);
                    return;
                }
                if (ui.menu_kind == "filter") {
                    if (action == "clear") {
                        ui.topic_cats = 0;
                        close_menu();
                    } else {
                        ui.topic_cats ^= std::atoi(action.c_str());
                        ui.menu_items = filter_menu((unsigned)ui.topic_cats);
                        handle.DirtyVariable("menu_items");
                    }
                    handle.DirtyVariable("topic_cats");
                    tree_dirty = true;
                    return;
                }
                if (action == "self_on" || action == "self_off") {
                    set_sub(ui.menu_self, action == "self_on");
                } else {
                    std::vector<std::string> names = subtree(capture, ui.menu_branch, ui.topic_filter,
                                                             (unsigned)ui.topic_cats);
                    if (!ui.menu_self.empty()) names.push_back(ui.menu_self);
                    for (const std::string& name : names) set_sub(name, action == "all_on");
                }
                close_menu();
            });
        /* The value tree: a branch row folds, the round toggle shows the field's visual. */
        ctor.BindEventCallback("fold_value",
            [&editor, &value_dirty](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
                if (args.size() < 2 || !args[1].Get<bool>()) return;
                const Rml::String path = args[0].Get<Rml::String>();
                ValueView& view = editor.view();
                if (const Capture::ValueNode* node = editor.node(path)) view.open[path] = !is_open(*node, view);
                value_dirty = true;
            });
        /* A visual the user turns on starts pinned, so it stays when another topic is picked.
           Only the root's, opened by default, starts unpinned. */
        ctor.BindEventCallback("toggle_value",
            [&editor, &ui, &capture, &value_dirty](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList& args) {
                if (args.empty()) return;
                const Rml::String  path = args[0].Get<Rml::String>();
                const std::string& name = capture.watched().name;
                ValueView& view = editor.view(name);
                event.StopPropagation();   /* the row under the toggle must not also fold */
                if (view.shown.erase(path)) {
                    view.pinned.erase(path);
                } else if (ui.cards.size() >= (size_t)MAX_CARDS) {
                    ui.note        = Rml::CreateString("at most %d cards at once, close one first", MAX_CARDS);
                    ui.note_frames = 150;
                    return;
                } else {
                    view.shown.insert(path);
                    view.pinned.insert(path);
                    if (!Capture::is_call(capture.watched().kind)) capture.subscribe(name, true);
                }
                value_dirty = true;
            });
        ctor.BindEventCallback("pin_card",
            [&editor, &capture, &value_dirty](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
                if (args.size() < 2) return;
                const Rml::String name = args[0].Get<Rml::String>(), path = args[1].Get<Rml::String>();
                ValueView& view = editor.view(name);
                if (!view.pinned.erase(path)) {
                    view.pinned.insert(path);
                    capture.subscribe(name, true);
                }
                value_dirty = true;
            });
        ctor.BindEventCallback("close_card",
            [&editor, &value_dirty](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
                if (args.size() < 2) return;
                const Rml::String path = args[1].Get<Rml::String>();
                ValueView& view = editor.view(args[0].Get<Rml::String>());
                view.shown.erase(path);
                view.pinned.erase(path);
                value_dirty = true;
            });
        /* The wheel steps a number while its editor has focus, and the pane does not scroll. */
        ctor.BindEventCallback("wheel_value",
            [&editor](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList& args) {
                /* the target can be the input's own text element, so ask the input */
                if (args.empty() || !event.GetCurrentElement()->IsPseudoClassSet("focus")) return;
                const float dy = event.GetParameter<float>("wheel_delta_y", 0.f);
                const Rml::String path = args[0].Get<Rml::String>();
                editor.nudge(path, editor.text(path), dy < 0 ? 1 : dy > 0 ? -1 : 0, event.GetParameter<bool>("shift_key", false));
                event.StopPropagation();
            });
        /* Dragging a number's field scrubs it: a step per 4dp from where the drag began. The
           pointer wraps at the window's edges and the start moves with it, so it never runs out. */
        ctor.BindEventCallback("scrub_value",
            [&ui, &editor, context, window](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList& args) {
                if (args.empty()) return;
                const Rml::String path  = args[0].Get<Rml::String>();
                const float       ratio = context->GetDensityIndependentPixelRatio();
                const float       px    = event.GetParameter<float>("mouse_x", 0.f);
                const float       x     = px / ratio;
                if (event.GetId() == Rml::EventId::Dragstart) {
                    ui.scrub_from = editor.text(path);
                    ui.scrub_x    = x;
                    return;
                }
                editor.nudge(path, ui.scrub_from, (int)std::lround((x - ui.scrub_x) / 4.f), event.GetParameter<bool>("shift_key", false));
                const float width = (float)context->GetDimensions().x;
                if (px > 0.f && px < width - 1.f) return;
                const float to      = px <= 0.f ? width - 2.f : 1.f;
                const float density = SDL_GetWindowPixelDensity(window);
                SDL_WarpMouseInWindow(window, to / density, event.GetParameter<float>("mouse_y", 0.f) / density);
                ui.scrub_x += (to - px) / ratio;
            });
        ctor.BindEventCallback("edit_value",
            [&editor](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList& args) {
                /* Enter also reaches an input as a change with a line break, after edit_key
                   has already written the value. */
                if (args.empty() || event.GetParameter<bool>("linebreak", false)) return;
                editor.apply(args[0].Get<Rml::String>(), event.GetParameter<Rml::String>("value", ""));
            });
        ctor.BindEventCallback("flip_value",
            [&editor](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
                if (!args.empty()) editor.flip(args[0].Get<Rml::String>());
            });
        ctor.BindEventCallback("reset_value",
            [&editor](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList& args) {
                if (args.empty()) return;
                editor.reset(args[0].Get<Rml::String>());
                event.StopPropagation();
            });
        /* Enter does what the head's button does, Escape puts the field back to the mesh's
           value. */
        ctor.BindEventCallback("edit_key",
            [&editor](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList& args) {
                if (args.empty()) return;
                const Rml::String path = args[0].Get<Rml::String>();
                const int key = event.GetParameter<int>("key_identifier", 0);
                if (key == Rml::Input::KI_UP || key == Rml::Input::KI_DOWN) {
                    editor.nudge(path, editor.text(path), key == Rml::Input::KI_UP ? 1 : -1, event.GetParameter<bool>("shift_key", false));
                    event.StopPropagation();
                } else if (key == Rml::Input::KI_RETURN || key == Rml::Input::KI_NUMPADENTER) {
                    event.GetTargetElement()->Blur();
                    editor.commit();
                } else if (key == Rml::Input::KI_ESCAPE) {
                    event.GetTargetElement()->Blur();
                    editor.reset(path);
                }
            });
        ctor.BindEventCallback("compose",
            [&editor](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
                if (!args.empty()) editor.compose(args[0].Get<bool>());
            });
        ctor.BindEventCallback("toggle_repeat",
            [&editor](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList&) {
                if (event.GetTargetElement()->GetTagName() != "input") editor.toggle_repeat();
            });
        ctor.BindEventCallback("send",
            [&editor](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { editor.send(); });
        ctor.BindEventCallback("stop",
            [&editor](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { editor.stop(); });
        ctor.BindEventCallback("call",
            [&editor](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { editor.call(); });
        ctor.BindEventCallback("cancel",
            [&editor](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { editor.cancel(); });
        ctor.BindEventCallback("undo_values",
            [&editor](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { editor.undo(); });
        ctor.BindEventCallback("clear_filter",
            [&ui](Rml::DataModelHandle handle, Rml::Event&, const Rml::VariantList&) {
                ui.topic_filter.clear();
                handle.DirtyVariable("topic_filter");
            });
        ctor.BindEventCallback("toggle_log",
            [&ui, &capture](Rml::DataModelHandle handle, Rml::Event&, const Rml::VariantList& args) {
                if (args.empty()) return;
                switch (args[0].Get<int>()) {
                case 0: ui.log_error = !ui.log_error; handle.DirtyVariable("log_error"); break;
                case 1: ui.log_warn  = !ui.log_warn;  handle.DirtyVariable("log_warn");  break;
                default: ui.log_info = !ui.log_info;  handle.DirtyVariable("log_info");  break;
                }
                capture.set_log_levels((ui.log_error ? 1u : 0u) | (ui.log_warn ? 2u : 0u) |
                                       (ui.log_info ? 4u : 0u));
            });

        model = ctor.GetModelHandle();
    }

    /* The copy lines, the same wherever a right click lands, after what a handler added:

         Copy image           a picture under the pointer
         Copy cell            the cell under the pointer
         Copy                 a lone value, marked copy
         Copy subtree         the branch of the row under the pointer, marked subtree
         Copy table           the table the pointer is in: marked copy-table and read from
         Copy table as JSON   its rows, a value marked topic and path read from the mesh, or
                              a windowed tree marked table, read from its source by name

       Every line reads the element it names, so no table or list needs code of its own. */
    const std::map<std::string, std::function<Table(const std::string&)>> table_sources = {
        { "topics", [&ui, &capture](const std::string& branch) {
              return topic_table(capture, branch, ui.topic_filter, (unsigned)ui.topic_cats);
          } },
    };
    Listener right_click([&ui, &editor, context, open_menu, value_of, &table_sources](Rml::Event& event) {
        if (event.GetParameter<int>("button", 0) != 1) return;
        std::vector<MenuItem> lines;

        /* A tree table's header offers its columns, nothing to copy. */
        Rml::Element* head = event.GetTargetElement();
        while (head && !head->IsClassSet("tt-head")) head = head->GetParentNode();
        if (head) {
            lines = column_menu(head, ui);
            ui.menu_pending_kind = "columns";
        } else {
            /* The cell may be under the pointer without holding it: a .kv box holds its value
               and padding, and a table row can be taller than one of its cells. */
            Rml::Element*       cell = nullptr;
            const Rml::Vector2f point(event.GetParameter<float>("mouse_x", 0.f), event.GetParameter<float>("mouse_y", 0.f));
            for (Rml::Element* at = event.GetTargetElement(); at && !cell; at = at->GetParentNode()) {
                if (at->HasAttribute("cell-path") || is_cell(at)) cell = at;
                else if (at->IsClassSet("kv")) cell = nearest_cell(at, point, "v");
                else if (is_row(at)) cell = nearest_cell(at, point, nullptr);
            }
            /* The value a topic attribute names: the nearest element carrying one, from here up. */
            auto value_at = [&value_of](Rml::Element* from, std::string& topic, const std::vector<Capture::ValueNode>*& nodes)
                -> const Capture::WatchView* {
                while (from && !from->HasAttribute("topic")) from = from->GetParentNode();
                if (!from) return nullptr;
                topic = from->GetAttribute<Rml::String>("topic", "");
                return value_of(topic, nodes);
            };
            if (cell) {
                std::string text;
                if (cell->HasAttribute("cell-path")) {
                    /* a field of a value copies whole, and an edit as typed */
                    std::string topic;
                    const std::vector<Capture::ValueNode>* nodes = nullptr;
                    const std::string path = cell->GetAttribute<Rml::String>("cell-path", "");
                    if (const Capture::WatchView* view = value_at(cell, topic, nodes)) text = copy_text(*nodes, *view, path);
                    const auto mine = editor.views().find(topic);
                    if (mine != editor.views().end() && mine->second.edits.count(path)) text = mine->second.edits.at(path).text;
                } else if (!cell->GetAttribute<Rml::String>("cell", "").empty()) {
                    text = cell->GetAttribute<Rml::String>("cell", "");
                } else {
                    shown_text(cell, text);
                }
                if (!text.empty()) lines.push_back(copy_item("Copy cell", text));
                if (ui.menu_cell) ui.menu_cell->SetClass("held", false);
                cell->SetClass("held", true);
                ui.menu_cell = cell->GetObserverPtr();
            }
            std::string subtree;
            for (Rml::Element* at = event.GetTargetElement(); at && subtree.empty(); at = at->GetParentNode())
                subtree = at->GetAttribute<Rml::String>("subtree", "");

            for (Rml::Element* at = event.GetTargetElement(); at; at = at->GetParentNode())
                if (at->HasAttribute("copy")) {
                    std::string text;
                    shown_text(at, text);
                    if (!text.empty()) lines.push_back(copy_item("Copy", text));
                    break;
                }

            Rml::Element* source = event.GetTargetElement();
            while (source && !source->HasAttribute("copy-table") && !source->HasAttribute("topic") &&
                   !source->HasAttribute("table"))
                source = source->GetParentNode();
            /* A table read whole, as a line to copy it and one to copy its JSON. */
            auto table_lines = [&ui, &lines](Table table, const char* label, const char* json_label) {
                if (table.rows.empty()) return;
                MenuItem json = copy_item(json_label, table_json(table));
                json.hint.clear();
                lines.push_back(table_item(ui.menu_pending_tables, std::move(table), label));
                lines.push_back(json);
            };
            if (source && source->HasAttribute("copy-table")) {
                Table table;
                dom_rows(source, table);
                table_lines(std::move(table), "Copy table", "Copy table as JSON");
            } else if (source && source->HasAttribute("table")) {
                const auto found = table_sources.find(source->GetAttribute<Rml::String>("table", ""));
                if (found != table_sources.end()) {
                    if (!subtree.empty()) table_lines(found->second(subtree), "Copy subtree", "Copy subtree as JSON");
                    table_lines(found->second(""), "Copy table", "Copy table as JSON");
                }
            } else if (source) {
                std::string topic;
                const std::vector<Capture::ValueNode>* nodes = nullptr;
                const std::string path = source->GetAttribute<Rml::String>("path", "");
                const Capture::WatchView* view = value_at(source, topic, nodes);
                const int index = view ? find_node(*nodes, *view, path) : -2;
                const std::string std_name = index == -1 ? view->root_std : index >= 0 ? (*nodes)[index].std_name : "";
                MenuItem json;
                json.label  = "Copy table as JSON";
                json.action = "json";
                json.path   = path;
                ui.menu_topic = topic;
                /* a branch field under the pointer copies as a table of its own */
                if (view && !subtree.empty() && subtree != path) {
                    Table branch = table_of(*nodes, *view, subtree);
                    if (!branch.rows.empty()) {
                        MenuItem branch_json = json;
                        branch_json.label = "Copy subtree as JSON";
                        branch_json.path  = subtree;
                        lines.push_back(table_item(ui.menu_pending_tables, std::move(branch), "Copy subtree"));
                        lines.push_back(branch_json);
                    }
                }
                if (std_name == "Image" || std_name == "VideoFrame") {
                    /* a picture is no table, but its raw data still copies */
                    ui.menu_image = std_name == "Image" ? image_of(*nodes, index, view->message) : viz_picture(source);
                    MenuItem image;
                    image.label  = "Copy image";
                    image.action = "image";
                    if (ui.menu_image.valid()) lines.insert(lines.begin(), image);
                    lines.push_back(json);
                } else if (index >= -1) {
                    Table table = table_of(*nodes, *view, path);
                    if (!table.rows.empty()) {
                        lines.push_back(table_item(ui.menu_pending_tables, std::move(table), "Copy table"));
                        lines.push_back(json);
                    } else if (!cell) {
                        const std::string text = copy_text(*nodes, *view, path);
                        if (!text.empty()) lines.push_back(copy_item("Copy", text));
                    }
                }
            }
        }
        ui.menu_pending.insert(ui.menu_pending.end(), lines.begin(), lines.end());

        if (ui.menu_pending.empty()) {
            ui.menu_pending_tables.clear();
            return;
        }
        ui.menu_items.swap(ui.menu_pending);
        ui.menu_pending.clear();
        ui.menu_tables.swap(ui.menu_pending_tables);
        ui.menu_pending_tables.clear();
        const float ratio = context->GetDensityIndependentPixelRatio();
        open_menu(ui.menu_pending_kind.empty() ? "copy" : ui.menu_pending_kind,
                  event.GetParameter<float>("mouse_x", 0.f) / ratio, event.GetParameter<float>("mouse_y", 0.f) / ratio);
        ui.menu_pending_kind.clear();
    });

    /* RmlUi's text field selects a word on a double click and has no triple click. A press
       on the same field soon after a double click selects all of it. The field handles
       its own press first, so this runs last. */
    Rml::Element* double_field = nullptr;
    double        double_at    = 0;
    Listener triple_click([&double_field, &double_at](Rml::Event& event) {
        auto* input = rmlui_dynamic_cast<Rml::ElementFormControlInput*>(event.GetTargetElement());
        if (event.GetId() == Rml::EventId::Dblclick) {
            double_field = input;
            double_at    = Capture::now_s();
            return;
        }
        if (input && input == double_field && Capture::now_s() - double_at < 0.5) input->Select();
        double_field = nullptr;
    });
    auto listen = [&right_click, &triple_click](Rml::ElementDocument* document) {
        document->AddEventListener(Rml::EventId::Mousedown, &right_click);
        document->AddEventListener(Rml::EventId::Mousedown, &triple_click);
        document->AddEventListener(Rml::EventId::Dblclick, &triple_click);
    };

    const Rml::String document_path = "shell.rml";
    Rml::ElementDocument* document = context->LoadDocument(document_path);
    if (!document) {
        std::fprintf(stderr, "could not load %s\n", document_path.c_str());
        return 1;
    }
    document->Show();
    listen(document);
    model.DirtyAllVariables();

    Rml::String machines_sig;

    /* What the tree was last built from, and the window of it the document holds. */
    uint32_t    tree_epoch = 0xFFFFFFFFu;
    Rml::String       tree_filter;
    std::vector<bool> tree_passing;   /* per topic, while a state box is checked */
    int               window_first = -1, window_last = -1;

    /* The same for the value tree, plus the message its texts were last written from. */
    std::string value_name, value_shape;
    uint64_t    value_seq = 0;
    int         value_first = -1, value_last = -1;

    /* Hot reload: an edited RML or RCSS is picked up without a rebuild, and F5 forces it. */
    AssetWatcher watcher(assets_dir());
    bool         reload_now = false;

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_EVENT_QUIT:
                running = false;
                break;
            case SDL_EVENT_WINDOW_MAXIMIZED:
            case SDL_EVENT_WINDOW_RESTORED:
                ui.maximized = event.type == SDL_EVENT_WINDOW_MAXIMIZED;
                model.DirtyVariable("maximized");
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                SDL_GetWindowSizeInPixels(window, &width, &height);
                render_interface.SetViewport(width, height);
                context->SetDimensions(Rml::Vector2i(width, height));
                context->SetDensityIndependentPixelRatio(SDL_GetWindowPixelDensity(window) * zoom);
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                /* Ctrl and the wheel zoom the whole UI the way a browser does. */
                if (SDL_GetModState() & SDL_KMOD_CTRL) {
                    zoom *= event.wheel.y > 0 ? 1.1f : 1.0f / 1.1f;
                    zoom  = zoom < 0.5f ? 0.5f : zoom > 4.0f ? 4.0f : zoom;
                    context->SetDensityIndependentPixelRatio(SDL_GetWindowPixelDensity(window) * zoom);
                    break;
                }
                RmlSDL::InputEventHandler(context, window, event);
                break;
            case SDL_EVENT_KEY_DOWN:
                if (event.key.key == SDLK_F12) {
                    Rml::Debugger::SetVisible(!Rml::Debugger::IsVisible());
                    break;
                }
                if (event.key.key == SDLK_F5) {
                    reload_now = true;
                    break;
                }
                if (event.key.key == SDLK_ESCAPE && ui.menu_open) {
                    close_menu();
                    break;
                }
                /* Ctrl+C with no text field focused copies the selected topic or node. */
                if (event.key.key == SDLK_C && (event.key.mod & SDL_KMOD_CTRL)) {
                    const Rml::Element* focus = context->GetFocusElement();
                    const Rml::String   tag   = focus ? focus->GetTagName() : Rml::String();
                    if (tag != "input" && tag != "select" && tag != "textarea") {
                        copy(ui.tab == "topics" ? ui.sel_name : ui.selected.name);
                        break;
                    }
                }
                RmlSDL::InputEventHandler(context, window, event);
                break;
            default:
                RmlSDL::InputEventHandler(context, window, event);
                break;
            }
        }

        if (watcher.changed()) reload_now = true;
        if (reload_now) {
            reload_now = false;
            system_interface.last_problem.clear();
            document = reload_document(context, document, document_path);
            if (document) listen(document);
            apply_body(document, ui);
            /* The fresh elements hold nothing yet, so every binding and both windows start over. */
            window_first = window_last = value_first = value_last = -1;
            ui.note       = document ? ("reloaded " + format_clock((uint64_t)std::time(nullptr) * 1000000u)) : "reload failed";
            ui.note_frames = 300;
            std::fprintf(stderr, "%s %s", document ? "reloaded" : "reload FAILED",
                         document_path.c_str());
            std::fputc(10, stderr);
            std::fflush(stderr);
            model.DirtyAllVariables();
        }
        if (ui.note_frames && --ui.note_frames == 0) ui.note.clear();

        capture.poll();

        /* note, sel and meta are marked every frame at the end: sel and meta count live
           seconds, and note is written from everywhere. The rest move only when changed. */
        sync(model, "peer_count", ui.peer_count, capture.node_count());
        ui.machines = capture.machines();
        sync(model, "machines", machines_sig, machines_signature(ui.machines));
        sync(model, "publishes", ui.publishes, capture.publishes());
        sync(model, "subscribes", ui.subscribes, capture.subscribes());
        sync(model, "log", ui.log, capture.log());
        ui.meta = capture.meta();
        if (capture.ok() && !capture.latest_log().text.empty()) {
            const Capture::LogRow& latest = capture.latest_log();
            sync(model, "status", ui.status, latest.time + "  " + latest.node + "  " + latest.text);
            sync(model, "status_level", ui.status_level, latest.level);
        }

        ui.selected = Capture::NodeRow();
        for (const Capture::MachineRow& machine : ui.machines)
            for (const Capture::NodeRow& row : machine.nodes)
                if (row.id == capture.selected()) ui.selected = row;
        sync(model, "has_selection", ui.has_selection, !ui.selected.id.empty());

        /* The state boxes follow subscriptions and arrivals, so while one is checked the
           tree is rebuilt whenever a topic passes differently. */
        if (ui.topic_cats & (CAT_SUBSCRIBED | CAT_ACTIVE)) {
            std::vector<bool> passing;
            passing.reserve(capture.topics().size());
            for (const Capture::TopicRow& topic : capture.topics())
                passing.push_back(topic_passes(capture, topic, std::string(), (unsigned)ui.topic_cats));
            if (passing != tree_passing) {
                tree_passing.swap(passing);
                tree_dirty = true;
            }
        }
        if (capture.topics_epoch() != tree_epoch || ui.topic_filter != tree_filter || tree_dirty) {
            tree_epoch  = capture.topics_epoch();
            tree_filter = ui.topic_filter;
            tree_dirty  = false;
            int matched = 0;
            tree = build_tree(capture, ui.expanded, ui.topic_filter, (unsigned)ui.topic_cats, matched);
            window_first = window_last = -1;
            sync(model, "topic_matched", ui.topic_matched, matched);
            sync(model, "topic_count", ui.topic_count, (int)capture.topics().size());
            sync(model, "tree_total", ui.tree_total, (int)tree.size());
            sync(model, "filtering", ui.filtering, !ui.topic_filter.empty() || ui.topic_cats != 0);
        }
        fit_head(document, "topic-tree");
        fit_head(document, "value-tree");
        if (window_rows(context, document, "topic-tree", (int)tree.size(),
                        window_first, window_last, ui.tree_top, ui.tree_bottom)) {
            ui.tree_rows.assign(tree.begin() + window_first, tree.begin() + window_last);
            model.DirtyVariable("tree_rows");
            model.DirtyVariable("tree_top");
            model.DirtyVariable("tree_bottom");
        }
        /* The columns beside each row move per message and per second, so they are a parallel
           array rewritten only when a cell differs. A dash wherever nothing is subscribed. */
        {
            Rml::Vector<TreeCell> cells;
            cells.reserve(ui.tree_rows.size());
            for (const TreeRow& row : ui.tree_rows) {
                TreeCell cell;
                if (row.topic >= 0 && row.topic < (int)capture.topics().size()) {
                    const Capture::TopicRow&  topic = capture.topics()[row.topic];
                    const Capture::WatchView* view  = capture.view(topic.name);
                    cell.dot     = subscribable(topic);
                    cell.preview = cell.dot && capture.previewing(topic.name);
                    cell.sub     = cell.preview || (cell.dot && capture.subscribed(topic.name));
                    cell.active  = cell.sub && view && view->rate_hz > 0;
                    topic_columns(capture, topic, cell.value, cell.rate, cell.jitter);
                }
                cells.push_back(std::move(cell));
            }
            sync(model, "tree_cells", ui.tree_cells, std::move(cells));
        }

        const Capture::WatchView& watch = capture.watched();
        ValueView& view = editor.view();
        seed_view(view, watch);
        editor.repeat();
        const bool can_compose = !watch.writable && watch.kind == Capture::Kind::Topic && watch.draftable;
        const bool callable    = Capture::is_call(watch.kind);
        /* A composed tree shows the frozen draft, so a new message moves nothing in it. */
        const std::vector<Capture::ValueNode>& shown = editor.shown();
        /* A head flag that moved rebuilds the tree too, since the editors follow it. */
        bool rebuild = value_dirty;
        rebuild |= editor.take_rows();
        rebuild |= sync(model, "value_writable", ui.value_writable, watch.writable);
        rebuild |= sync(model, "value_can_compose", ui.value_can_compose, can_compose);
        rebuild |= sync(model, "value_kind", ui.value_kind, watch.kind);
        if (rebuild || watch.name != value_name || (watch.shape != value_shape && !view.composing)) {
            value_name  = watch.name;
            value_shape = watch.shape;
            value_dirty = false;
            sync(model, "value_composing", ui.value_composing, view.composing);
            sync(model, "value_repeat", ui.value_repeat, view.repeat);
            sync(model, "value_repeating", ui.value_repeating, view.repeating);
            value_rows = build_value_rows(shown, view, watch.writable || view.composing || callable);
            sync(model, "value_total", ui.value_total, (int)value_rows.size());
            /* The head holds the root's toggle only when the root has no row of its own. */
            const bool root_row = !value_rows.empty() && value_rows[0].path.empty();
            sync(model, "value_root_toggle", ui.value_root_toggle, !watch.nodes.empty() && !root_row && !callable);
            sync(model, "value_root_on", ui.value_root_on, view.shown.count("") > 0);
            /* The selected topic's cards in tree order, then every other topic's pinned ones.
               Pinning never moves a card, since it keeps its place among the selected's. */
            ui.cards.clear();
            auto card = [&ui, &watch](const std::string& topic, const std::string& path, const ValueView& of) {
                CardRow row;
                row.topic  = topic;
                row.path   = path;
                row.name   = path.empty() ? "value" : path;
                row.pinned = of.pinned.count(path) > 0;
                row.other  = topic != watch.name;
                ui.cards.push_back(row);
            };
            if (!watch.nodes.empty() && ui.value_root_on && !callable) card(watch.name, "", view);
            for (const Capture::ValueNode& node : watch.nodes)
                if (node.kind != Capture::ValueNode::Gap && node.kind != Capture::ValueNode::Part &&
                    !node.path.empty() && view.shown.count(node.path))
                    card(watch.name, node.path, view);
            for (const auto& entry : editor.views())
                if (entry.first != watch.name && capture.view(entry.first))
                    for (const std::string& path : entry.second.pinned) card(entry.first, path, entry.second);
            /* Past the most the pane tiles, the root's default card goes first, since the
               user never asked for it, then the last ones. */
            if (ui.cards.size() > (size_t)MAX_CARDS && !ui.cards.empty() && ui.cards[0].path.empty() &&
                !ui.cards[0].other && !ui.cards[0].pinned)
                ui.cards.erase(ui.cards.begin());
            if (ui.cards.size() > (size_t)MAX_CARDS) ui.cards.resize(MAX_CARDS);
            value_first = value_last = -1;
            value_seq   = ~watch.seq;   /* the texts are rewritten below */
            model.DirtyVariable("cards");
        }
        /* Traces follow the cards: the selected topic's shown set, another's pinned set. */
        for (const auto& entry : editor.views()) {
            const Capture::WatchView* sub = capture.view(entry.first);
            if (!sub) {
                traced.erase(entry.first);
                continue;
            }
            const std::set<std::string>& paths = entry.first == watch.name ? entry.second.shown : entry.second.pinned;
            std::string key = sub->shape + "|";
            for (const std::string& path : paths) key += path + ",";
            if (traced[entry.first] == key) continue;
            traced[entry.first] = key;
            const VizFeeds feeds = viz_feeds(*sub, paths);
            capture.trace(entry.first, feeds.traces);
            capture.stream(entry.first, feeds.streams);
        }

        const bool value_moved = window_rows(context, document, "value-tree",
                                             (int)value_rows.size(), value_first, value_last,
                                             ui.value_top, ui.value_bottom);
        if (value_moved) {
            ui.value_rows.assign(value_rows.begin() + value_first, value_rows.begin() + value_last);
            model.DirtyVariable("value_rows");
            model.DirtyVariable("value_top");
            model.DirtyVariable("value_bottom");
        }
        editor.settle(watch.seq != value_seq);
        /* Per message and per edit only the cells move, and the elements showing them stay.
           An editor shows what the user typed, else what was written, else the mesh's value. */
        auto make_cell = [&view](const ValueRow& row, const Capture::ValueNode& node) {
            ValueCell cell;
            const auto edit  = view.edits.find(node.path);
            const auto wrote = view.writing.find(node.path);
            if (row.part)                      { cell.text = node.text; cell.tone = node.tone; }
            else if (row.editor.empty())       cell.text = row.blank && !row.branch ? Rml::String(DASH) : value_text(node);
            else if (edit != view.edits.end()) { cell.text = edit->second.text; cell.edited = edit->second.mine; cell.bad = edit->second.bad; }
            else if (wrote != view.writing.end()) { cell.text = wrote->second; cell.writing = true; }
            else                               cell.text = edit_text(node);
            cell.size = std::max(2, (int)cell.text.size());
            return cell;
        };
        const bool cells_moved = editor.take_cells();
        if (value_moved || watch.seq != value_seq || cells_moved) {
            value_seq = watch.seq;
            Rml::Vector<ValueCell> cells;
            cells.reserve(ui.value_rows.size());
            for (const ValueRow& row : ui.value_rows)
                cells.push_back(row.node >= 0 && row.node < (int)shown.size()
                                ? make_cell(row, shown[row.node]) : ValueCell());
            ui.value_cells.swap(cells);
            model.DirtyVariable("value_cells");
            int  count = 0;
            bool bad   = false;
            for (const auto& entry : view.edits) {
                count += entry.second.mine ? 1 : 0;
                bad = bad || entry.second.bad;
            }
            /* A draft counts what it sent, a variable what waits to be set. */
            const Rml::String note = view.composing ? (view.sent ? std::to_string(view.sent) + " sent" : Rml::String())
                                   : !view.writing.empty() ? Rml::String("writing")
                                   : count ? std::to_string(count) + " edited" : Rml::String();
            sync(model, "value_edit_count", ui.value_edit_count, count);
            sync(model, "value_bad", ui.value_bad, bad);
            sync(model, "value_note", ui.value_note, note);
            sync(model, "value_problem", ui.value_problem, !editor.error().empty() ? editor.error() : watch.error);
        }
        sync(model, "value_calling", ui.value_calling, watch.calling);
        sync(model, "value_progress", ui.value_progress,
             watch.progress >= 0 ? Rml::CreateString("%.1f%%", watch.progress * 100) : Rml::String());
        sync(model, "value_type", ui.value_type, watch.type);
        sync(model, "value_status", ui.value_status, watch.status);

        Capture::TopicRow topic;
        for (const Capture::TopicRow& row : capture.topics())
            if (row.name == ui.sel_name) topic = row;
        sync(model, "has_topic", ui.has_topic, !topic.name.empty());
        sync(model, "topic", ui.topic, std::move(topic));

        /* A bad selector or a missing file lands here, where a windowed build can show it. */
        if (!system_interface.last_problem.empty()) {
            ui.note        = system_interface.last_problem;
            ui.note_frames = 300;
            system_interface.last_problem.clear();
        }

        model.DirtyVariable("note");
        model.DirtyVariable("sel");
        model.DirtyVariable("meta");

        context->Update();
        /* New cards exist only after the update, so a moved tiling lays out once more before
           it draws, and no card shows a frame in the wrong place. */
        if (tile_cards(context, document, tiling)) context->Update();

        /* Clear before BeginFrame: Clear sets an opaque clear colour for the window and
           BeginFrame a transparent one for layers. Reversed, every filter layer is cleared
           to opaque black. */
        render_interface.Clear();
        render_interface.BeginFrame();
        context->Render();
        render_interface.EndFrame();
        SDL_GL_SwapWindow(window);
    }

    Rml::Debugger::Shutdown();
    Rml::Shutdown();
    RmlGL3::Shutdown();
    SDL_GL_DestroyContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
