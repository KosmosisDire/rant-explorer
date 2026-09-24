/* The value tree: the watched topic's latest value as the rows the right sidebar binds,
   honouring what the user folded and which fields show a visual. */
#ifndef VALUES_HPP
#define VALUES_HPP

#include "capture.hpp"
#include "image.hpp"

#include <map>
#include <set>
#include <string>
#include <vector>

struct ValueRow {
    std::string name;
    std::string type;        /* a branch adds a variable array's length */
    std::string indent;      /* tree_indent of its depth */
    std::string path;        /* the field's path, the key for folding and for its visual */
    std::string editor;      /* text, bool or enum while the value can be written, else empty */
    std::vector<std::string> options;   /* an enum editor's choices */
    std::string width = "auto";   /* an enum editor's width, its longest option */
    int         node = -1;   /* index into the value nodes the rows were built from */
    bool        branch = false, open = false, gap = false, on = false;
    bool        part = false;    /* the heading of a function's or task's part */
    bool        blank = false;   /* in a part with nothing in it yet, so the value is a dash */
    bool        scrub = false;   /* a number editor: dragging the name steps it */
    bool        odd = false;   /* every other row is striped */
};

/* What one row's value shows this frame, parallel to the rows and rewritten per message
   and per edit, so the row elements stay. */
struct ValueCell {
    std::string text;
    int         size = 1;           /* a text editor's width in characters */
    bool        edited = false;     /* the user changed it and has not written it */
    bool        bad = false;        /* the typed text is not a value of the field's type */
    bool        writing = false;    /* written, and the owner has not echoed it yet */
    int         tone = 0;           /* a part's state: 0 plain, 1 ok, 2 bad, 3 busy */
};

/* One field the user changed: the typed text, and the value it parses to. A field of a
   draft reset to the mesh's newer value is an edit too, but not the user's, so no mark. */
struct FieldEdit {
    std::string         text;
    Capture::ValueNode  value;
    bool                bad = false;
    bool                mine = true;
};

/* What the user did to one topic's tree, kept per topic while the explorer runs. */
struct ValueView {
    bool                        seeded = false;   /* the shown set has had its default */
    std::set<std::string>       shown;            /* the paths whose visual is in the detail */
    std::set<std::string>       pinned;           /* of those, the ones kept while another topic shows */
    std::map<std::string, bool> open;             /* the branches the user opened or closed */
    std::map<std::string, FieldEdit>   edits;     /* by path, until they are written or reset */
    std::map<std::string, std::string> writing;   /* by path, the text written until the echo */
    double                      writing_since = 0;

    /* A topic being composed: the message it froze, which the edits are written over. */
    bool                            composing = false;
    std::vector<Capture::ValueNode> frozen;
    std::vector<uint8_t>            frozen_message;
    uint64_t                        frozen_schema = 0;
    bool                            repeat = false;      /* Send repeats at the rate until Stop */
    bool                            repeating = false;
    double                          next_send = 0;
    int                             sent = 0;
};

/* What a field is drawn as in its card, from the mock's table: the standard types by
   name, everything else by shape. Kv is the fallback, a plain list of the fields. */
enum class Visual {
    None, Plot, State, Text, Time, Duration, Hex, Chips, Bars, Cells, Table, Matrix, Multi,
    Vec2, Vec3, Quat, Transform, Twist, Geo, Color, Rect, Joints, Image, Video,
    Vec2s, Vec3s, Transforms, Geos, Rects, Colors, Kv,
};
Visual visual_for(const Capture::ValueNode& node);

/* The visual of the node at index, as find_node gives it. A struct root has no node of its
   own, so its visual comes from the root type. None for no node. */
Visual visual_at(const Capture::WatchView& watch, const std::vector<Capture::ValueNode>& nodes, int index);

/* The first time a topic shows: its root's visual is on when the root type has one. */
void seed_view(ValueView& view, const Capture::WatchView& watch);

/* The rows in tree order, skipping the children of closed branches. A struct root has no
   row of its own and neither has a bare array, whose elements are the top level. A bare
   scalar is one row named Value, and so is a part's bare root. While editable, every leaf
   that can be written takes its editor, in a function or task only the input part's. */
std::vector<ValueRow> build_value_rows(const std::vector<Capture::ValueNode>& nodes,
                                       const ValueView& view, bool editable);

/* A branch starts open unless it is a long array. The user's choice wins after that. */
bool is_open(const Capture::ValueNode& node, const ValueView& view);

/* What the value column shows for one node, empty for a branch. */
std::string value_text(const Capture::ValueNode& node);

/* The newest value on one line for the topic tree: a bare root's value, a standard
   struct's fields, an image's or a video's size. A plain struct stays blank. */
std::string preview_text(const Capture::WatchView& view);

/* The nodes directly under index, -1 for the top level. */
std::vector<int> children_of(const std::vector<Capture::ValueNode>& nodes, int index);

/* The node at path, -1 for a struct root, which has no node of its own, -2 for none. */
int find_node(const std::vector<Capture::ValueNode>& nodes, const Capture::WatchView& view,
              const std::string& path);

/* The pixels of the standard Image at index, from the message its nodes were decoded
   from. Invalid when they cannot be read, with the reason in problem. */
Image image_of(const std::vector<Capture::ValueNode>& nodes, int index, const std::vector<uint8_t>& message,
               std::string* problem = nullptr);

/* What copying a field gives: a standard type in the form other programs read (ISO 8601
   UTC, seconds, #rrggbb, "lat, lon"), else the raw data. */
std::string copy_text(const std::vector<Capture::ValueNode>& nodes, const Capture::WatchView& view,
                      const std::string& path);
/* The raw data: a leaf's stored value exact, a string as is, bytes as hex, a branch as
   indented JSON. */
std::string copy_raw(const std::vector<Capture::ValueNode>& nodes, const Capture::WatchView& view,
                     const std::string& path);
/* A branch as rows of cells: a struct as name and value, a matrix as its grid, an array of
   structs under a header row of field names, any other array as one column. A nested cell
   is one line of JSON, and no cell holds a tab or a line break. No rows for a leaf. */
struct Table {
    std::vector<std::vector<std::string>> rows;
    bool header = false;   /* the first row names the columns */
};
Table table_of(const std::vector<Capture::ValueNode>& nodes, const Capture::WatchView& view,
               const std::string& path);
/* Rows as JSON, every cell a string: objects keyed by the header when there is one, one
   object of names to values for two columns, else arrays. */
std::string table_json(const Table& table);

/* Which editor a node takes when its value can be written, empty for none. */
std::string editor_for(const Capture::ValueNode& node);

/* What an editor shows: the pill's text, except that an empty string stays empty. */
std::string edit_text(const Capture::ValueNode& node);

/* A typed text read back into a copy of the node. False when the text is not a value of
   the node's type: a number out of range, an unknown option, a string over its capacity. */
bool parse_value(const Capture::ValueNode& node, const std::string& text, Capture::ValueNode& out);

/* A number's text moved by whole steps: one for an integer, else a round step near a
   hundredth of the value, ten times that when big. Held inside an integer's range. Empty
   when from is not a number of the node's type. */
std::string step_value(const Capture::ValueNode& node, const std::string& from, int steps, bool big);

#endif /* VALUES_HPP */
