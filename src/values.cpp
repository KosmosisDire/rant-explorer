#include "values.hpp"

#include "format.hpp"
#include "image.hpp"
#include "tree.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

using ValueNode = Capture::ValueNode;

/* The standard structs with a visual of their own. The rest read as a list of fields,
   CameraIntrinsics among them. */
Visual struct_visual(const std::string& name)
{
    static const struct { const char* name; Visual visual; } table[] = {
        { "Float2", Visual::Vec2 }, { "Double2", Visual::Vec2 }, { "Int2", Visual::Vec2 },
        { "Float3", Visual::Vec3 }, { "Double3", Visual::Vec3 }, { "Int3", Visual::Vec3 },
        { "Quaternion", Visual::Quat }, { "Transform", Visual::Transform },
        { "Twist", Visual::Twist }, { "GeoPoint", Visual::Geo }, { "Color", Visual::Color },
        { "Rect", Visual::Rect }, { "RectI", Visual::Rect },
        { "JointState", Visual::Joints }, { "Image", Visual::Image },
        { "VideoFrame", Visual::Video },
    };
    for (const auto& entry : table)
        if (name == entry.name) return entry.visual;
    return Visual::Kv;
}

/* An array of a standard struct: the ones sharing a space overlay in one picture, the
   rest become a grid of small copies. */
Visual array_of_struct(const std::string& name)
{
    static const struct { const char* name; Visual visual; } table[] = {
        { "Float3", Visual::Vec3s }, { "Double3", Visual::Vec3s }, { "Int3", Visual::Vec3s },
        { "Float2", Visual::Vec2s }, { "Double2", Visual::Vec2s }, { "Int2", Visual::Vec2s },
        { "Transform", Visual::Transforms }, { "GeoPoint", Visual::Geos },
        { "Rect", Visual::Rects }, { "RectI", Visual::Rects }, { "Color", Visual::Colors },
    };
    for (const auto& entry : table)
        if (name == entry.name) return entry.visual;
    return struct_visual(name) == Visual::Kv ? Visual::Table : Visual::Multi;
}

bool is_branch(const ValueNode& node)
{
    return node.kind == ValueNode::Struct || node.kind == ValueNode::Array;
}

} /* namespace */

Visual visual_for(const ValueNode& node)
{
    switch (node.kind) {
    case ValueNode::Number:
        if (node.std_name == "Timestamp") return Visual::Time;
        if (node.std_name == "Duration")  return Visual::Duration;
        return Visual::Plot;
    case ValueNode::Bool:
    case ValueNode::Enum:   return Visual::State;
    case ValueNode::Text:   return Visual::Text;
    case ValueNode::Blob:   return Visual::Hex;
    case ValueNode::Struct: return struct_visual(node.std_name);
    case ValueNode::Array:
        if (node.std_name.compare(0, 6, "Matrix") == 0) return Visual::Matrix;
        switch (node.elem) {
        case ValueNode::Number: return node.elem_std.empty() ? Visual::Bars : Visual::Chips;
        case ValueNode::Bool:   return Visual::Cells;
        case ValueNode::Text:   return Visual::Chips;
        default:                return array_of_struct(node.elem_std);
        }
    default:                return Visual::None;
    }
}

Visual visual_at(const Capture::WatchView& watch, const std::vector<ValueNode>& nodes, int index)
{
    if (index == -1) return struct_visual(watch.root_std);
    return index >= 0 ? visual_for(nodes[index]) : Visual::None;
}

/* A plain struct only lists its fields, which is no visual of its own. */
void seed_view(ValueView& view, const Capture::WatchView& watch)
{
    if (view.seeded || watch.nodes.empty()) return;
    view.seeded = true;
    const Visual root = visual_at(watch, watch.nodes, find_node(watch.nodes, watch, ""));
    if (root != Visual::None && root != Visual::Kv) view.shown.insert("");
}

bool is_open(const ValueNode& node, const ValueView& view)
{
    const auto choice = view.open.find(node.path);
    if (choice != view.open.end()) return choice->second;
    return !(node.kind == ValueNode::Array && node.count > Capture::ARRAY_WHOLE);
}

std::vector<ValueRow> build_value_rows(const std::vector<ValueNode>& nodes, const ValueView& view,
                                       bool editable)
{
    std::vector<ValueRow> rows;
    const bool parts = std::any_of(nodes.begin(), nodes.end(),
                                   [](const ValueNode& n) { return n.kind == ValueNode::Part; });
    std::string part_root;   /* the path of the current part's bare root */
    bool part_blank = false, part_input = true;
    const bool bare_branch = !nodes.empty() && nodes[0].path.empty() && is_branch(nodes[0]);
    const int  lift = bare_branch ? 1 : 0;   /* a bare array's elements are the top level */
    int closed_at = INT_MAX;   /* the depth of the closed branch whose children are skipped */
    for (size_t i = bare_branch ? 1 : 0; i < nodes.size(); i++) {
        const ValueNode& node = nodes[i];
        if (node.depth > closed_at) continue;
        closed_at = INT_MAX;

        ValueRow row;
        row.node   = (int)i;
        if (node.kind == ValueNode::Part) {
            part_root  = node.path.substr(1);
            part_blank = node.count == 0;
            part_input = node.input;
            closed_at  = INT_MAX;
            row.part   = true;
            row.name   = node.name;
            row.path   = node.path;
            row.type   = node.type;
            row.indent = tree_indent(0);
            row.odd    = rows.size() % 2 == 1;
            rows.push_back(std::move(row));
            continue;
        }
        row.blank  = part_blank;
        row.name   = node.path.empty() || (parts && node.path == part_root) ? std::string("Value") : node.name;
        row.path   = node.path;
        row.indent = tree_indent(node.depth - lift);
        row.gap    = node.kind == ValueNode::Gap;
        row.branch = is_branch(node);
        row.open   = row.branch && is_open(node, view);
        row.on     = !row.gap && view.shown.count(node.path) > 0;
        if (row.gap)
            row.name = "... " + format_number(node.count) + " more";   /* in the one column never hidden */
        else if (node.kind == ValueNode::Array && node.type.size() > 2 &&
                 node.type.compare(node.type.size() - 2, 2, "[]") == 0)
            row.type = node.type + " " + format_number(node.count);   /* a variable array's length */
        else
            row.type = node.type;
        if (editable && !row.gap && !row.branch && (!parts || part_input)) row.editor = editor_for(node);
        row.scrub = row.editor == "text" && node.kind == ValueNode::Number;
        if (row.editor == "enum") {
            size_t longest = 1;
            for (const std::string& option : node.options) longest = std::max(longest, option.size());
            row.options = node.options;
            /* the mono advance is about 0.55em, plus the padding and the chevron */
            row.width = std::to_string(0.56 * longest + 2.2) + "em";
        }
        if (row.branch && !row.open) closed_at = node.depth;
        row.odd = rows.size() % 2 == 1;
        rows.push_back(std::move(row));
    }
    return rows;
}

std::string value_text(const ValueNode& node)
{
    char buf[48];
    switch (node.kind) {
    case ValueNode::Number:
        if (node.is_float)         std::snprintf(buf, sizeof buf, "%.3f", node.number);
        else if (node.is_unsigned) std::snprintf(buf, sizeof buf, "%llu", (unsigned long long)node.integer);
        else                       std::snprintf(buf, sizeof buf, "%lld", (long long)node.integer);
        return buf;
    case ValueNode::Bool:
        return node.number != 0 ? "true" : "false";
    case ValueNode::Enum:
        if (!node.text.empty()) return node.text;
        std::snprintf(buf, sizeof buf, "%lld", (long long)node.integer);
        return buf;
    case ValueNode::Text:
        return node.text.empty() ? "empty" : node.text;
    case ValueNode::Blob:
        return format_number(node.count) + (node.count == 1 ? " byte" : " bytes");
    case ValueNode::Map:
        return format_number(node.count) + (node.count == 1 ? " entry" : " entries");
    default:
        return {};
    }
}

namespace {

/* A number as short as it reads clearly, for a one line preview. */
std::string compact(const ValueNode& node)
{
    char buf[32];
    if (node.kind == ValueNode::Number && node.is_float) {
        std::snprintf(buf, sizeof buf, "%.4g", node.number);
        return buf;
    }
    return value_text(node);
}

} /* namespace */

std::string preview_text(const Capture::WatchView& view)
{
    constexpr size_t LIMIT = 48;
    if (view.nodes.empty()) return {};
    std::string out;
    auto add = [&out](const std::string& text) {
        if (!out.empty()) out += ", ";
        out += text;
    };
    const ValueNode& root = view.nodes[0];
    if (!view.root_struct && root.kind == ValueNode::Array) {
        if (root.elem == ValueNode::Struct) return format_number(root.count) + " x " + view.type;
        for (size_t i = 1; i < view.nodes.size() && out.size() < LIMIT; i++)
            if (view.nodes[i].depth == 1 && view.nodes[i].kind != ValueNode::Gap) add(compact(view.nodes[i]));
        out = "[" + out + (root.count > 8 ? ", ...]" : "]");
    } else if (!view.root_struct) {
        out = compact(root);
    } else if (view.root_std == "Image" || view.root_std == "VideoFrame") {
        int64_t width = 0, height = 0;
        std::string format;
        for (const ValueNode& n : view.nodes) {
            if (n.name == "width")  width  = n.integer;
            if (n.name == "height") height = n.integer;
            if (n.name == "format" || n.name == "codec") format = value_text(n);
        }
        /* a video may leave its size to the bitstream */
        out = width ? std::to_string(width) + "x" + std::to_string(height) + " " + format : format;
    } else if (!view.root_std.empty()) {
        for (const ValueNode& n : view.nodes) {
            if (out.size() >= LIMIT) break;
            if (n.kind == ValueNode::Number || n.kind == ValueNode::Bool || n.kind == ValueNode::Enum ||
                n.kind == ValueNode::Text)
                add(compact(n));
        }
    }
    for (char& c : out)
        if ((unsigned char)c < 32) c = ' ';
    if (out.size() > LIMIT) out = out.substr(0, LIMIT) + "...";
    return out;
}

std::string editor_for(const ValueNode& node)
{
    switch (node.kind) {
    case ValueNode::Number:
    case ValueNode::Text: return "text";
    case ValueNode::Bool: return "bool";
    case ValueNode::Enum: return node.options.empty() ? "text" : "enum";
    default:              return {};
    }
}

std::string edit_text(const ValueNode& node)
{
    return node.kind == ValueNode::Text ? node.text : value_text(node);
}

bool parse_value(const ValueNode& node, const std::string& text, ValueNode& out)
{
    out = node;
    switch (node.kind) {
    case ValueNode::Bool:
        if (text != "true" && text != "false") return false;
        out.number = text == "true" ? 1 : 0;
        return true;
    case ValueNode::Enum:
        for (size_t k = 0; k < node.options.size(); k++) {
            if (node.options[k] != text) continue;
            out.integer = node.option_values[k];
            out.number  = (double)out.integer;
            out.text    = text;
            return true;
        }
        return false;
    case ValueNode::Text:
        if (node.cap && text.size() > node.cap) return false;
        out.text = text;
        return true;
    case ValueNode::Number: {
        if (text.empty()) return false;
        const char* start = text.c_str();
        char* end = nullptr;
        errno = 0;
        if (node.is_float) {
            const double v = std::strtod(start, &end);
            if (*end || errno || !std::isfinite(v)) return false;
            out.number = v;
            return true;
        }
        /* The width comes from the type word, u8 through i64. A named integer such as a
           Timestamp is 64 bits. */
        int bits = 64;
        if (node.type.size() >= 2 && (node.type[0] == 'u' || node.type[0] == 'i'))
            bits = std::atoi(node.type.c_str() + 1);
        if (bits != 8 && bits != 16 && bits != 32) bits = 64;
        if (node.is_unsigned) {
            if (text[0] == '-') return false;
            const unsigned long long v = std::strtoull(start, &end, 10);
            if (*end || errno || (bits < 64 && v >> bits)) return false;
            out.integer = (int64_t)v;
            out.number  = (double)v;
        } else {
            const long long v = std::strtoll(start, &end, 10);
            const long long top = bits < 64 ? (1LL << (bits - 1)) - 1 : LLONG_MAX;
            if (*end || errno || v > top || v < -top - 1) return false;
            out.integer = v;
            out.number  = (double)v;
        }
        return true;
    }
    default:
        return false;
    }
}

std::string step_value(const ValueNode& node, const std::string& from, int steps, bool big)
{
    ValueNode at;
    if (node.kind != ValueNode::Number || !parse_value(node, from, at)) return {};
    char buf[48];
    if (node.is_float) {
        const double v    = at.number;
        const double base = std::max(0.001, std::pow(10.0, std::floor(std::log10(std::max(std::abs(v), 0.01)))) / 100);
        const double next = std::round((v + steps * base * (big ? 10 : 1)) * 1e6) / 1e6;
        std::snprintf(buf, sizeof buf, "%.6g", next);
        return buf;
    }
    /* the same width rule as parse_value, so a step never leaves the type */
    int bits = 64;
    if (node.type.size() >= 2 && (node.type[0] == 'u' || node.type[0] == 'i')) bits = std::atoi(node.type.c_str() + 1);
    if (bits != 8 && bits != 16 && bits != 32) bits = 64;
    const long long delta = (long long)steps * (big ? 10 : 1);
    if (node.is_unsigned) {
        const unsigned long long top = bits < 64 ? (1ull << bits) - 1 : ULLONG_MAX;
        const unsigned long long v   = (unsigned long long)at.integer;
        const unsigned long long next = delta < 0 ? (v < (unsigned long long)-delta ? 0 : v + delta)
                                                  : (top - v < (unsigned long long)delta ? top : v + delta);
        std::snprintf(buf, sizeof buf, "%llu", next);
    } else {
        const long long top = bits < 64 ? (1LL << (bits - 1)) - 1 : LLONG_MAX;
        const long long v   = at.integer;
        const long long next = delta < 0 ? (v < -top - 1 - delta ? -top - 1 : v + delta)
                                         : (v > top - delta ? top : v + delta);
        std::snprintf(buf, sizeof buf, "%lld", next);
    }
    return buf;
}

std::vector<int> children_of(const std::vector<ValueNode>& nodes, int index)
{
    std::vector<int> out;
    const int depth = index < 0 ? -1 : nodes[index].depth;
    for (size_t j = (size_t)(index + 1); j < nodes.size() && nodes[j].depth > depth; j++)
        if (nodes[j].depth == depth + 1) out.push_back((int)j);
    return out;
}

int find_node(const std::vector<ValueNode>& nodes, const Capture::WatchView& view, const std::string& path)
{
    if (path.empty() && view.root_struct && !nodes.empty()) return -1;
    for (size_t i = 0; i < nodes.size(); i++)
        if (nodes[i].path == path) return (int)i;
    return -2;
}

Image image_of(const std::vector<ValueNode>& nodes, int index, const std::vector<uint8_t>& message,
               std::string* problem)
{
    uint32_t width = 0, height = 0, stride = 0;
    int format = -1;
    const ValueNode* data = nullptr;
    for (const int k : children_of(nodes, index)) {
        const ValueNode& e = nodes[k];
        if (e.name == "width")  width  = (uint32_t)e.integer;
        if (e.name == "height") height = (uint32_t)e.integer;
        if (e.name == "stride") stride = (uint32_t)e.integer;
        if (e.name == "format") format = (int)e.integer;
        if (e.name == "data" && e.kind == ValueNode::Blob) data = &e;
    }
    const bool held = data && (size_t)data->offset + data->count <= message.size();
    Image image = held ? image_from_frame(width, height, stride, format, message.data() + data->offset, data->count)
                       : Image();
    if (!image.valid() && problem)
        *problem = !data || !data->count ? "no pixels" : std::string(image_format_name(format)) + " frame not readable";
    return image;
}

namespace {

/* A float in the fewest digits that read back to the same value. */
std::string exact(const ValueNode& node)
{
    if (node.kind != ValueNode::Number || !node.is_float) return edit_text(node);
    char buf[40];
    const auto end = node.type == "f32" ? std::to_chars(buf, buf + sizeof buf, (float)node.number).ptr
                                        : std::to_chars(buf, buf + sizeof buf, node.number).ptr;
    return std::string(buf, end);
}

/* A blob's whole bytes from the message, or the head the node kept when they are not there. */
std::string blob_hex(const ValueNode& node, const std::vector<uint8_t>& message)
{
    const bool whole = (size_t)node.offset + node.count <= message.size() && node.count > 0;
    return whole ? format_hex(message.data() + node.offset, node.count)
                 : format_hex((const uint8_t*)node.text.data(), node.text.size());
}

std::string quote(const std::string& text)
{
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if ((unsigned char)c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", (unsigned)c);
            out += buf;
        } else {
            out += c;
        }
    }
    return out + "\"";
}

/* A part heads its fields the way a struct does, so it copies as an object. */
bool is_container(int index, const std::vector<ValueNode>& nodes)
{
    return index < 0 || is_branch(nodes[index]) || nodes[index].kind == ValueNode::Part;
}

/* A leaf as it pastes on its own: bytes as hex, a string raw, a number exact. */
std::string leaf(const ValueNode& node, const std::vector<uint8_t>& message)
{
    if (node.kind == ValueNode::Blob) return blob_hex(node, message);
    if (node.kind == ValueNode::Text) return node.text;
    return exact(node);
}

/* Appends the node at index, -1 for a struct root, as JSON. A negative indent writes it
   on one line. */
void json(const std::vector<ValueNode>& nodes, int index, int indent, const std::vector<uint8_t>& message,
          std::string& out)
{
    if (!is_container(index, nodes)) {
        const ValueNode& node = nodes[index];
        switch (node.kind) {
        case ValueNode::Number: out += std::isfinite(node.number) ? exact(node) : "null"; break;
        case ValueNode::Bool:   out += node.number != 0 ? "true" : "false"; break;
        case ValueNode::Enum:   out += node.text.empty() ? exact(node) : quote(node.text); break;
        default:                out += quote(leaf(node, message)); break;
        }
        return;
    }
    const bool list = index >= 0 && nodes[index].kind == ValueNode::Array;
    bool first = true;
    out += list ? "[" : "{";
    for (const int k : children_of(nodes, index)) {
        if (nodes[k].kind == ValueNode::Gap) continue;
        if (indent < 0) {
            out += first ? "" : ", ";
        } else {
            out += first ? "\n" : ",\n";
            out += std::string(indent + 2, ' ');
        }
        first = false;
        if (!list) out += quote(nodes[k].name) + ": ";
        json(nodes, k, indent < 0 ? indent : indent + 2, message, out);
    }
    if (!first && indent >= 0) out += "\n" + std::string(indent, ' ');
    out += list ? "]" : "}";
}

/* One table cell: a leaf as it copies alone, a branch as one line of JSON, with no tab or
   line break left to split the table. */
std::string cell(const std::vector<ValueNode>& nodes, int index, const std::vector<uint8_t>& message)
{
    std::string out;
    if (is_container(index, nodes)) json(nodes, index, -1, message, out);
    else out = leaf(nodes[index], message);
    std::replace(out.begin(), out.end(), '\t', ' ');
    std::replace(out.begin(), out.end(), '\n', ' ');
    std::replace(out.begin(), out.end(), '\r', ' ');
    return out;
}

/* A standard type in the form other programs read, empty for a type with none. */
std::string standard(const std::vector<ValueNode>& nodes, int index, const std::string& std_name,
                     const std::vector<uint8_t>& message)
{
    const ValueNode* node = index >= 0 ? &nodes[index] : nullptr;
    if (node && node->kind == ValueNode::Number) {
        if (std_name == "Timestamp") return format_iso_time(node->integer);
        if (std_name == "Duration") {
            char buf[40];
            std::snprintf(buf, sizeof buf, "%.6f", node->integer / 1e6);
            return buf;
        }
        return {};
    }
    std::map<std::string, const ValueNode*> field;
    for (const int k : children_of(nodes, index)) field[nodes[k].name] = &nodes[k];
    auto get = [&field](const char* name) { return field.count(name) ? field[name] : nullptr; };
    if (std_name == "Color" && get("r") && get("g") && get("b") && get("a")) {
        char buf[16];
        const int a = (int)get("a")->integer;
        std::snprintf(buf, sizeof buf, a == 255 ? "#%02x%02x%02x" : "#%02x%02x%02x%02x", (int)get("r")->integer,
                      (int)get("g")->integer, (int)get("b")->integer, a);
        return buf;
    }
    if (std_name == "GeoPoint" && get("lat") && get("lon")) return exact(*get("lat")) + ", " + exact(*get("lon"));
    return {};
}

} /* namespace */

std::string copy_text(const std::vector<ValueNode>& nodes, const Capture::WatchView& view, const std::string& path)
{
    const int index = find_node(nodes, view, path);
    if (index < -1) return {};
    const std::string std_name = index < 0 ? view.root_std : nodes[index].std_name;
    const std::string text = standard(nodes, index, std_name, view.message);
    return text.empty() ? copy_raw(nodes, view, path) : text;
}

std::string copy_raw(const std::vector<ValueNode>& nodes, const Capture::WatchView& view, const std::string& path)
{
    const int index = find_node(nodes, view, path);
    if (index < -1) return {};
    if (!is_container(index, nodes)) return leaf(nodes[index], view.message);
    std::string out;
    json(nodes, index, 0, view.message, out);
    return out;
}

Table table_of(const std::vector<ValueNode>& nodes, const Capture::WatchView& view, const std::string& path)
{
    Table table;
    const int index = find_node(nodes, view, path);
    if (index < -1 || !is_container(index, nodes)) return table;
    std::vector<int> rows;
    for (const int k : children_of(nodes, index))
        if (nodes[k].kind != ValueNode::Gap) rows.push_back(k);
    if (rows.empty()) return table;
    const bool list = index >= 0 && nodes[index].kind == ValueNode::Array;
    if (!list) {
        for (const int k : rows) table.rows.push_back({ nodes[k].name, cell(nodes, k, view.message) });
        return table;
    }
    if (nodes[index].std_name.compare(0, 6, "Matrix") == 0) {
        const size_t n = (size_t)std::lround(std::sqrt((double)rows.size()));
        if (n * n == rows.size()) {
            for (size_t r = 0; r < n; r++) {
                std::vector<std::string> cells;
                for (size_t c = 0; c < n; c++) cells.push_back(cell(nodes, rows[r * n + c], view.message));
                table.rows.push_back(cells);
            }
            return table;
        }
    }
    /* Elements with fields take a column per field, named by the first element's. */
    const std::vector<int> header = children_of(nodes, rows[0]);
    if (!is_container(rows[0], nodes) || nodes[rows[0]].kind == ValueNode::Array || header.empty()) {
        for (const int k : rows) table.rows.push_back({ cell(nodes, k, view.message) });
        return table;
    }
    table.header = true;
    table.rows.emplace_back();
    for (const int k : header) table.rows.back().push_back(nodes[k].name);
    for (const int e : rows) {
        std::vector<std::string> cells;
        for (const int k : children_of(nodes, e)) cells.push_back(cell(nodes, k, view.message));
        table.rows.push_back(cells);
    }
    return table;
}

std::string table_json(const Table& table)
{
    std::string out = "[";
    const size_t first = table.header ? 1 : 0;
    const bool   pairs = !table.header && std::all_of(table.rows.begin(), table.rows.end(),
                                                      [](const std::vector<std::string>& row) { return row.size() == 2; });
    if (pairs) out = "{";
    for (size_t r = first; r < table.rows.size(); r++) {
        const std::vector<std::string>& row = table.rows[r];
        out += r > first ? ",\n  " : "\n  ";
        if (pairs) {
            out += quote(row[0]) + ": " + quote(row[1]);
            continue;
        }
        out += table.header ? "{" : "[";
        for (size_t c = 0; c < row.size(); c++) {
            out += c ? ", " : "";
            if (table.header) out += quote(c < table.rows[0].size() ? table.rows[0][c] : std::to_string(c)) + ": ";
            out += quote(row[c]);
        }
        out += table.header ? "}" : "]";
    }
    return out + (table.rows.size() > first ? "\n" : "") + (pairs ? "}" : "]");
}
