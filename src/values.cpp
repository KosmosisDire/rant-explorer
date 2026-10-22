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
        { "Float4", Visual::Vec4 }, { "Double4", Visual::Vec4 }, { "Int4", Visual::Vec4 },
        { "Quaternion", Visual::Quat }, { "Pose", Visual::Pose }, { "Transform", Visual::Pose },
        { "Pose2D", Visual::Pose2D }, { "Transform2D", Visual::Pose2D },
        { "Twist", Visual::Twist }, { "Wrench", Visual::Wrench },
        { "Twist2D", Visual::Twist2D }, { "Wrench2D", Visual::Wrench2D },
        { "AlignedBox", Visual::Solid }, { "OrientedBox", Visual::Solid }, { "Plane", Visual::Solid },
        { "Segment", Visual::Solid }, { "Sphere", Visual::Solid }, { "Capsule", Visual::Solid },
        { "Cylinder", Visual::Solid }, { "Cone", Visual::Solid }, { "Polygon", Visual::Solid },
        { "AlignedBox2D", Visual::Figure }, { "OrientedBox2D", Visual::Figure },
        { "Circle", Visual::Figure }, { "Polygon2D", Visual::Figure }, { "Plane2D", Visual::Figure },
        { "Segment2D", Visual::Figure }, { "Capsule2D", Visual::Figure },
        { "GeoPoint", Visual::Geo }, { "Color", Visual::Color },
        { "JointState", Visual::Joints }, { "JointNames", Visual::Chips }, { "Image", Visual::Image },
        { "VideoFrame", Visual::Video }, { "ExternalVideoStream", Visual::Stream },
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
        { "Pose", Visual::Poses }, { "Transform", Visual::Poses }, { "Pose2D", Visual::Poses2D },
        { "Transform2D", Visual::Poses2D },
        { "GeoPoint", Visual::Geos },
        { "Color", Visual::Colors },
    };
    for (const auto& entry : table)
        if (name == entry.name) return entry.visual;
    if (struct_visual(name) == Visual::Solid) return Visual::Solids;
    if (struct_visual(name) == Visual::Figure) return Visual::Figures;
    return struct_visual(name) == Visual::Kv ? Visual::Table : Visual::Multi;
}

bool is_branch(const ValueNode& node)
{
    return node.kind == ValueNode::Struct || node.kind == ValueNode::Array;
}

bool is_variable_array(const ValueNode& node)
{
    return node.kind == ValueNode::Array && node.type.size() > 2 &&
           node.type.compare(node.type.size() - 2, 2, "[]") == 0;
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
        if (node.std_name == "Uuid") return Visual::Uuid;
        switch (node.elem) {
        case ValueNode::Number: return node.elem_std.empty() ? Visual::Bars : Visual::Chips;
        case ValueNode::Bool:   return Visual::Cells;
        case ValueNode::Text:
        case ValueNode::Enum:   return Visual::Chips;
        /* only a gathered path's leaves have no visual as an array, so they list */
        case ValueNode::Blob:
        case ValueNode::Map:    return Visual::Kv;
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
    return !node.folded && !(node.kind == ValueNode::Array && node.count > Capture::ARRAY_WHOLE);
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
    /* The open variable arrays of a draft whose add row is still to come, innermost last. */
    std::vector<int> adds;
    std::vector<bool> variable;   /* by depth: the last node there is a variable array */
    auto note = [&](int i) {
        const ValueNode& node = nodes[i];
        if ((int)variable.size() <= node.depth) variable.resize(node.depth + 1);
        variable[node.depth] = view.composing && is_variable_array(node);
        if (variable[node.depth] && ((i == 0 && bare_branch) || is_open(node, view))) adds.push_back(i);
    };
    auto add_rows = [&](int depth) {
        while (!adds.empty() && nodes[adds.back()].depth >= depth) {
            const ValueNode& array = nodes[adds.back()];
            ValueRow row;
            row.node   = -1;
            row.add    = true;
            row.name   = "add";
            row.path   = array.path;
            row.indent = tree_indent(array.depth + 1 - lift);
            row.odd    = rows.size() % 2 == 1;
            rows.push_back(std::move(row));
            adds.pop_back();
        }
    };
    if (bare_branch) note(0);
    for (size_t i = bare_branch ? 1 : 0; i < nodes.size(); i++) {
        const ValueNode& node = nodes[i];
        add_rows(node.depth);
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
        else if (is_variable_array(node))
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
        row.remove = node.depth > 0 && node.depth - 1 < (int)variable.size() && variable[node.depth - 1] &&
                     !node.name.empty() && node.name[0] == '[';
        row.odd = rows.size() % 2 == 1;
        rows.push_back(std::move(row));
        note((int)i);
    }
    add_rows(0);
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
    if (!view.root_struct && root.std_name == "Uuid") return uuid_text(view.nodes, 0);
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

namespace {

/* One past the last node of the subtree at index. */
size_t subtree_end(const std::vector<ValueNode>& nodes, size_t index)
{
    size_t j = index + 1;
    while (j < nodes.size() && nodes[j].depth > nodes[index].depth) j++;
    return j;
}

/* The nodes in nodes[from, to) that path names, every element's at each [*]. */
void pick(const std::vector<ValueNode>& nodes, size_t from, size_t to, const std::string& path,
          std::vector<size_t>& out)
{
    const size_t star = path.find("[*]");
    const std::string head = path.substr(0, star);
    size_t at = from;
    while (at < to && nodes[at].path != head) at++;
    if (at == to) return;
    if (star == std::string::npos) {
        out.push_back(at);
        return;
    }
    if (nodes[at].kind != ValueNode::Array) return;
    const std::string rest = path.substr(star + 3);
    for (const int k : children_of(nodes, (int)at))
        if (nodes[k].kind != ValueNode::Gap) pick(nodes, k, subtree_end(nodes, k), nodes[k].path + rest, out);
}

} /* namespace */

bool is_gathered(const std::string& path)
{
    return path.find("[*]") != std::string::npos;
}

std::vector<ValueNode> gather(const std::vector<ValueNode>& nodes, const std::string& path)
{
    std::vector<size_t> picked;
    pick(nodes, 0, nodes.size(), path, picked);
    std::vector<ValueNode> out;
    if (picked.empty()) return out;
    const ValueNode& first = nodes[picked[0]];
    ValueNode array;
    array.kind     = ValueNode::Array;
    array.name     = path;
    array.path     = path;
    array.type     = first.type + "[]";
    array.count    = (uint32_t)picked.size();
    array.elem     = first.kind;
    array.elem_std = first.std_name;
    out.push_back(std::move(array));
    for (size_t i = 0; i < picked.size(); i++) {
        const size_t at = picked[i], end = subtree_end(nodes, at), top = out.size();
        const int shift = 1 - nodes[at].depth;
        for (size_t j = at; j < end; j++) {
            out.push_back(nodes[j]);
            out.back().depth += shift;
        }
        out[top].name = "[" + std::to_string(i) + "]";
    }
    return out;
}

std::string every_element(const std::string& path)
{
    for (size_t close = path.rfind(']'); close != std::string::npos && close > 0; close = path.rfind(']', close - 1)) {
        const size_t open = path.rfind('[', close);
        if (open == std::string::npos) break;
        const bool index = close > open + 1 &&
                           std::all_of(path.begin() + open + 1, path.begin() + close, [](char c) { return c >= '0' && c <= '9'; });
        if (index && close + 1 < path.size()) return path.substr(0, open) + "[*]" + path.substr(close + 1);
    }
    return std::string();
}

std::vector<FieldPath> field_paths(const std::vector<ValueNode>& nodes)
{
    std::vector<FieldPath> out;
    std::set<std::string> seen;
    for (const ValueNode& node : nodes) {
        if (node.kind == ValueNode::Gap || node.kind == ValueNode::Part || node.path.empty()) continue;
        std::string path;
        for (size_t i = 0; i < node.path.size(); i++) {
            const size_t close = node.path[i] == '[' ? node.path.find(']', i) : std::string::npos;
            if (close == std::string::npos) {
                path += node.path[i];
                continue;
            }
            path += "[*]";
            i = close;
        }
        if (path.size() >= 3 && path.compare(path.size() - 3, 3, "[*]") == 0) continue;
        if (seen.insert(path).second) out.push_back(FieldPath{ path, node.type });
    }
    return out;
}

bool path_fits(const std::vector<ValueNode>& nodes, const Capture::WatchView& view, const std::string& path)
{
    if (find_node(nodes, view, path) >= -1) return true;
    if (!is_gathered(path)) return false;
    if (!gather(nodes, path).empty()) return true;
    const std::string head = path.substr(0, path.find("[*]"));
    for (const ValueNode& node : nodes)
        if (node.path == head) return node.kind == ValueNode::Array && node.count == 0;
    return false;
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

/* What a container holds. A part's fields sit beside its heading, up to the next part. */
std::vector<int> members(const std::vector<ValueNode>& nodes, int index)
{
    if (index < 0 || nodes[index].kind != ValueNode::Part) return children_of(nodes, index);
    std::vector<int> out;
    for (size_t j = (size_t)index + 1; j < nodes.size() && nodes[j].kind != ValueNode::Part; j++)
        if (nodes[j].depth == nodes[index].depth) out.push_back((int)j);
    return out;
}

/* A part whose value is not a struct holds it as one node at the part's own path, which
   stands for the part. */
int unwrap(const std::vector<ValueNode>& nodes, int index)
{
    if (index < 0 || nodes[index].kind != ValueNode::Part) return index;
    const std::vector<int> inside = members(nodes, index);
    return inside.size() == 1 && nodes[inside[0]].path == nodes[index].path.substr(1) ? inside[0] : index;
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
    index = unwrap(nodes, index);
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
    for (const int k : members(nodes, index)) {
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
    if (std_name == "Uuid") return uuid_text(nodes, index);
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

std::string uuid_text(const std::vector<ValueNode>& nodes, int index)
{
    const std::vector<int> bytes = children_of(nodes, index);
    if (bytes.size() != 16) return {};
    std::string out;
    for (size_t i = 0; i < bytes.size(); i++) {
        char hex[4];
        std::snprintf(hex, sizeof hex, "%02x", (unsigned)(nodes[bytes[i]].integer & 0xFF));
        if (i == 4 || i == 6 || i == 8 || i == 10) out += '-';
        out += hex;
    }
    return out;
}

std::string copy_text(const std::vector<ValueNode>& nodes, const Capture::WatchView& view, const std::string& path)
{
    const int index = unwrap(nodes, find_node(nodes, view, path));
    if (index < -1) return {};
    const std::string std_name = index < 0 ? view.root_std : nodes[index].std_name;
    const std::string text = standard(nodes, index, std_name, view.message);
    return text.empty() ? copy_raw(nodes, view, path) : text;
}

std::string copy_raw(const std::vector<ValueNode>& nodes, const Capture::WatchView& view, const std::string& path)
{
    const int index = unwrap(nodes, find_node(nodes, view, path));
    if (index < -1) return {};
    if (!is_container(index, nodes)) return leaf(nodes[index], view.message);
    std::string out;
    json(nodes, index, 0, view.message, out);
    return out;
}

Table table_of(const std::vector<ValueNode>& nodes, const Capture::WatchView& view, const std::string& path)
{
    Table table;
    const int index = unwrap(nodes, find_node(nodes, view, path));
    if (index < -1 || !is_container(index, nodes)) return table;
    std::vector<int> rows;
    for (const int k : members(nodes, index))
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

namespace {

int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* JSON into Pasted. Every list and object keeps its source and every number its literal. */
class JsonReader {
public:
    explicit JsonReader(const std::string& text) : s_(text) {}

    bool read(Pasted& out)
    {
        space();
        if (!value(out, 0)) return false;
        space();
        return i_ == s_.size();
    }

private:
    const std::string& s_;
    size_t             i_ = 0;

    bool at(char c) const { return i_ < s_.size() && s_[i_] == c; }
    bool digit() const { return i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9'; }

    void space()
    {
        while (at(' ') || at('\t') || at('\n') || at('\r')) i_++;
    }

    bool word(const char* text)
    {
        const size_t n = std::string(text).size();
        if (s_.compare(i_, n, text) != 0) return false;
        i_ += n;
        return true;
    }

    /* Nesting is capped, so a hostile clipboard cannot run the stack out. */
    bool value(Pasted& out, int depth)
    {
        const size_t start = i_;
        if (depth > 64 || i_ >= s_.size()) return false;
        if (at('{') || at('[')) {
            const bool object = at('{');
            const char close  = object ? '}' : ']';
            out.kind = object ? Pasted::Object : Pasted::List;
            i_++;
            space();
            for (bool first = true; !at(close); first = false) {
                if (!first) {
                    if (!at(',')) return false;
                    i_++;
                    space();
                }
                if (object) {
                    out.keys.emplace_back();
                    if (!string(out.keys.back())) return false;
                    space();
                    if (!at(':')) return false;
                    i_++;
                    space();
                }
                out.items.emplace_back();
                if (!value(out.items.back(), depth + 1)) return false;
                space();
            }
            i_++;
            out.text = s_.substr(start, i_ - start);
            return true;
        }
        if (at('"')) {
            out.kind = Pasted::Text;
            return string(out.text);
        }
        if (word("true") || word("false")) {
            out.kind = Pasted::Bool;
            out.text = s_.substr(start, i_ - start);
            return true;
        }
        if (word("null")) {
            out.kind = Pasted::Null;
            return true;
        }
        out.kind = Pasted::Number;
        return number(out.text);
    }

    bool number(std::string& out)
    {
        const size_t start = i_;
        if (at('-')) i_++;
        const size_t whole = i_;
        while (digit()) i_++;
        if (i_ == whole || (s_[whole] == '0' && i_ - whole > 1)) return false;
        if (at('.')) {
            const size_t frac = ++i_;
            while (digit()) i_++;
            if (i_ == frac) return false;
        }
        if (at('e') || at('E')) {
            i_++;
            if (at('+') || at('-')) i_++;
            const size_t exp = i_;
            while (digit()) i_++;
            if (i_ == exp) return false;
        }
        out = s_.substr(start, i_ - start);
        return true;
    }

    bool hex4(uint32_t& code)
    {
        code = 0;
        for (int k = 0; k < 4; k++, i_++) {
            const int d = i_ < s_.size() ? hex_digit(s_[i_]) : -1;
            if (d < 0) return false;
            code = code * 16 + (uint32_t)d;
        }
        return true;
    }

    static void utf8(uint32_t code, std::string& out)
    {
        if (code < 0x80) {
            out += (char)code;
        } else if (code < 0x800) {
            out += (char)(0xC0 | code >> 6);
            out += (char)(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += (char)(0xE0 | code >> 12);
            out += (char)(0x80 | (code >> 6 & 0x3F));
            out += (char)(0x80 | (code & 0x3F));
        } else {
            out += (char)(0xF0 | code >> 18);
            out += (char)(0x80 | (code >> 12 & 0x3F));
            out += (char)(0x80 | (code >> 6 & 0x3F));
            out += (char)(0x80 | (code & 0x3F));
        }
    }

    bool string(std::string& out)
    {
        if (!at('"')) return false;
        i_++;
        out.clear();
        while (i_ < s_.size()) {
            const char c = s_[i_++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i_ >= s_.size()) return false;
            switch (const char e = s_[i_++]) {
            case '"': case '\\': case '/': out += e; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                uint32_t code = 0, low = 0;
                if (!hex4(code)) return false;
                /* a character past the first plane comes as a surrogate pair */
                if (code >= 0xD800 && code < 0xDC00 && s_.compare(i_, 2, "\\u") == 0) {
                    i_ += 2;
                    if (!hex4(low) || low < 0xDC00 || low > 0xDFFF) return false;
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                }
                utf8(code, out);
                break;
            }
            default: return false;
            }
        }
        return false;
    }
};

Pasted number_of(const std::string& literal)
{
    Pasted out;
    out.kind = Pasted::Number;
    out.text = literal;
    return out;
}

std::string trimmed(const std::string& text)
{
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

/* Bytes in hex as their numbers, false unless they are exactly count of them. */
bool hex_bytes(const std::string& hex, size_t count, Pasted& out)
{
    if (hex.size() != count * 2) return false;
    for (size_t k = 0; k < hex.size(); k += 2) {
        const int high = hex_digit(hex[k]), low = hex_digit(hex[k + 1]);
        if (high < 0 || low < 0) return false;
        out.items.push_back(number_of(std::to_string(high * 16 + low)));
    }
    return true;
}

/* A standard type's copy_text form read into the shape its field has: #rrggbb or #rrggbbaa,
   "lat, lon", 8-4-4-4-12 hex and ISO 8601 UTC. False for any other text. */
bool read_standard(const std::string& std_name, const std::string& text, Pasted& out)
{
    const std::string t = trimmed(text);
    out = Pasted();
    if (std_name == "Color") {
        const std::string hex = !t.empty() && t[0] == '#' ? t.substr(1) : t;
        if (!hex_bytes(hex, 3, out) && !hex_bytes(hex, 4, out)) return false;
        if (out.items.size() == 3) out.items.push_back(number_of("255"));
        out.kind = Pasted::Object;
        out.keys = { "r", "g", "b", "a" };
        return true;
    }
    if (std_name == "GeoPoint") {
        const size_t comma = t.find(',');
        if (comma == std::string::npos) return false;
        for (const std::string& part : { trimmed(t.substr(0, comma)), trimmed(t.substr(comma + 1)) }) {
            char* end = nullptr;
            std::strtod(part.c_str(), &end);
            if (part.empty() || *end) return false;
            out.items.push_back(number_of(part));
        }
        out.kind = Pasted::Object;
        out.keys = { "lat", "lon" };
        return true;
    }
    if (std_name == "Uuid") {
        std::string hex;
        for (const char c : t)
            if (c != '-') hex += c;
        out.kind = Pasted::List;
        return hex_bytes(hex, 16, out);
    }
    int64_t us = 0;
    if (std_name == "Timestamp" && parse_iso_time(t, us)) {
        out = number_of(std::to_string(us));
        return true;
    }
    return false;
}

/* Whether two values of one field are equal as written, a float at its own width. */
bool same(const ValueNode& a, const ValueNode& b)
{
    if (a.kind == ValueNode::Text) return a.text == b.text;
    if (a.kind == ValueNode::Bool) return (a.number != 0) == (b.number != 0);
    if (a.kind == ValueNode::Number && a.is_float)
        return a.type == "f32" ? (float)a.number == (float)b.number : a.number == b.number;
    return a.integer == b.integer;
}

/* Walks a pasted value and the nodes it lands on together. */
struct Paster {
    const std::vector<ValueNode>& nodes;
    const std::string&            root_std;
    bool                          resizable;
    PastePlan&                    out;
    std::string&                  error;

    bool fail(const std::string& why)
    {
        error = why;
        return false;
    }

    std::string where(int index) const
    {
        if (index < 0 || nodes[index].path.empty()) return "the value";
        return nodes[index].kind == ValueNode::Part ? nodes[index].name : nodes[index].path;
    }

    static std::string what(const Pasted& value)
    {
        switch (value.kind) {
        case Pasted::Bool:   return "true or false";
        case Pasted::Number: return "a number";
        case Pasted::Text:   return "text";
        case Pasted::List:   return "a list";
        default:             return "an object";
        }
    }

    bool at(int index, const Pasted& value)
    {
        if (value.kind == Pasted::Null) return true;   /* null leaves a field as it is */
        index = unwrap(nodes, index);
        Pasted standard;
        if (value.kind == Pasted::Text && read_standard(index < 0 ? root_std : nodes[index].std_name, value.text, standard))
            return at(index, standard);
        if (!is_container(index, nodes)) return leaf(index, value);
        return index >= 0 && nodes[index].kind == ValueNode::Array ? list(index, value) : fields(index, value);
    }

    /* A struct takes the members named as its fields and keeps the rest, but one at least. */
    bool fields(int index, const Pasted& value)
    {
        if (value.kind != Pasted::Object) return fail(where(index) + " takes an object, not " + what(value));
        const std::vector<int> inside = members(nodes, index);
        int matched = 0;
        for (const int k : inside) {
            const auto key = std::find(value.keys.begin(), value.keys.end(), nodes[k].name);
            if (nodes[k].kind == ValueNode::Gap || key == value.keys.end()) continue;
            matched++;
            if (!at(k, value.items[key - value.keys.begin()])) return false;
        }
        if (inside.empty() || matched) return true;
        return fail("no field of " + where(index) + " is in the clipboard");
    }

    /* A variable array outside any array of structs is the one kind that can change length. */
    bool list(int index, const Pasted& value)
    {
        const ValueNode& node = nodes[index];
        if (value.kind != Pasted::List) return fail(where(index) + " takes a list, not " + what(value));
        if (value.items.size() != node.count) {
            if (resizable && is_variable_array(node) && node.elems.empty()) {
                out.lengths.emplace_back(node.field, (uint32_t)value.items.size());
                return true;
            }
            return fail(where(index) + " takes " + std::to_string(node.count) + " elements, not " +
                        std::to_string(value.items.size()));
        }
        for (const int k : children_of(nodes, index)) {
            if (nodes[k].kind == ValueNode::Gap) continue;
            const size_t element = std::strtoul(nodes[k].name.c_str() + 1, nullptr, 10);
            if (element < value.items.size() && !at(k, value.items[element])) return false;
        }
        return true;
    }

    /* A leaf takes the text its editor takes. An enum also takes an option's number, and a
       Duration seconds, as Copy cell writes them. */
    bool leaf(int index, const Pasted& value)
    {
        const ValueNode& node = nodes[index];
        if (editor_for(node).empty()) return fail(where(index) + " cannot be edited");
        if (node.kind != ValueNode::Text && (value.kind == Pasted::List || value.kind == Pasted::Object))
            return fail(where(index) + " takes one value, not " + what(value));
        std::string text = value.text;
        if (node.kind == ValueNode::Enum && value.kind == Pasted::Number)
            for (size_t k = 0; k < node.options.size() && k < node.option_values.size(); k++)
                if (std::to_string(node.option_values[k]) == text) text = node.options[k];
        if (node.std_name == "Duration" && value.kind == Pasted::Number && text.find_first_of(".eE") != std::string::npos) {
            const double seconds = std::strtod(text.c_str(), nullptr);
            if (std::abs(seconds) < 9e12) text = std::to_string(std::llround(seconds * 1e6));
        }
        ValueNode to;
        if (!parse_value(node, text, to)) {
            const std::string shown = text.size() > 24 ? text.substr(0, 24) + "..." : text;
            return fail(where(index) + " is " + node.type + ", which cannot hold " + shown);
        }
        if (same(to, node)) return true;
        /* shown rounded as every value is, and written exact */
        FieldEdit edit;
        edit.text  = edit_text(to);
        edit.value = to;
        out.edits.push_back(edit);
        return true;
    }
};

} /* namespace */

Pasted read_pasted(const std::string& text)
{
    Pasted out;
    if (JsonReader(text).read(out)) return out;
    out      = Pasted();
    out.kind = Pasted::Text;
    out.text = text;
    while (!out.text.empty() && (out.text.back() == '\n' || out.text.back() == '\r')) out.text.pop_back();
    return out;
}

bool plan_paste(const std::vector<ValueNode>& nodes, const Capture::WatchView& view, const std::string& path,
                const Pasted& value, bool resizable, PastePlan& out, std::string& error)
{
    out = PastePlan();
    error.clear();
    const int index = find_node(nodes, view, path);
    if (index < -1) {
        error = path.empty() ? "there is no value yet" : "there is no " + path;
        return false;
    }
    Paster paster{ nodes, view.root_std, resizable, out, error };
    return paster.at(index, value);
}
