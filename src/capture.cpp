#include "capture.hpp"

#include "format.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <tuple>
#include <atomic>
#include <mutex>
#include <utility>

#include <rant.hpp>

namespace {

uint64_t steady_us()
{
    using namespace std::chrono;
    return (uint64_t)duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

uint64_t wall_us()
{
    using namespace std::chrono;
    return (uint64_t)duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
}

/* "1.2.3.4:7401" groups under "1.2.3.4". */
std::string host_of(const std::string& address)
{
    const size_t colon = address.rfind(':');
    return colon == std::string::npos ? address : address.substr(0, colon);
}

Capture::Kind kind_of(rant::EntityKind kind)
{
    switch (kind) {
    case rant::EntityKind::Function: return Capture::Kind::Function;
    case rant::EntityKind::Variable: return Capture::Kind::Variable;
    case rant::EntityKind::Task:     return Capture::Kind::Task;
    default:                         return Capture::Kind::Topic;
    }
}

/* What the Topics tab says about an entity beyond its kind. */
std::string entity_note(const rant::Entity& e)
{
    std::string note;
    auto add = [&note](const char* word) {
        if (!note.empty()) note += ", ";
        note += word;
    };
    if (e.kind == rant::EntityKind::Variable) add(e.writable ? "writable" : "read-only");
    if (e.incomplete) add("half advertised");
    return note;
}

/* One node's side of a name, from its own reflection. */
struct Endpoint {
    uint32_t     peer = 0;
    std::string  node;
    rant::Entity entity;
};

/* Why each writer cannot reach each reader on another node, by the transport's gates in
   their order. A function's or task's provider reads the request, so it is the reader. */
std::vector<std::string> match_issues(Capture::Kind kind, const std::vector<Endpoint>& ends)
{
    const bool provider_writes = kind == Capture::Kind::Topic || kind == Capture::Kind::Variable;
    std::vector<std::string> issues;
    for (const Endpoint& w : ends) {
        if (!(provider_writes ? w.entity.provides : w.entity.consumes)) continue;
        for (const Endpoint& r : ends) {
            if (r.peer == w.peer || !(provider_writes ? r.entity.consumes : r.entity.provides)) continue;
            const rant::Schema& rs = r.entity.schema;
            const rant::Schema& ws = w.entity.schema;
            std::string issue;
            if (r.entity.reliable && !w.entity.reliable)
                issue = r.node + " wants reliable, " + w.node + " sends best effort";
            else if (rs && !ws)
                issue = r.node + " is typed, " + w.node + " sends untyped";
            else if (rs && !rs.can_read(ws))
                issue = r.node + " cannot read " + w.node + ": " + rs.why_not(ws);
            if (!issue.empty() && std::find(issues.begin(), issues.end(), issue) == issues.end())
                issues.push_back(std::move(issue));
        }
    }
    return issues;
}

/* The distinct node names among the ends, joined. */
std::string node_list(const std::vector<Endpoint>& ends)
{
    std::vector<std::string> names;
    for (const Endpoint& end : ends)
        if (std::find(names.begin(), names.end(), end.node) == names.end()) names.push_back(end.node);
    std::string list;
    for (const std::string& name : names) list += (list.empty() ? "" : ", ") + name;
    return list;
}

/* A view named by its key, the kind read back from the key's word. */
void name_view(Capture::WatchView& v, const std::string& key)
{
    using Kind = Capture::Kind;
    v.key  = key;
    v.name = Capture::name_of(key);
    v.kind = Kind::Topic;
    for (const Kind kind : { Kind::Function, Kind::Variable, Kind::Task })
        if (key.compare(0, key.find(':'), Capture::kind_word(kind)) == 0) v.kind = kind;
}

uint64_t dict_uint(const rant::MapDict& d, const char* key)
{
    const auto it = d.find(key);
    return it == d.end() ? 0 : it->second.as_uint();
}

std::string dict_string(const rant::MapDict& d, const char* key)
{
    const auto it = d.find(key);
    return it == d.end() ? std::string() : it->second.as_string();
}

bool dict_bool(const rant::MapDict& d, const char* key)
{
    const auto it = d.find(key);
    return it != d.end() && it->second.as_bool();
}

/* How many log lines the ring holds. The sidebar renders far fewer. */
constexpr size_t LOG_RING = 512;

/* How often the selected node's @rant/meta endpoint is asked, in microseconds. */
constexpr uint64_t META_PERIOD_US = 1000000;

/* How often every node is asked for its traffic counters, and when an unanswered ask is
   given up. A reply further apart than three periods starts the rates over. */
constexpr uint64_t TRAFFIC_PERIOD_US  = 2000000;
constexpr uint64_t TRAFFIC_TIMEOUT_US = 10000000;

/* The watch queue between the service thread and the next poll. A drain normally takes
   everything, so these only bound a stalled UI or a flood of large messages. */
constexpr size_t WATCH_QUEUE_COUNT = 4096;
constexpr size_t WATCH_QUEUE_BYTES = 64u << 20;

/* The frames a video stream queues for its card, bounded for a card that is not drawn. */
constexpr size_t STREAM_QUEUE_COUNT = 240;
constexpr size_t STREAM_QUEUE_BYTES = 64u << 20;

/* A queue bounded by count and by bytes, which drops its oldest items but never the newest. */
template <class T>
struct Bounded {
    std::deque<T> items;
    size_t        bytes = 0;

    /* True when an older item was dropped to make room. */
    bool push(T item, size_t most, size_t most_bytes)
    {
        bytes += item.data.size();
        items.push_back(std::move(item));
        bool dropped = false;
        while (items.size() > most || (bytes > most_bytes && items.size() > 1)) {
            bytes -= items.front().data.size();
            items.pop_front();
            dropped = true;
        }
        return dropped;
    }

    std::deque<T> take()
    {
        bytes = 0;
        return std::exchange(items, {});
    }
};

/* How much of a blob the value keeps: enough for a hex dump's four rows. */
constexpr size_t BLOB_HEAD = 64;

/* An array longer than this shows its first and last few elements and a gap between. */
constexpr uint32_t ARRAY_HEAD = 5, ARRAY_TAIL = 3;

/* How many more elements each click on a gap shows. */
constexpr uint32_t REVEAL_STEP = 100;

using Field     = rant::Schema::Field;
using FieldType = rant::FieldType;
using ValueNode = Capture::ValueNode;

const char* scalar_word(FieldType kind)
{
    switch (kind) {
    case FieldType::U8:  return "u8";  case FieldType::U16: return "u16";
    case FieldType::U32: return "u32"; case FieldType::U64: return "u64";
    case FieldType::I8:  return "i8";  case FieldType::I16: return "i16";
    case FieldType::I32: return "i32"; case FieldType::I64: return "i64";
    case FieldType::F32: return "f32"; case FieldType::F64: return "f64";
    case FieldType::Bool:    return "bool";
    case FieldType::Struct:  return "struct";
    case FieldType::Map:     return "map";
    case FieldType::VString: return "string";
    default:                 return "?";
    }
}

/* A field's type the way the core spells it in a mismatch reason. */
std::string field_type(const Field& f)
{
    if (!f.type_name.empty()) return std::string(f.type_name);
    if (f.kind == FieldType::Map)     return "map";
    if (f.kind == FieldType::VString) return "string";
    if (f.kind == FieldType::Enum)    return std::string("enum<") + scalar_word(f.elem) + ">";
    const bool array = f.kind == FieldType::Array || f.kind == FieldType::VArray;
    std::string out;
    if (array && !f.elem_name.empty()) out = std::string(f.elem_name);
    else {
        const FieldType elem = array ? f.elem : f.kind;
        out = elem == FieldType::String ? "string<" + std::to_string(f.str_cap) + ">" : scalar_word(elem);
    }
    if (f.kind == FieldType::Array)  out += "[" + std::to_string(f.count) + "]";
    if (f.kind == FieldType::VArray) out += "[]";
    return out;
}

/* A schema's root type: its name, a bare root's own type, or "struct". */
std::string root_type(const rant::Schema& schema)
{
    if (!schema) return {};
    if (!schema.name().empty()) return std::string(schema.name());
    /* A root that is not a struct is its one unnamed field, and an array of structs lists
       its element's members after it. */
    Field root;
    if (schema.field_at(0, root) && root.name.empty()) return field_type(root);
    return "struct";
}

std::string std_name(std::string_view name)
{
    if (name.empty()) return {};
    const auto type = rant::detail::rant_std_by_name(rant::detail::rant_string(name.data(), name.size()));
    return type == rant::detail::RANT_STD_NONE ? std::string() : std::string(name);
}

bool is_number(FieldType kind)
{
    return kind <= FieldType::F64;   /* U8 through F64 lead the enum */
}

int scalar_size(FieldType kind)
{
    switch (kind) {
    case FieldType::U16: case FieldType::I16: return 2;
    case FieldType::U32: case FieldType::I32: case FieldType::F32: return 4;
    case FieldType::U64: case FieldType::I64: case FieldType::F64: return 8;
    default: return 1;
    }
}

/* One scalar from the wire, little endian whatever the host. */
void read_scalar(FieldType kind, const uint8_t* p, ValueNode& n)
{
    uint64_t bits = 0;
    const int size = scalar_size(kind);
    for (int i = 0; i < size; i++) bits |= (uint64_t)p[i] << (8 * i);

    n.kind = kind == FieldType::Bool ? ValueNode::Bool : ValueNode::Number;
    switch (kind) {
    case FieldType::F32: { uint32_t b = (uint32_t)bits; float f; std::memcpy(&f, &b, 4); n.number = f; n.is_float = true; return; }
    case FieldType::F64: { double d; std::memcpy(&d, &bits, 8); n.number = d; n.is_float = true; return; }
    case FieldType::I8:  n.integer = (int8_t)bits;  break;
    case FieldType::I16: n.integer = (int16_t)bits; break;
    case FieldType::I32: n.integer = (int32_t)bits; break;
    case FieldType::I64: n.integer = (int64_t)bits; break;
    default:             n.integer = (int64_t)bits; n.is_unsigned = true; break;
    }
    n.number = n.is_unsigned ? (double)(uint64_t)n.integer : (double)n.integer;
}

/* The inverse, for an element of a plain array. */
void write_scalar(FieldType kind, uint8_t* p, const ValueNode& n)
{
    uint64_t bits = 0;
    if (kind == FieldType::F32) {
        const float f = (float)n.number;
        uint32_t b;
        std::memcpy(&b, &f, 4);
        bits = b;
    } else if (kind == FieldType::F64) {
        std::memcpy(&bits, &n.number, 8);
    } else if (kind == FieldType::Bool) {
        bits = n.number != 0;
    } else {
        bits = (uint64_t)n.integer;
    }
    for (int i = 0, size = scalar_size(kind); i < size; i++) p[i] = (uint8_t)(bits >> (8 * i));
}

/* Writes one edited node into a message buffer at the place its addressing names. A fixed
   string that does not fit its capacity is refused rather than cut. */
bool write_edit(const rant::Schema& schema, std::vector<uint8_t>& buf, const ValueNode& e)
{
    const rant::detail::RantSchema* raw = schema.raw();
    Field f;
    if (e.field == 0xFFFF || !schema.field_at(e.field, f)) return false;
    rant::detail::RantValue v{};
    std::vector<uint8_t> elements;
    if (e.element >= 0) {
        /* A plain array is written whole, so patch the one element in a copy of it. */
        rant::detail::RantValue now;
        if (!rant::detail::rant_get_value_at(rant::detail::rant_bytes(buf.data(), buf.size()), raw, e.field,
                                             e.elems.data(), (uint16_t)e.elems.size(), &now) || !now.bytes.data)
            return false;
        elements.assign(now.bytes.data, now.bytes.data + now.bytes.len);
        const size_t at = (size_t)e.element * f.elem_size;
        if (!f.elem_size || at + f.elem_size > elements.size()) return false;
        uint8_t* p = elements.data() + at;
        if (f.elem == FieldType::String) {
            if (e.text.size() > f.str_cap) return false;
            p[0] = (uint8_t)(e.text.size() & 0xFF);
            p[1] = (uint8_t)(e.text.size() >> 8);
            std::memset(p + 2, 0, f.str_cap);
            std::memcpy(p + 2, e.text.data(), e.text.size());
        } else {
            write_scalar(f.elem, p, e);
        }
        v.bytes = rant::detail::rant_bytes(elements.data(), elements.size());
    } else {
        switch (e.kind) {
        case ValueNode::Number:
            if (e.is_float)         v.v.f = e.number;
            else if (e.is_unsigned) v.v.u = (uint64_t)e.integer;
            else                    v.v.i = e.integer;
            break;
        case ValueNode::Bool: v.v.u = e.number != 0; break;
        case ValueNode::Enum: v.v.i = e.integer; break;
        case ValueNode::Text:
            if (f.kind == FieldType::String && e.text.size() > f.str_cap) return false;
            v.bytes = rant::detail::rant_bytes(e.text.data(), e.text.size());
            break;
        default: return false;
        }
    }
    return rant::detail::rant_set_value_at(buf.data(), buf.size(), raw, e.field, e.elems.data(),
                                           (uint16_t)e.elems.size(), &v) != 0;
}

/* Walks the flat field table of one message into value nodes. A struct array's members are
   listed once as a template under the array, so they are walked again per element. */
struct Decoder {
    const rant::detail::RantSchema* raw;
    std::vector<Field>              fields;
    rant::detail::RantBytes         msg;
    std::vector<ValueNode>&         out;
    std::string&                    shape;
    bool                            whole = false;   /* every element, no gap */
    const std::map<std::string, uint32_t>* revealed = nullptr;   /* extra head elements by array path */

    /* The field after i's subtree: the table is depth first. */
    size_t end_of(size_t i) const
    {
        size_t j = i + 1;
        while (j < fields.size() && fields[j].depth > fields[i].depth) j++;
        return j;
    }

    static std::string join(const std::string& parent, std::string_view name)
    {
        if (parent.empty()) return std::string(name);
        if (name.empty()) return parent;   /* a bare root under a part's prefix */
        return parent + "." + std::string(name);
    }

    /* How many elements lead a long array before its gap: a few, plus what was revealed. */
    uint32_t head_of(const std::string& path) const
    {
        if (!revealed) return ARRAY_HEAD;
        const auto it = revealed->find(path);
        return ARRAY_HEAD + (it == revealed->end() ? 0 : it->second);
    }

    /* The element indices a long array shows, with -1 where the gap goes. */
    std::vector<int64_t> visible(uint32_t count, uint32_t head) const
    {
        std::vector<int64_t> out;
        if (whole || count <= Capture::ARRAY_WHOLE || head + ARRAY_TAIL >= count) {
            for (uint32_t k = 0; k < count; k++) out.push_back(k);
            return out;
        }
        for (uint32_t k = 0; k < head; k++) out.push_back(k);
        out.push_back(-1);
        for (uint32_t k = count - ARRAY_TAIL; k < count; k++) out.push_back(k);
        return out;
    }

    /* elems holds the element index in each enclosing struct array, outermost first */
    void field(size_t i, const std::vector<uint32_t>& elems, const std::string& parent, int depth)
    {
        const Field& f = fields[i];
        ValueNode n;
        n.name     = f.name.empty() ? "value" : std::string(f.name);
        n.path     = join(parent, f.name);
        n.type     = field_type(f);
        n.std_name = std_name(f.type_name);
        n.depth    = depth;
        n.field    = (uint16_t)i;
        n.elems    = elems;

        rant::detail::RantValue v;
        const bool ok = rant::detail::rant_get_value_at(msg, raw, (uint16_t)i, elems.data(),
                                                        (uint16_t)elems.size(), &v) != 0;

        switch (f.kind) {
        case FieldType::Struct:
            n.kind = ValueNode::Struct;
            out.push_back(n);
            for (size_t j = i + 1, end = end_of(i); j < end; j = end_of(j)) field(j, elems, n.path, depth + 1);
            return;
        case FieldType::Array:
        case FieldType::VArray:
            array(i, f, ok ? v : rant::detail::RantValue{}, n, elems);
            return;
        case FieldType::String:
        case FieldType::VString:
            n.kind = ValueNode::Text;
            n.cap  = f.kind == FieldType::String ? f.str_cap : 0;
            if (ok && v.bytes.data) n.text.assign((const char*)v.bytes.data, v.bytes.len);
            break;
        case FieldType::Map:
            n.kind  = ValueNode::Map;
            n.count = ok ? v.count : 0;
            break;
        case FieldType::Enum: {
            n.kind    = ValueNode::Enum;
            n.integer = ok ? v.v.i : 0;
            n.number  = (double)n.integer;
            const rant::detail::RantString name = rant::detail::rant_enum_name_of(raw, (uint16_t)i, n.integer);
            if (name.len) n.text.assign(name.data, name.len);
            for (uint16_t k = 0, count = rant::detail::rant_schema_enum_count(raw, (uint16_t)i); k < count; k++) {
                int64_t value = 0;
                rant::detail::RantString option{};
                if (!rant::detail::rant_schema_enum_variant(raw, (uint16_t)i, k, &value, &option)) continue;
                n.options.emplace_back(option.data ? option.data : "", option.len);
                n.option_values.push_back(value);
            }
            break;
        }
        case FieldType::Bool:
            n.kind   = ValueNode::Bool;
            n.number = ok && v.v.u ? 1 : 0;
            break;
        default:
            if (!is_number(f.kind)) break;
            n.kind = ValueNode::Number;
            if (f.kind == FieldType::F32 || f.kind == FieldType::F64) {
                n.is_float = true;
                n.number   = ok ? v.v.f : 0;
            } else if (f.kind >= FieldType::I8) {
                n.integer = ok ? v.v.i : 0;
                n.number  = (double)n.integer;
            } else {
                n.integer     = ok ? (int64_t)v.v.u : 0;
                n.is_unsigned = true;
                n.number      = (double)(ok ? v.v.u : 0);
            }
            break;
        }
        out.push_back(n);
    }

    void array(size_t i, const Field& f, const rant::detail::RantValue& v, ValueNode& n,
               const std::vector<uint32_t>& elems)
    {
        const uint32_t elem_size = f.elem_size;
        uint32_t count = 0;
        if (f.kind == FieldType::Array) count = f.count;
        else if (elem_size) count = (uint32_t)(v.bytes.len / elem_size);

        /* A variable byte array is one blob: an image, a file, a payload. */
        if (f.kind == FieldType::VArray && f.elem == FieldType::U8) {
            n.kind  = ValueNode::Blob;
            n.count = (uint32_t)v.bytes.len;
            n.offset = v.bytes.data ? (uint32_t)(v.bytes.data - msg.data) : 0;
            if (v.bytes.data) n.text.assign((const char*)v.bytes.data, v.bytes.len < BLOB_HEAD ? v.bytes.len : BLOB_HEAD);
            out.push_back(n);
            return;
        }

        n.kind     = ValueNode::Array;
        n.count    = count;
        n.elem     = f.elem == FieldType::Struct ? ValueNode::Struct
                   : f.elem == FieldType::String ? ValueNode::Text
                   : f.elem == FieldType::Bool   ? ValueNode::Bool : ValueNode::Number;
        n.elem_std = std_name(f.elem_name);
        const uint32_t head = head_of(n.path);
        shape  += std::to_string(count) + "+" + std::to_string(head) + ",";
        const std::string elem_type = !f.elem_name.empty() ? std::string(f.elem_name)
                                    : f.elem == FieldType::String ? "string<" + std::to_string(f.str_cap) + ">"
                                    : scalar_word(f.elem);
        const std::string elem_std = std_name(f.elem_name);
        const std::string path = n.path;
        const int depth = n.depth;
        out.push_back(n);

        for (const int64_t k : visible(count, head)) {
            ValueNode e;
            e.depth = depth + 1;
            if (k < 0) {
                e.kind  = ValueNode::Gap;
                e.path  = path + "[...]";
                e.count = count - head - ARRAY_TAIL;
                out.push_back(e);
                continue;
            }
            e.name     = "[" + std::to_string(k) + "]";
            e.path     = path + e.name;
            e.folded   = count > 1;
            e.type     = elem_type;
            e.std_name = elem_std;
            e.field    = (uint16_t)i;
            e.elems    = elems;
            if (f.elem == FieldType::Struct) {
                e.kind = ValueNode::Struct;
                out.push_back(e);
                std::vector<uint32_t> inner = elems;
                inner.push_back((uint32_t)k);
                for (size_t j = i + 1, end = end_of(i); j < end; j = end_of(j)) field(j, inner, e.path, depth + 2);
                continue;
            }
            e.element = (int32_t)k;
            const uint8_t* p = v.bytes.data ? v.bytes.data + (size_t)k * elem_size : nullptr;
            if (!p || (size_t)(k + 1) * elem_size > v.bytes.len) {
                e.kind = ValueNode::Number;
            } else if (f.elem == FieldType::String) {
                /* a string slot is [u16 length][capacity bytes] */
                e.kind = ValueNode::Text;
                e.cap  = f.str_cap;
                uint16_t len = (uint16_t)(p[0] | (p[1] << 8));
                if (len > f.str_cap) len = f.str_cap;
                e.text.assign((const char*)p + 2, len);
            } else {
                read_scalar(f.elem, p, e);
            }
            out.push_back(e);
        }
    }
};

} /* namespace */

/* The event, log and meta handlers all fire on the node's service thread, so the members
   they touch are declared before the node and guarded. */
struct Capture::Impl {
    struct RawLog {
        int         level = 2;
        uint64_t    wall_us = 0;
        std::string node;
        std::string text;
    };
    static LogRow row(const RawLog& raw)
    {
        LogRow out;
        out.level = raw.level;
        out.time  = format_clock(raw.wall_us);
        out.node  = raw.node;
        out.text  = raw.text.empty() ? "(empty line)" : raw.text;
        return out;
    }

    std::mutex         mutex;
    std::string        last_event;
    std::deque<RawLog> log_ring;
    rant::MetaSnapshot meta;
    bool               meta_fresh   = false;
    uint64_t           meta_at_us   = 0;   /* our steady clock when the reply landed */
    std::atomic<bool>  meta_pending{false};
    int                meta_failures = 0;

    /* One node's traffic counters, by channel name, as its reply left them. */
    struct TrafficReply {
        std::string id;
        bool        ok = false;
        uint64_t    at_us = 0;
        std::vector<std::tuple<std::string, uint64_t, uint64_t>> counts;   /* channel, tx, rx */
    };
    std::vector<TrafficReply> traffic_inbox;

    rant::Node node;

    /* UI thread only below here. */
    struct Track {
        uint64_t first_seen_us = 0;
        uint32_t epoch         = 0;
        int      updates       = 0;
        bool     seen          = false;
        uint32_t counted_epoch = 0xFFFFFFFFu;   /* the epochs the counts below were walked at */
        uint32_t counted_mesh  = 0xFFFFFFFFu;
        int      publishes     = 0;
        int      subscribes    = 0;
        uint32_t peer          = 0;
        struct End {
            std::string name;
            Kind        kind = Kind::Topic;
            bool        provides = false, consumes = false, reliable = false;
        };
        std::vector<End> ends;   /* that walk, for the mesh graph */
    };
    std::map<std::string, Track> tracks;   /* by uuid hex */

    /* The node stamps last_heard_us on its own monotonic clock and the wrapper exposes no
       way to read that clock, so hold an origin against ours and slide it forward. */
    uint64_t origin_node_us = 0, origin_steady_us = 0;

    uint64_t    prev_cpu_us = 0, prev_cpu_wall_us = 0;   /* the last CPU sample, for the share */
    double      cpu_pct = -1;                              /* the share those two samples gave */
    uint64_t    last_meta_us   = 0;
    uint32_t    selected_peer  = 0;
    bool        have_selection = false;
    uint32_t    endpoints_epoch = 0xFFFFFFFFu, endpoints_mesh = 0xFFFFFFFFu;
    std::string endpoints_id;
    uint32_t    mesh_epoch = 0xFFFFFFFFu;   /* the epoch topics_ was walked at */
    int         mesh_nodes = -1;            /* and the node count then, since a drop is silent */
    std::map<std::string, rant::Entity> entities;   /* that walk by key, for schemas */

    /* One channel's counters at one node, and the rates between its last two replies. */
    struct Channel {
        uint64_t tx = 0, rx = 0;
        double   tx_hz = -1, rx_hz = -1;
        uint64_t tx_moved_us = 0, rx_moved_us = 0;
        uint64_t first_us = 0;   /* the reply it first showed in */
    };
    struct Traffic {
        bool     pending = false;
        uint64_t asked_us = 0, at_us = 0;
        std::map<std::string, Channel> channels;
    };
    std::map<std::string, Traffic> traffic;   /* by uuid hex, like tracks */
    bool                           graph_dirty = true;

    /* Asks one node for its counters, one ask in flight per node. */
    void ask_traffic(rant::Reflection& mesh, const std::string& id, uint32_t peer, uint64_t now)
    {
        Traffic& t = traffic[id];
        if (now - t.asked_us < (t.pending ? TRAFFIC_TIMEOUT_US : TRAFFIC_PERIOD_US)) return;
        t.pending  = true;
        t.asked_us = now;
        const rant::SendStatus sent = mesh.meta_async(peer, [this, id](const rant::MetaSnapshot& snap) {
            TrafficReply reply;
            reply.id    = id;
            reply.ok    = snap.valid;
            reply.at_us = steady_us();
            const auto topics = snap.info.find("topics");
            if (topics != snap.info.end() && topics->second.is_array())
                for (const rant::MapItem& item : topics->second.as_array()) {
                    if (!item.is_map()) continue;
                    const rant::MapDict& d = item.as_map();
                    reply.counts.emplace_back(dict_string(d, "name"), dict_uint(d, "txMsgs"), dict_uint(d, "rxMsgs"));
                }
            std::lock_guard<std::mutex> lock(mutex);
            traffic_inbox.push_back(std::move(reply));
        }, rant::MetaTopics);
        if (sent != rant::SendStatus::Ok) t.pending = false;
    }

    /* Folds the replies that landed into rates. True when one did. */
    bool fold_traffic()
    {
        std::vector<TrafficReply> replies;
        {
            std::lock_guard<std::mutex> lock(mutex);
            replies.swap(traffic_inbox);
        }
        bool moved = false;
        for (const TrafficReply& reply : replies) {
            const auto found = traffic.find(reply.id);
            if (found == traffic.end()) continue;   /* the node left */
            Traffic& t = found->second;
            t.pending = false;
            if (!reply.ok) continue;
            const bool   fresh = t.at_us && reply.at_us > t.at_us && reply.at_us - t.at_us < 3 * TRAFFIC_PERIOD_US;
            const double dt    = fresh ? (double)(reply.at_us - t.at_us) / 1e6 : 0;
            std::map<std::string, Channel> next;
            for (const auto& count : reply.counts) {
                Channel c;
                c.tx       = std::get<1>(count);
                c.rx       = std::get<2>(count);
                c.first_us = reply.at_us;
                const auto old = t.channels.find(std::get<0>(count));
                if (old != t.channels.end()) {
                    c.tx_moved_us = old->second.tx_moved_us;
                    c.rx_moved_us = old->second.rx_moved_us;
                    c.first_us    = old->second.first_us;
                    /* counters that went back are a restarted handle, so start over */
                    if (fresh && c.tx >= old->second.tx && c.rx >= old->second.rx) {
                        c.tx_hz = (double)(c.tx - old->second.tx) / dt;
                        c.rx_hz = (double)(c.rx - old->second.rx) / dt;
                        if (c.tx > old->second.tx) c.tx_moved_us = reply.at_us;
                        if (c.rx > old->second.rx) c.rx_moved_us = reply.at_us;
                    }
                }
                next[std::get<0>(count)] = c;
            }
            t.channels.swap(next);
            t.at_us = reply.at_us;
            moved   = true;
        }
        return moved;
    }

    /* A node's channel as its latest reply gave it, null when that is stale or lacks it. */
    const Channel* channel(const std::string& id, const std::string& name, uint64_t now) const
    {
        const auto t = traffic.find(id);
        if (t == traffic.end() || !t->second.at_us || now - t->second.at_us > 3 * TRAFFIC_PERIOD_US) return nullptr;
        const auto c = t->second.channels.find(name);
        return c == t->second.channels.end() ? nullptr : &c->second;
    }

    /* What a handler hands over. The handler holds the inbox weakly, since a variable's
       observer stays registered until the node closes. */
    struct Sample {
        std::vector<uint8_t> data;
        uint64_t             schema_hash = 0;   /* 0 for an untyped writer */
        uint64_t             recv_us = 0;
    };
    struct Inbox {
        std::mutex                                  mutex;
        Bounded<Sample>                             queue;
        std::map<uint64_t, std::vector<uint8_t>>    wires;   /* each schema's wire, by hash */

        /* One value off the service thread, with the schema it was written in. */
        void push(rant::Bytes data, const rant::detail::RantSchema* raw, uint64_t recv_us)
        {
            Sample sample;
            sample.data.assign(data.begin(), data.end());
            sample.schema_hash = raw ? rant::detail::rant_schema_hash(raw) : 0;
            sample.recv_us     = recv_us;
            std::lock_guard<std::mutex> lock(mutex);
            if (sample.schema_hash && !wires.count(sample.schema_hash)) {
                const rant::detail::RantBytes wire = rant::detail::rant_schema_wire(raw);
                wires[sample.schema_hash].assign(wire.data, wire.data + wire.len);
            }
            queue.push(std::move(sample), WATCH_QUEUE_COUNT, WATCH_QUEUE_BYTES);
        }
    };

    /* One call or run in flight: what came back, off the service thread. */
    struct CallBox {
        Inbox            reply, progress;
        std::mutex       mutex;
        bool             done = false, running = false;
        rant::CallStatus status = rant::CallStatus::Ok;
        std::string      message;
    };

    /* One live handle on a name: a topic subscription, a remote variable, function or task. */
    struct Handle {
        rant::Subscriber<rant::Bytes>                           topic;
        rant::RemoteVariable<rant::Bytes>                       variable;
        rant::RemoteFunction<rant::Bytes, rant::Bytes>          function;
        rant::RemoteTask<rant::Bytes, rant::Bytes, rant::Bytes> task;
        std::string                       name;
        bool                              reliable = false;   /* a topic's QoS */
        bool                              writable = false;   /* a variable opened to set */
        rant::Publisher<rant::Bytes>      publisher;          /* a composed topic's other role */
        uint64_t                          typed = 0;          /* the schema a composed topic took */

        bool valid() const { return topic.valid() || variable.valid() || function.valid() || task.valid(); }
        int  matches() const
        {
            return topic.valid() ? topic.match_count() : variable.valid() ? variable.match_count()
                 : function.valid() ? function.match_count() : task.match_count();
        }
        /* Close every share this handle holds. State while one of them is mid dispatch, and
           what is still open stays open for the next try. */
        rant::SendStatus close()
        {
            rant::SendStatus st = rant::SendStatus::Ok;
            auto one = [&](auto& h) {
                if (!h.valid()) return;
                const rant::SendStatus r = h.close();
                if (r != rant::SendStatus::Ok && r != rant::SendStatus::NoTopic) st = r;
            };
            one(publisher);
            one(topic);
            one(variable);
            one(function);
            one(task);
            return st;
        }
    };

    enum class CallState { Idle, Waiting, Done };

    /* One subscribed or watched name and everything kept for it. */
    struct Sub {
        bool                   wanted = false;   /* kept by subscribe(), not only while watched */
        Handle                 live;             /* feeding the inbox, when valid */
        std::string            live_error;       /* why the last create failed */
        bool                   failed = false;   /* and it waits for the mesh to move to retry */
        uint32_t               failed_epoch = 0;
        std::shared_ptr<Inbox> inbox;
        uint32_t               reliable_epoch = 0xFFFFFFFFu;
        bool                   want_reliable = false;
        bool                   provided = false;   /* a publisher of it is on the mesh */
        /* The node stamps arrival on its own monotonic clock. The smallest gap to ours seen
           maps a stamp onto our clock, so a batch drained at once keeps its spacing. */
        int64_t                clock_offset_us = INT64_MAX;
        uint64_t               last_hash = 0;      /* the schema of the newest message */
        uint64_t               compose_hash = 0;   /* the schema a composed topic is typed with */
        WatchView              view;
        std::map<std::string, std::deque<TracePoint>> traces;
        std::map<std::string, double>                 trace_seconds;
        struct Stream {
            Bounded<Capture::Packet> queue;
            bool                     lost = false;
        };
        std::map<std::string, Stream> streams;   /* by the VideoFrame's path */
        std::vector<ValueNode> whole;
        uint64_t               whole_seq = ~0ull;
        std::map<std::string, uint32_t> revealed;   /* elements shown past each long array's head */

        /* The rate window, and the spacing estimate behind the jitter. */
        double                 rate_since = 0;
        uint64_t               rate_seq = 0;
        uint64_t               gap_last_us = 0;
        double                 gap_mean_ms = 0, gap_p90_ms = 0;
        int                    gaps = 0;

        /* A function or task: the call in flight and what the last one gave. */
        std::shared_ptr<CallBox> call;
        uint32_t               call_id = 0;
        CallState              call_state = CallState::Idle;
        rant::CallStatus       call_status = rant::CallStatus::Ok;
        std::string            call_message;
        double                 call_start = 0, call_took = 0;
        bool                   running = false;
        Sample                 reply, progress;
        bool                   has_reply = false, has_progress = false;
        bool                   matched = false;      /* a provider is there to call */
        uint64_t               generation = ~0ull;   /* the entity's, so a new schema rebuilds */
        bool                   form_dirty = true;

        bool listening() const { return live.valid() && (live.topic.valid() || live.variable.valid()); }
    };

    std::map<std::string, Sub>       subs;         /* by key */
    std::string                      watch_key;    /* what the UI asked for */
    std::vector<Handle>              retiring;     /* a close refuses mid dispatch, so it retries */
    std::map<uint64_t, rant::Schema> schemas;      /* every schema seen, parsed here once */

    Sub* find(const std::string& key)
    {
        const auto it = subs.find(key);
        return it == subs.end() ? nullptr : &it->second;
    }

    /* The key's sub, made on first use. Its kind comes from the key and never changes. */
    Sub& open(const std::string& key)
    {
        Sub& s = subs[key];
        if (s.view.key.empty()) name_view(s.view, key);
        return s;
    }

    /* The watched key's sub, null while nothing is watched or the node failed. */
    static Sub* watched(Impl* im) { return im ? im->find(im->watch_key) : nullptr; }

    const rant::Entity* entity(const std::string& key) const
    {
        const auto it = entities.find(key);
        return it == entities.end() ? nullptr : &it->second;
    }

    void know(const rant::Schema& schema)
    {
        if (schema && !schemas.count(schema.hash())) schemas[schema.hash()] = schema;
    }

    /* One message into the nodes the tree shows, with the root's type, each path under
       prefix. A whole decode keeps every array element and leaves the message where it is. */
    void decode(const std::vector<uint8_t>& data, uint64_t hash, Capture::WatchView& into,
                bool whole = false, const std::string& prefix = {},
                const std::map<std::string, uint32_t>* revealed = nullptr) const
    {
        into.nodes.clear();
        if (!whole) into.message = data;
        into.shape = std::to_string(hash) + ":";
        const auto found = schemas.find(hash);
        if (!hash || found == schemas.end() || found->second.empty()) {
            ValueNode blob;
            blob.kind  = ValueNode::Blob;
            blob.name  = "value";
            blob.path  = prefix;
            blob.type  = "bytes";
            blob.count = (uint32_t)data.size();
            blob.text.assign((const char*)data.data(), data.size() < BLOB_HEAD ? data.size() : BLOB_HEAD);
            into.nodes.push_back(blob);
            into.type        = "bytes";
            into.root_std.clear();
            into.root_struct = false;
            return;
        }
        const rant::Schema& schema = found->second;
        Decoder decoder{ schema.raw(), schema.fields(), rant::detail::rant_bytes(data.data(), data.size()),
                         into.nodes, into.shape, whole, revealed };
        for (size_t i = 0; i < decoder.fields.size(); i = decoder.end_of(i)) decoder.field(i, {}, prefix, 0);
        const bool bare  = !decoder.fields.empty() && decoder.fields[0].name.empty();
        into.root_struct = !bare;
        into.root_std    = std_name(schema.name());
        into.type        = root_type(schema);
        /* A bare root's own field carries no name: the schema's name is its type. */
        if (bare && !into.nodes.empty()) {
            into.nodes[0].type = into.type;
            if (into.nodes[0].std_name.empty()) into.nodes[0].std_name = into.root_std;
        }
    }

    /* The default message of a schema: zeros, and every variable field empty. */
    static std::vector<uint8_t> default_message(const rant::Schema& schema)
    {
        if (!schema) return {};
        std::vector<uint8_t> buf(rant::detail::rant_schema_msg_min(schema.raw()));
        if (!rant::detail::rant_schema_message_default(schema.raw(), buf.data(), buf.size())) return {};
        return buf;
    }

    /* Takes what a handler queued, and parses each schema it has not seen by its wire. */
    void drain(Inbox& box, std::deque<Sample>& out)
    {
        std::map<uint64_t, std::vector<uint8_t>> wires;
        {
            std::lock_guard<std::mutex> lock(box.mutex);
            out = box.queue.take();
            for (const auto& entry : box.wires)
                if (!schemas.count(entry.first)) wires.insert(entry);
        }
        for (const auto& entry : wires) {
            rant::Schema parsed;
            try {
                parsed = node.schema_from_wire(rant::Bytes(entry.second));
            } catch (const rant::Error&) {
            }
            schemas[entry.first] = parsed;   /* empty when the wire failed: it reads as bytes */
        }
    }

    Impl(const std::string& name, const rant::NodeOptions& opts) : node(name, opts)
    {
        node.on_event([this](const rant::Event& e) {
            std::lock_guard<std::mutex> lock(mutex);
            last_event = e.to_string();
        });
    }

    uint64_t node_now_us(uint64_t now_steady) const
    {
        if (!origin_steady_us) return 0;
        return origin_node_us + (now_steady - origin_steady_us);
    }

    /* Reliable only if every publisher of the name offers it, else best effort, since a
       reliable reader refuses a best effort writer. */
    /* The publishers of a topic the mesh knows: whether there is one, and whether every
       one is reliable. */
    struct Providers {
        bool any = false, reliable = true;
    };

    Providers providers(const std::string& name) const
    {
        Providers out;
        rant::Reflection mesh = node.reflection();
        for (const rant::Peer& p : mesh.peers()) {
            if (!p.active) continue;
            for (const rant::Entity& e : mesh.entities(p.id)) {
                if (e.kind != rant::EntityKind::Topic || !e.provides || e.name != name) continue;
                out.any      = true;
                out.reliable = out.reliable && e.reliable;
            }
        }
        return out;
    }

    /* Stop delivering from the live handle and hand it to the retire retries. */
    void drop_live(Sub& s)
    {
        s.inbox.reset();   /* the handler's weak hold fails from here, so nothing more lands */
        s.call.reset();
        if (s.call_state == CallState::Waiting) s.call_state = CallState::Idle;
        if (s.live.valid()) retiring.push_back(std::move(s.live));
        s.live = Handle();
        retire_pending();
    }

    void retire_pending()
    {
        for (auto it = retiring.begin(); it != retiring.end();) {
            const rant::SendStatus st = it->close();
            if (st == rant::SendStatus::Ok || st == rant::SendStatus::NoTopic) it = retiring.erase(it);
            else ++it;
        }
    }

    bool retiring_name(const std::string& name) const
    {
        for (const Handle& h : retiring)
            if (h.name == name) return true;
        return false;
    }

    bool may_create(const std::string& name, const Sub& s, uint32_t epoch) const
    {
        return !s.live.valid() && !retiring_name(name) && !(s.failed && s.failed_epoch == epoch);
    }

    void failed(Sub& s, const Handle& h, uint32_t epoch)
    {
        s.failed = !h.valid();
        if (!s.failed) return;
        if (s.live_error.empty()) s.live_error = node.last_error();
        s.failed_epoch = epoch;
    }

    /* Watch untyped, so each value arrives with its writer's own schema. A variable is
       watched read only, so the explorer never advertises a writer. */
    void create_live(const std::string& name, Sub& s, bool variable, bool reliable, bool writable,
                     uint64_t typed, uint32_t epoch)
    {
        auto box = std::make_shared<Inbox>();
        const std::weak_ptr<Inbox> weak = box;
        Handle h;
        h.name     = name;
        h.reliable = reliable;
        h.writable = writable;
        s.live_error.clear();
        try {
            if (variable) {
                /* A read only accessor opens no set channel. One that may set takes the
                   owner's schema from the mesh, since the owner refuses untyped writes. */
                rant::VariableOptions<rant::Bytes> opts;
                opts.read_only = !writable;
                opts.reflect_from_mesh = writable;
                h.variable = node.remote_variable<rant::Bytes>(name, opts);
                /* on_change replays the current value at once, then fires on every change */
                h.variable.on_change([weak](const rant::VariableUpdate& u) {
                    if (auto in = weak.lock()) in->push(u.value(), u.raw_schema(), u.recv_us());
                });
            } else {
                rant::Qos qos;
                qos.reliability = reliable ? rant::Reliability::Reliable : rant::Reliability::BestEffort;
                if (reliable) qos.catch_up = 1;   /* a slow publisher's last value shows at once */
                /* Composing: both roles on one slot, typed, so the send reaches typed readers. */
                const auto found = typed ? schemas.find(typed) : schemas.end();
                const rant::Schema schema = found != schemas.end() ? found->second : rant::Schema();
                h.topic = node.subscriber<rant::Bytes>(name,
                    [weak](const rant::MessageView& m) {
                        if (auto in = weak.lock()) in->push(m.data(), m.raw_schema(), m.recv_us());
                    }, qos, schema);
                if (schema && h.topic.valid()) {
                    h.typed     = typed;
                    h.publisher = node.publisher<rant::Bytes>(name, qos, schema);
                }
            }
        } catch (const rant::Error& e) {
            s.live_error = e.what();
        }
        failed(s, h, epoch);
        if (s.failed) return;
        s.live  = std::move(h);
        s.inbox = box;
    }

    /* A remote function or task typed from the mesh, so a request encodes as its owner reads. */
    void create_call(const std::string& name, Sub& s, bool task, uint32_t epoch)
    {
        Handle h;
        h.name = name;
        s.live_error.clear();
        try {
            if (task) {
                rant::TaskOptions o;
                o.reflect_from_mesh = true;
                h.task = node.remote_task<rant::Bytes, rant::Bytes, rant::Bytes>(name, o);
            } else {
                rant::FunctionOptions o;
                o.reflect_from_mesh = true;
                h.function = node.remote_function<rant::Bytes, rant::Bytes>(name, o);
            }
        } catch (const rant::Error& e) {
            s.live_error = e.what();
        }
        failed(s, h, epoch);
        if (!s.failed) s.live = std::move(h);
    }

    /* A stamp on the node's clock mapped onto ours, now when there is none. */
    static double arrival_s(const Sub& s, uint64_t recv_us)
    {
        return recv_us && s.clock_offset_us != INT64_MAX ? (double)((int64_t)recv_us + s.clock_offset_us) / 1e6
                                                         : Capture::now_s();
    }

    static bool traceable(const ValueNode& node)
    {
        return node.kind == ValueNode::Number || node.kind == ValueNode::Bool || node.kind == ValueNode::Enum;
    }

    static void add_trace_points(Sub& s, const std::vector<ValueNode>& nodes, double t)
    {
        for (const ValueNode& node : nodes) {
            if (!traceable(node)) continue;
            const auto it = s.traces.find(node.path);
            if (it == s.traces.end()) continue;
            if (!it->second.empty() && t < it->second.back().t) continue;   /* never back in time */
            it->second.push_back(TracePoint{ t, node.number });
        }
    }

    /* Each streamed VideoFrame out of one decoded message, its bitstream copied whole. */
    static void add_packets(Sub& s, const std::vector<ValueNode>& nodes, const std::vector<uint8_t>& message)
    {
        for (auto& entry : s.streams) {
            const std::string prefix = entry.first.empty() ? "" : entry.first + ".";
            Capture::Packet p;
            bool held = false;
            for (const ValueNode& n : nodes) {
                if (n.path.size() <= prefix.size() || n.path.compare(0, prefix.size(), prefix) != 0) continue;
                const std::string field = n.path.substr(prefix.size());
                if (field == "codec")         p.codec    = (int)n.integer;
                else if (field == "keyframe") p.keyframe = n.number != 0;
                else if (field == "width")    p.width    = (uint32_t)n.integer;
                else if (field == "height")   p.height   = (uint32_t)n.integer;
                else if (field == "pts")      p.pts      = n.integer;
                else if (field == "data" && n.kind == ValueNode::Blob && (size_t)n.offset + n.count <= message.size()) {
                    p.data.assign(message.begin() + n.offset, message.begin() + n.offset + n.count);
                    held = true;
                }
            }
            if (!held) continue;
            Sub::Stream& st = entry.second;
            if (st.queue.push(std::move(p), STREAM_QUEUE_COUNT, STREAM_QUEUE_BYTES)) st.lost = true;
        }
    }

    /* A trace opened on a value already shown starts from it, so a plot need not wait for
       the next message. */
    static void seed_trace(Sub& s, const std::string& path)
    {
        if (s.view.last_s < 0) return;
        for (const ValueNode& node : s.view.nodes)
            if (node.path == path && traceable(node)) {
                s.traces[path].push_back(TracePoint{ s.view.last_s, node.number });
                return;
            }
    }

    static void trim_traces(Sub& s)
    {
        const double now = Capture::now_s();
        for (auto& entry : s.traces) {
            const double horizon = now - s.trace_seconds[entry.first];
            std::deque<TracePoint>& points = entry.second;
            while (points.size() > 1 && points[1].t < horizon) points.pop_front();
        }
    }

    /* The rate over a window that closes after enough messages or long enough, so a fast
       topic updates about once a second and a silent one decays to 0. */
    static void measure_rate(Sub& s)
    {
        const double now = Capture::now_s();
        if (s.rate_since <= 0) {
            s.rate_since = now;
            s.rate_seq   = s.view.seq;
            return;
        }
        const double   span  = now - s.rate_since;
        const uint64_t count = s.view.seq - s.rate_seq;
        if (span < 1 || (count < 4 && span < 4)) return;
        s.view.rate_hz = (double)count / span;
        s.rate_since   = now;
        s.rate_seq     = s.view.seq;
    }

    /* Jitter over the whole subscription: an EWMA of the spacing is the expected gap, and
       each deviation feeds an online p90 estimator, so no history is kept. */
    static void measure_gap(Sub& s, uint64_t recv_us)
    {
        if (!recv_us) return;
        if (s.gap_last_us && recv_us >= s.gap_last_us) {
            const double gap = (double)(recv_us - s.gap_last_us) / 1e3;
            if (s.gaps == 0) {
                s.gap_mean_ms = gap;
            } else {
                const double dev = std::abs(gap - s.gap_mean_ms);
                s.gap_mean_ms += 0.2 * (gap - s.gap_mean_ms);
                const double step = s.gap_mean_ms * 0.05;
                s.gap_p90_ms += dev >= s.gap_p90_ms ? step * 0.9 : -step * 0.1;
                if (s.gap_p90_ms < 0) s.gap_p90_ms = 0;
            }
            s.gaps++;
        }
        s.gap_last_us    = recv_us;
        s.view.jitter_ms = s.gaps >= 4 ? s.gap_p90_ms : -1;
    }

    static void map_clock(Sub& s, uint64_t recv_us)
    {
        const int64_t now_us = (int64_t)steady_us();
        if (recv_us && now_us - (int64_t)recv_us < s.clock_offset_us) s.clock_offset_us = now_us - (int64_t)recv_us;
    }

    /* Keeps one key's handle in line with what it is and decodes what arrived since. */
    void poll_sub(Sub& s, uint32_t epoch)
    {
        const std::string&  name = s.view.name;
        const rant::Entity* e    = entity(s.view.key);
        if (is_call(s.view.kind)) {
            poll_call(name, s, e, epoch);
            return;
        }

        const bool     variable = s.view.kind == Kind::Variable;
        const bool     writable = variable && e && e->writable;
        const uint64_t typed    = variable ? 0 : s.compose_hash;
        if (!variable && s.reliable_epoch != epoch) {
            const Providers found = providers(name);
            s.reliable_epoch = epoch;
            s.want_reliable  = found.any && found.reliable;
            s.provided       = found.any;
        }
        const bool reliable = !variable && s.want_reliable;
        if (s.live.valid() && (s.live.variable.valid() != variable || s.live.reliable != reliable ||
                               s.live.writable != writable || s.live.typed != typed))
            drop_live(s);
        if (may_create(name, s, epoch)) create_live(name, s, variable, reliable, writable, typed, epoch);
        WatchView& v = s.view;
        v.writable   = s.live.valid() && s.live.writable;
        v.publishing = s.live.valid() && s.live.publisher.valid();
        v.draftable  = !variable && (s.last_hash || (e && e->schema));
        if (v.nodes.empty() && e) v.type = root_type(e->schema);
        measure_rate(s);

        if (!s.inbox) {
            if (v.nodes.empty()) v.status = s.failed ? "cannot watch: " + s.live_error : "subscribing";
            return;
        }
        std::deque<Sample> drained;
        drain(*s.inbox, drained);
        /* A topic's match count is of subscribers, so its publishers come from the mesh. */
        if (drained.empty()) {
            if (v.seq == 0)
                v.status = variable ? (s.live.matches() > 0 ? "waiting for a value" : "no owner matched yet")
                         : s.provided ? "waiting for a value" : "no publisher matched yet";
            return;
        }
        v.seq += drained.size();
        v.status.clear();

        /* The tree shows the newest message. A traced field takes a point from every one,
           and a streamed video its frame. */
        for (const Sample& sample : drained) {
            map_clock(s, sample.recv_us);
            measure_gap(s, sample.recv_us);
        }
        const Sample& last = drained.back();
        if (!s.traces.empty() || !s.streams.empty()) {
            WatchView scratch;
            for (const Sample& sample : drained) {
                if (&sample == &last) break;
                decode(sample.data, sample.schema_hash, scratch);
                add_trace_points(s, scratch.nodes, arrival_s(s, sample.recv_us));
                add_packets(s, scratch.nodes, scratch.message);
            }
        }
        decode(last.data, last.schema_hash, v, false, {}, &s.revealed);
        s.last_hash = last.schema_hash;
        v.schema    = last.schema_hash;
        v.draftable = !variable;
        v.last_s    = arrival_s(s, last.recv_us);
        add_trace_points(s, v.nodes, v.last_s);
        add_packets(s, v.nodes, v.message);
        trim_traces(s);
        /* an Empty is a signal, so its arrivals are all there is to show */
        if (v.nodes.empty()) v.status = std::to_string(v.seq) + " received, no fields";
    }

    /* A function or task: its handle, what came back from the last call, and the form. */
    void poll_call(const std::string& name, Sub& s, const rant::Entity* e, uint32_t epoch)
    {
        const bool task = s.view.kind == Kind::Task;
        if (!e) {
            s.view.status = "no longer on the mesh";
            return;
        }
        if (e->generation != s.generation) {
            s.generation = e->generation;
            s.form_dirty = true;
            if (s.live.valid()) task ? s.live.task.refresh() : s.live.function.refresh();
        }
        if (may_create(name, s, epoch)) create_call(name, s, task, epoch);
        s.view.status = s.failed ? "cannot call: " + s.live_error : std::string();
        const bool matched = s.live.valid() && s.live.matches() > 0;
        if (matched != s.matched) {
            s.matched    = matched;
            s.form_dirty = true;
        }

        if (s.call) {
            std::deque<Sample> replies, progress;
            drain(s.call->reply, replies);
            drain(s.call->progress, progress);
            bool done, running;
            {
                std::lock_guard<std::mutex> lock(s.call->mutex);
                done    = s.call->done;
                running = s.call->running;
                if (done) {
                    s.call_status  = s.call->status;
                    s.call_message = s.call->message;
                }
            }
            for (const Sample& sample : progress) {
                map_clock(s, sample.recv_us);
                WatchView scratch;
                decode(sample.data, sample.schema_hash, scratch, false, "progress");
                add_trace_points(s, scratch.nodes, arrival_s(s, sample.recv_us));
            }
            if (!progress.empty()) {
                s.progress     = progress.back();
                s.has_progress = true;
                s.form_dirty   = true;
            }
            if (running != s.running) {
                s.running    = running;
                s.form_dirty = true;
            }
            if (!replies.empty()) {
                s.reply     = replies.back();
                s.has_reply = true;
            }
            if (done) {
                s.call_state = CallState::Done;
                s.call_took  = Capture::now_s() - s.call_start;
                s.call.reset();
                s.form_dirty = true;
            }
            trim_traces(s);
        }
        if (s.form_dirty) build_form(s, *e, task);
        s.view.calling = s.call_state == CallState::Waiting;
    }

    /* One part of a function or task: its heading, then its value under the part's prefix.
       A part with nothing in it yet shows its schema's default, which the tree blanks. */
    void add_part(WatchView& v, const char* label, const std::string& prefix, const std::vector<uint8_t>& data,
                  uint64_t hash, bool has_value, const std::string& state, uint8_t tone, bool input)
    {
        WatchView scratch;
        decode(data, hash, scratch, false, prefix);
        ValueNode part;
        part.kind  = ValueNode::Part;
        part.name  = label;
        part.path  = "@" + prefix;
        part.type  = scratch.type;
        part.text  = state;
        part.tone  = tone;
        part.count = has_value ? 1 : 0;
        part.input = input;
        v.nodes.push_back(part);
        for (ValueNode& n : scratch.nodes) {
            n.input = input;
            v.nodes.push_back(std::move(n));
        }
        v.shape += scratch.shape + (has_value ? "+" : "-") + "|";
    }

    static std::string call_word(rant::CallStatus st)
    {
        switch (st) {
        case rant::CallStatus::AppError:   return "failed";
        case rant::CallStatus::NoHandler:  return "no handler";
        case rant::CallStatus::Timeout:    return "timed out";
        case rant::CallStatus::PeerLost:   return "provider lost";
        case rant::CallStatus::Cancelled:  return "cancelled";
        case rant::CallStatus::NoProvider: return "no provider";
        default:                           return "refused";
        }
    }

    /* A fraction done from a progress field named percent, or progress or fraction. */
    static double progress_of(const std::vector<ValueNode>& nodes)
    {
        for (const ValueNode& n : nodes) {
            if (n.kind != ValueNode::Number || n.depth != 0) continue;
            if (n.name == "percent")                          return std::min(1.0, std::max(0.0, n.number / 100));
            if (n.name == "progress" || n.name == "fraction") return std::min(1.0, std::max(0.0, n.number));
        }
        return -1;
    }

    void build_form(Sub& s, const rant::Entity& e, bool task)
    {
        s.form_dirty = false;
        WatchView& v = s.view;
        v.seq++;   /* the part states and values are rewritten */
        know(e.schema);
        know(e.rsp_schema);
        know(e.progress_schema);
        if (v.request_schema != e.schema.hash() || v.request.empty()) {
            v.request        = default_message(e.schema);
            v.request_schema = e.schema.hash();
        }
        v.type = root_type(e.schema);
        v.root_struct = true;   /* the parts stand in for a root */
        v.nodes.clear();
        v.shape.clear();

        const bool   done = s.call_state == CallState::Done;
        const bool   ok   = done && s.call_status == rant::CallStatus::Ok;
        const std::string word = call_word(s.call_status);
        const std::string why  = s.call_message.empty() || s.call_message == word ? word : word + ": " + s.call_message;
        /* A part's cell holds only the word, the whole reason goes below the tree. */
        v.error = done && !ok && s.call_status != rant::CallStatus::Cancelled ? why : std::string();
        const char* idle = !s.matched ? "no provider matched yet" : task ? "not run yet" : "not called yet";
        const uint8_t bad = s.call_status == rant::CallStatus::Cancelled ? 0 : 2;
        char took[48];
        std::snprintf(took, sizeof took, s.call_took < 1 ? "%.0f ms" : "%.2f s",
                      s.call_took < 1 ? s.call_took * 1e3 : s.call_took);

        add_part(v, task ? "Goal" : "Request", task ? "goal" : "request", v.request, v.request_schema,
                 true, {}, 0, true);
        const std::vector<uint8_t> rsp_default = default_message(e.rsp_schema);
        const bool reply = s.has_reply && ok;
        if (!task) {
            const std::string state = s.call_state == CallState::Idle    ? idle
                                    : s.call_state == CallState::Waiting ? "waiting"
                                    : ok ? std::string("replied in ") + took : word;
            add_part(v, "Reply", "reply", reply ? s.reply.data : rsp_default,
                     reply ? s.reply.schema_hash : e.rsp_schema.hash(), reply, state,
                     s.call_state == CallState::Waiting ? 3 : !done ? 0 : ok ? 1 : bad, false);
            return;
        }
        WatchView progress;
        if (s.has_progress) decode(s.progress.data, s.progress.schema_hash, progress);
        v.progress = s.call_state == CallState::Waiting && s.has_progress ? progress_of(progress.nodes) : -1;
        char percent[16];
        std::snprintf(percent, sizeof percent, "%.0f%%", v.progress * 100);
        const std::string state = s.call_state == CallState::Idle ? idle
                                : s.call_state == CallState::Waiting ? (v.progress >= 0 ? percent : s.running ? "running" : "starting")
                                : ok ? "done" : call_word(s.call_status);
        add_part(v, "Progress", "progress", s.has_progress ? s.progress.data : default_message(e.progress_schema),
                 s.has_progress ? s.progress.schema_hash : e.progress_schema.hash(), s.has_progress, state,
                 s.call_state == CallState::Waiting ? 3 : !done ? 0 : ok ? 1 : bad, false);
        add_part(v, "Result", "result", reply ? s.reply.data : rsp_default,
                 reply ? s.reply.schema_hash : e.rsp_schema.hash(), reply,
                 !done ? std::string() : ok ? std::string("in ") + took : word,
                 !done ? 0 : ok ? 1 : bad, false);
    }
};

Capture::Capture(const Options& opts)
{
    rant::NodeOptions node_opts;
    node_opts.domain              = opts.domain;
    node_opts.discovery_group     = opts.discovery_group;
    node_opts.discovery_port      = opts.discovery_port;
    node_opts.multicast_interface = opts.multicast_interface;
    /* The explorer watches every topic on the mesh, so it needs every peer's schemas. */
    node_opts.fetch_details       = true;
    /* A watched image can be megabytes, and the OS default buffer drops a message that size. */
    node_opts.recv_buffer_bytes   = 8u << 20;
    /* Never stall the UI thread waiting for a match. */
    node_opts.match_wait          = std::chrono::milliseconds(-1);

    try {
        impl_ = std::make_unique<Impl>(opts.name, node_opts);
    } catch (const rant::Error& e) {
        error_ = e.what();
        return;
    }
    if (!impl_->node.valid()) {
        error_ = impl_->node.last_error();
        impl_.reset();
        return;
    }

    discovery_group_ = opts.discovery_group.empty()
                     ? "239.255.0." + std::to_string(opts.domain)
                     : opts.discovery_group;

    impl_->node.on_log([this](const rant::LogLine& line) {
        Impl::RawLog row;
        row.level   = (int)line.level;
        row.wall_us = line.wall_us;
        row.node.assign(line.node.data(), line.node.size());
        row.text.assign(line.text.data(), line.text.size());
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->log_ring.push_back(std::move(row));
        while (impl_->log_ring.size() > LOG_RING) impl_->log_ring.pop_front();
    });
}

Capture::~Capture() = default;

const char* Capture::kind_word(Kind kind)
{
    switch (kind) {
    case Kind::Function: return "function";
    case Kind::Variable: return "variable";
    case Kind::Task:     return "task";
    default:             return "topic";
    }
}

std::string Capture::key(Kind kind, const std::string& name)
{
    return std::string(kind_word(kind)) + ":" + name;
}

std::string Capture::name_of(const std::string& key)
{
    const size_t colon = key.find(':');
    return colon == std::string::npos ? key : key.substr(colon + 1);
}

void Capture::watch(const std::string& key)
{
    if (!impl_) {
        name_view(none_, key);
        return;
    }
    Impl& im = *impl_;
    if (im.watch_key == key) return;
    /* a draft belongs to the watch, so leaving the key ends it */
    if (Impl::Sub* old = im.find(im.watch_key)) old->compose_hash = 0;
    im.watch_key = key;
    name_view(none_, key);
    if (!key.empty()) im.open(key);
}

const Capture::WatchView& Capture::watched() const
{
    const Impl::Sub* s = Impl::watched(impl_.get());
    return s ? s->view : none_;
}

void Capture::subscribe(const std::string& key, bool on)
{
    if (!impl_ || key.empty()) return;
    if (on) {
        impl_->open(key).wanted = true;
    } else if (Impl::Sub* s = impl_->find(key)) {
        s->wanted = false;
    }
}

bool Capture::subscribed(const std::string& key) const
{
    const Impl::Sub* s = impl_ ? impl_->find(key) : nullptr;
    return s && s->listening() && s->wanted;
}

bool Capture::previewing(const std::string& key) const
{
    const Impl::Sub* s = impl_ && key == impl_->watch_key ? impl_->find(key) : nullptr;
    return s && s->listening() && !s->wanted;
}

const Capture::WatchView* Capture::view(const std::string& key) const
{
    const Impl::Sub* s = impl_ ? impl_->find(key) : nullptr;
    return s ? &s->view : nullptr;
}

void Capture::trace(const std::string& key, const std::vector<TraceSpec>& specs)
{
    Impl::Sub* s = impl_ ? impl_->find(key) : nullptr;
    if (!s) return;
    s->trace_seconds.clear();
    for (const TraceSpec& spec : specs)
        s->trace_seconds[spec.path] = std::max(s->trace_seconds[spec.path], spec.seconds);
    for (auto it = s->traces.begin(); it != s->traces.end();) {
        if (!s->trace_seconds.count(it->first)) it = s->traces.erase(it);
        else ++it;
    }
    for (const auto& entry : s->trace_seconds)
        if (s->traces.try_emplace(entry.first).second) Impl::seed_trace(*s, entry.first);
}

void Capture::stream(const std::string& key, const std::vector<std::string>& paths)
{
    Impl::Sub* s = impl_ ? impl_->find(key) : nullptr;
    if (!s) return;
    for (auto it = s->streams.begin(); it != s->streams.end();) {
        if (std::find(paths.begin(), paths.end(), it->first) == paths.end()) it = s->streams.erase(it);
        else ++it;
    }
    for (const std::string& path : paths) s->streams[path];
}

std::vector<Capture::Packet> Capture::take(const std::string& key, const std::string& path, bool& lost)
{
    std::vector<Packet> out;
    lost = false;
    Impl::Sub* s = impl_ ? impl_->find(key) : nullptr;
    if (!s || !s->streams.count(path)) return out;
    Impl::Sub::Stream& st = s->streams[path];
    std::deque<Packet> queued = st.queue.take();
    out.assign(std::make_move_iterator(queued.begin()), std::make_move_iterator(queued.end()));
    lost     = st.lost;
    st.lost  = false;
    return out;
}

const std::vector<Capture::ValueNode>& Capture::whole(const std::string& key) const
{
    static const std::vector<ValueNode> nothing;
    Impl::Sub* s = impl_ ? impl_->find(key) : nullptr;
    if (!s) return nothing;
    /* a call's parts are small and already whole */
    if (is_call(s->view.kind)) return s->view.nodes;
    if (s->whole_seq != s->view.seq) {
        s->whole_seq = s->view.seq;
        WatchView scratch;
        impl_->decode(s->view.message, s->last_hash, scratch, true);
        s->whole.swap(scratch.nodes);
        /* A bare root's node takes the schema's type, as the tree's does. */
        if (!s->view.root_struct && !s->whole.empty() && !s->view.nodes.empty()) {
            s->whole[0].type     = s->view.nodes[0].type;
            s->whole[0].std_name = s->view.nodes[0].std_name;
        }
    }
    return s->whole;
}

std::vector<uint8_t> Capture::encode(const std::vector<uint8_t>& base, uint64_t schema,
                                     const std::vector<ValueNode>& edits, std::string& error) const
{
    error.clear();
    if (!impl_) {
        error = "the node failed to start";
        return {};
    }
    const auto found = impl_->schemas.find(schema);
    if (found == impl_->schemas.end() || found->second.empty()) {
        error = "the value has no schema to write it with";
        return {};
    }
    /* A variable string or array grows the message in place, so leave it the room. */
    size_t room = 0;
    for (const ValueNode& e : edits) room += e.text.size() + 16;
    std::vector<uint8_t> buf(base);
    buf.resize(buf.size() + room);
    for (const ValueNode& e : edits) {
        if (!write_edit(found->second, buf, e)) {
            error = (e.path.empty() ? std::string("value") : e.path) + " does not fit its field";
            return {};
        }
    }
    buf.resize(rant::detail::rant_schema_msg_len(found->second.raw(), buf.data(), buf.size()));
    return buf;
}

std::string Capture::set(const std::vector<uint8_t>& value)
{
    Impl::Sub* s = Impl::watched(impl_.get());
    if (!s || !s->live.variable.valid()) return "no variable is watched";
    if (!s->live.writable) return "the variable is read only";
    switch (s->live.variable.set(rant::Bytes(value))) {
    case rant::SendStatus::Ok:       return {};
    case rant::SendStatus::NoTopic:  return "no owner matched";
    case rant::SendStatus::TooBig:   return "the value is too big";
    case rant::SendStatus::BadRole:  return "the owner takes no sets";
    case rant::SendStatus::Schema:   return "the owner's schema differs";
    default:                         return "the set was refused";
    }
}

void Capture::reveal(const std::string& path)
{
    Impl::Sub* s = Impl::watched(impl_.get());
    if (!s || !s->last_hash) return;
    s->revealed[path] += REVEAL_STEP;
    const std::vector<uint8_t> message = s->view.message;
    impl_->decode(message, s->last_hash, s->view, false, {}, &s->revealed);
}

/* A draft lists every element, so a fixed array shows whole and any element can be edited. */
bool Capture::draft(Draft& out) const
{
    Impl::Sub* s = Impl::watched(impl_.get());
    if (!s) return false;
    if (s->last_hash && !s->view.nodes.empty()) {
        out.message = s->view.message;
        out.schema  = s->last_hash;
    } else {
        const rant::Entity* e = impl_->entity(impl_->watch_key);
        if (!e || !e->schema) return false;
        impl_->know(e->schema);
        out.schema  = e->schema.hash();
        out.message = Impl::default_message(e->schema);
    }
    WatchView scratch;
    impl_->decode(out.message, out.schema, scratch, true);
    out.nodes = std::move(scratch.nodes);
    return true;
}

bool Capture::resize(Draft& draft, uint16_t field, int remove, std::string& error) const
{
    error.clear();
    if (!impl_) {
        error = "the node failed to start";
        return false;
    }
    const auto found = impl_->schemas.find(draft.schema);
    Field f;
    if (found == impl_->schemas.end() || !found->second.field_at(field, f) ||
        f.kind != FieldType::VArray || !f.elem_size) {
        error = "only a variable array can change its length";
        return false;
    }
    const rant::detail::RantSchema* raw = found->second.raw();
    rant::detail::RantValue now;
    if (!rant::detail::rant_get_value_at(rant::detail::rant_bytes(draft.message.data(), draft.message.size()), raw,
                                         field, nullptr, 0, &now)) {
        error = "the array could not be read";
        return false;
    }
    std::vector<uint8_t> elements;
    if (now.bytes.data) elements.assign(now.bytes.data, now.bytes.data + now.bytes.len);
    const size_t size = f.elem_size, count = elements.size() / size;
    if (remove < 0) {
        if (count) elements.insert(elements.end(), elements.end() - size, elements.end());
        else elements.resize(size, 0);
    } else if ((size_t)remove < count) {
        elements.erase(elements.begin() + remove * size, elements.begin() + (remove + 1) * size);
    }
    std::vector<uint8_t> buf(draft.message);
    buf.resize(buf.size() + size + 16);
    rant::detail::RantValue v{};
    v.bytes = rant::detail::rant_bytes(elements.data(), elements.size());
    if (!rant::detail::rant_set_value_at(buf.data(), buf.size(), raw, field, nullptr, 0, &v)) {
        error = "the array does not fit the message";
        return false;
    }
    buf.resize(rant::detail::rant_schema_msg_len(raw, buf.data(), buf.size()));
    draft.message = std::move(buf);
    WatchView scratch;
    impl_->decode(draft.message, draft.schema, scratch, true);
    draft.nodes = std::move(scratch.nodes);
    return true;
}

void Capture::compose(bool on, uint64_t schema)
{
    if (Impl::Sub* s = Impl::watched(impl_.get())) s->compose_hash = on ? schema : 0;
}

std::string Capture::publish(const std::vector<uint8_t>& message)
{
    Impl::Sub* s = Impl::watched(impl_.get());
    if (!s || !s->live.publisher.valid()) return "not ready to publish yet";
    switch (s->live.publisher.send(rant::Bytes(message))) {
    case rant::SendStatus::Ok:      return {};
    case rant::SendStatus::TooBig:  return "the message is too big";
    case rant::SendStatus::Schema:  return "the schema was refused";
    case rant::SendStatus::BadRole: return "the name cannot be published here";
    default:                        return "the send was refused";
    }
}

std::string Capture::call(const std::vector<uint8_t>& request)
{
    Impl::Sub* s = Impl::watched(impl_.get());
    if (!s || !(s->live.function.valid() || s->live.task.valid())) return "not ready to call yet";
    if (s->call_state == Impl::CallState::Waiting) return "a call is in flight";
    auto box = std::make_shared<Impl::CallBox>();
    const std::weak_ptr<Impl::CallBox> weak = box;
    auto on_response = [weak](const rant::ResponseView<rant::Bytes>& r) {
        auto b = weak.lock();
        if (!b) return;
        if (r.data().size()) b->reply.push(r.data(), r.raw_schema(), 0);
        std::lock_guard<std::mutex> lock(b->mutex);
        b->done    = true;
        b->status  = r.status();
        b->message = std::string(r.message());
    };
    rant::SendStatus st;
    if (s->live.task.valid()) {
        const rant::TaskCall tc = s->live.task.call_async(rant::Bytes(request),
            [weak](const rant::ProgressView<rant::Bytes>& p) {
                auto b = weak.lock();
                if (!b) return;
                if (p.has_value()) b->progress.push(p.data(), p.raw_schema(), p.recv_us());
                std::lock_guard<std::mutex> lock(b->mutex);
                b->running = true;
            }, on_response);
        st         = tc.status;
        s->call_id = tc.id;
    } else {
        st = s->live.function.call_async(rant::Bytes(request), on_response);
    }
    switch (st) {
    case rant::SendStatus::Ok:      break;
    case rant::SendStatus::NoTopic: return "no provider matched";
    case rant::SendStatus::TooBig:  return "the request is too big";
    case rant::SendStatus::Schema:  return "the provider's schema differs";
    default:                        return "the call was refused";
    }
    s->call         = box;
    s->call_state   = Impl::CallState::Waiting;
    s->call_start   = now_s();
    s->has_reply    = false;
    s->has_progress = false;
    s->running      = false;
    s->form_dirty   = true;
    return {};
}

std::string Capture::cancel()
{
    Impl::Sub* s = Impl::watched(impl_.get());
    if (!s || !s->live.task.valid() || s->call_state != Impl::CallState::Waiting) return "no run to cancel";
    switch (s->live.task.cancel(s->call_id)) {
    case rant::SendStatus::Ok:      return {};
    case rant::SendStatus::BadRole: return "the task cannot be cancelled";
    default:                        return "the cancel was refused";
    }
}

const std::deque<Capture::TracePoint>* Capture::trace_of(const std::string& key, const std::string& path) const
{
    const Impl::Sub* s = impl_ ? impl_->find(key) : nullptr;
    if (!s) return nullptr;
    const auto it = s->traces.find(path);
    return it == s->traces.end() ? nullptr : &it->second;
}

double Capture::now_s()
{
    return (double)steady_us() / 1e6;
}

bool Capture::ok() const
{
    return impl_ != nullptr;
}

void Capture::select(const std::string& id)
{
    if (selected_id_ == id) return;
    selected_id_ = id;
    /* Drop what belonged to the old selection rather than showing it under the new name. */
    publishes_.clear();
    subscribes_.clear();
    meta_ = MetaView();
    if (impl_) {
        impl_->endpoints_id.clear();
        impl_->endpoints_epoch = 0xFFFFFFFFu;
        impl_->prev_cpu_us = impl_->prev_cpu_wall_us = 0;
        impl_->cpu_pct = -1;
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->meta_fresh    = false;
        impl_->meta_failures = 0;
    }
}

void Capture::poll()
{
    if (!impl_) return;

    const uint64_t now_steady = steady_us();
    rant::Reflection mesh = impl_->node.reflection();
    const std::vector<rant::Peer> peers = mesh.peers();
    const uint32_t mesh_epoch = mesh.epoch();

    /* Slide the clock origin so node time never reads earlier than an announce we hold. */
    for (const rant::Peer& p : peers) {
        if (p.last_heard_us > impl_->node_now_us(now_steady)) {
            impl_->origin_node_us   = p.last_heard_us;
            impl_->origin_steady_us = now_steady;
        }
    }
    const uint64_t node_now = impl_->node_now_us(now_steady);

    for (auto& entry : impl_->tracks) entry.second.seen = false;

    /* Group by host. A handful of hosts, so a linear find beats a map. */
    machines_.clear();
    node_count_ = 0;
    uint32_t selected_peer = 0;
    bool     have_selection = false;
    uint32_t selected_epoch = 0;

    for (const rant::Peer& p : peers) {
        if (!p.active) continue;   /* dropped peers stay listed, the UI never shows them */

        const std::string id = format_hex(p.uuid.data(), p.uuid.size());
        Impl::Track& track = impl_->tracks[id];
        track.seen = true;
        if (!track.first_seen_us) {
            track.first_seen_us = now_steady;
            track.epoch         = p.epoch;
            impl_->graph_dirty  = true;
        } else if (p.epoch != track.epoch) {
            track.epoch = p.epoch;
            track.updates++;
        }

        /* The reflection walk is O(topics) per peer, so only redo it when the peer's
           announce moved, or the mesh did, since names arrive with the details later. */
        if (track.counted_epoch != p.epoch || track.counted_mesh != mesh_epoch) {
            track.counted_epoch = p.epoch;
            track.counted_mesh  = mesh_epoch;
            track.publishes = track.subscribes = 0;
            track.ends.clear();
            for (const rant::Entity& e : mesh.entities(p.id)) {
                if (e.provides) track.publishes++;
                if (e.consumes) track.subscribes++;
                track.ends.push_back(Impl::Track::End{ e.name, kind_of(e.kind), e.provides, e.consumes, e.reliable });
            }
            impl_->graph_dirty = true;
        }
        track.peer = p.id;
        if (traffic_on_) impl_->ask_traffic(mesh, id, p.id, now_steady);

        NodeRow row;
        row.id            = id;
        row.name          = p.name;
        row.address       = p.address;
        /* A peer that advertises newer state than we hold is still settling. */
        row.alive         = !p.catching_up;
        row.rtt_samples   = (int)p.rtt_samples;
        if (p.rtt_samples) {
            row.rtt_ms     = p.rtt_us / 1e3;
            row.jitter_ms  = p.rtt_jitter_us / 1e3;
            row.rtt_min_ms = p.rtt_min_us / 1e3;
        }
        if (node_now && p.last_heard_us)
            row.last_heard_s = (double)(node_now - p.last_heard_us) / 1e6;
        row.observed_s    = (double)(now_steady - track.first_seen_us) / 1e6;
        row.updates       = track.updates;
        row.fragment_size = p.fragment_size ? (int)p.fragment_size : -1;
        row.publishes     = track.publishes;
        row.subscribes    = track.subscribes;

        if (id == selected_id_) {
            selected_peer  = p.id;
            have_selection = true;
            selected_epoch = p.epoch;
        }

        const std::string host = host_of(p.address);
        auto machine = std::find_if(machines_.begin(), machines_.end(),
                                    [&host](const MachineRow& m) { return m.host == host; });
        if (machine == machines_.end()) {
            machines_.push_back(MachineRow{ host.empty() ? "(unknown host)" : host, {} });
            machine = machines_.end() - 1;
        }
        machine->nodes.push_back(std::move(row));
        node_count_++;
    }

    for (auto it = impl_->tracks.begin(); it != impl_->tracks.end();) {
        if (it->second.seen) {
            ++it;
            continue;
        }
        impl_->traffic.erase(it->first);
        impl_->graph_dirty = true;
        it = impl_->tracks.erase(it);
    }

    std::sort(machines_.begin(), machines_.end(),
              [](const MachineRow& a, const MachineRow& b) { return a.host < b.host; });
    for (MachineRow& m : machines_)
        std::sort(m.nodes.begin(), m.nodes.end(),
                  [](const NodeRow& a, const NodeRow& b) { return a.name < b.name; });

    impl_->selected_peer  = selected_peer;
    impl_->have_selection = have_selection;

    if (impl_->fold_traffic() || impl_->graph_dirty) build_mesh(now_steady);

    /* The endpoint lists follow the same epoch rule as the counts. */
    if (have_selection &&
        (impl_->endpoints_id != selected_id_ || impl_->endpoints_epoch != selected_epoch ||
         impl_->endpoints_mesh != mesh_epoch)) {
        impl_->endpoints_id    = selected_id_;
        impl_->endpoints_epoch = selected_epoch;
        impl_->endpoints_mesh  = mesh_epoch;
        publishes_.clear();
        subscribes_.clear();
        for (const rant::Entity& e : mesh.entities(selected_peer)) {
            EndpointRow row;
            row.name     = e.name;
            row.kind     = kind_of(e.kind);
            row.reliable = e.reliable;
            if (e.provides) {
                row.count = e.providers;
                publishes_.push_back(row);
            }
            if (e.consumes) {
                row.count = e.consumers;
                subscribes_.push_back(row);
            }
        }
        std::sort(publishes_.begin(), publishes_.end(),
                  [](const EndpointRow& a, const EndpointRow& b) { return a.name < b.name; });
        std::sort(subscribes_.begin(), subscribes_.end(),
                  [](const EndpointRow& a, const EndpointRow& b) { return a.name < b.name; });
    } else if (!have_selection) {
        publishes_.clear();
        subscribes_.clear();
    }

    /* The mesh fold is O(entities), so it is walked only when the mesh moved. */
    if (mesh_epoch != impl_->mesh_epoch || node_count_ != impl_->mesh_nodes) {
        impl_->mesh_epoch = mesh_epoch;
        impl_->mesh_nodes = node_count_;
        topics_.clear();
        impl_->entities.clear();
        std::map<std::string, std::vector<std::string>> nodes_of;
        std::map<std::string, std::vector<Endpoint>>    ends_of;   /* by key */
        for (const rant::Peer& p : peers) {
            if (!p.active) continue;
            for (rant::Entity& e : mesh.entities(p.id)) {
                std::vector<std::string>& names = nodes_of[e.name];
                if (std::find(names.begin(), names.end(), p.name) == names.end()) names.push_back(p.name);
                ends_of[key(kind_of(e.kind), e.name)].push_back(Endpoint{ p.id, p.name, std::move(e) });
            }
        }
        for (const rant::Entity& e : mesh.mesh()) {
            impl_->know(e.schema);
            impl_->know(e.rsp_schema);
            impl_->know(e.progress_schema);
            TopicRow row;
            row.key       = key(kind_of(e.kind), e.name);
            impl_->entities[row.key] = e;
            row.name      = e.name;
            row.kind      = kind_of(e.kind);
            row.note      = entity_note(e);
            row.from      = e.from;
            row.type      = root_type(e.schema);
            row.reliable  = e.reliable;
            row.writable  = e.kind == rant::EntityKind::Variable && e.writable;
            row.nodes     = nodes_of[e.name];
            row.issues    = match_issues(row.kind, ends_of[row.key]);
            /* The mesh fold also flags rival providers, which no pair above names. */
            if (e.conflict && row.issues.empty())
                row.issues.push_back("its nodes declare schemas that cannot read each other");
            topics_.push_back(std::move(row));
        }
        /* A name offered as two kinds never connects, so every row of it says so first. */
        std::map<std::string, std::vector<size_t>> rows_of;
        for (size_t i = 0; i < topics_.size(); i++) rows_of[topics_[i].name].push_back(i);
        for (TopicRow& row : topics_) {
            std::vector<std::string> kinds;
            for (const size_t i : rows_of[row.name]) {
                if (topics_[i].kind == row.kind) continue;
                const std::string nodes = node_list(ends_of[topics_[i].key]);
                kinds.push_back(std::string("also a ") + kind_word(topics_[i].kind) +
                                (nodes.empty() ? "" : " on " + nodes) + ", a name can be one kind only");
            }
            row.issues.insert(row.issues.begin(), kinds.begin(), kinds.end());
        }
        topics_epoch_++;
    }

    /* Every subscription and the watch. A name neither is kept for is dropped. */
    impl_->retire_pending();
    for (auto it = impl_->subs.begin(); it != impl_->subs.end();) {
        Impl::Sub& s = it->second;
        if (it->first != impl_->watch_key && (!s.wanted || is_call(s.view.kind))) {
            impl_->drop_live(s);
            it = impl_->subs.erase(it);
            continue;
        }
        impl_->poll_sub(s, mesh_epoch);
        ++it;
    }

    /* The meta poll: one directed call per second at the selection. Topics are left out,
       that section is about 250 B per topic and the Nodes tab does not read it. */
    if (have_selection && !impl_->meta_pending &&
        now_steady - impl_->last_meta_us >= META_PERIOD_US) {
        impl_->last_meta_us  = now_steady;
        impl_->meta_pending  = true;
        const rant::SendStatus sent = mesh.meta_async(
            selected_peer,
            [this](const rant::MetaSnapshot& snap) {
                std::lock_guard<std::mutex> lock(impl_->mutex);
                impl_->meta_pending = false;
                if (snap.valid) {
                    impl_->meta          = snap;
                    impl_->meta_fresh    = true;
                    impl_->meta_at_us    = steady_us();
                    impl_->meta_failures = 0;
                } else if (impl_->meta_failures < 1000) {
                    impl_->meta_failures++;
                }
            },
            rant::MetaNode | rant::MetaProc | rant::MetaPeers);
        if (sent != rant::SendStatus::Ok) impl_->meta_pending = false;
    }

    /* Copy the meta snapshot and the log ring out from under the service thread. */
    rant::MetaSnapshot snap;
    bool               snap_fresh = false;
    int                failures   = 0;
    std::vector<Impl::RawLog> lines;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        snap       = impl_->meta;
        snap_fresh = impl_->meta_fresh;
        failures   = impl_->meta_failures;
        lines.assign(impl_->log_ring.begin(), impl_->log_ring.end());
    }

    /* Find the selection's name once, for the meta match and the log filter. */
    std::string selected_name;
    for (const MachineRow& m : machines_)
        for (const NodeRow& n : m.nodes)
            if (n.id == selected_id_) selected_name = n.name;

    meta_ = MetaView();
    meta_.fresh = have_selection && snap_fresh && !selected_name.empty() &&
                  snap.node.name == selected_name;
    if (!meta_.fresh)
        meta_.status = (have_selection && failures >= 2) ? "no @rant/meta response"
                                                         : "querying...";
    if (meta_.fresh) {
        const uint64_t now_wall = wall_us();
        meta_.uptime_s       = snap.node.uptime_us / 1e6;
        meta_.age_s          = (snap.node.wall_us && now_wall > snap.node.wall_us)
                             ? (double)(now_wall - snap.node.wall_us) / 1e6 : 0.0;
        meta_.peers          = (double)snap.node.peers;
        meta_.max_peers      = (double)snap.node.max_peers;
        meta_.topics         = (double)snap.node.topics;
        meta_.max_topics     = (double)snap.node.max_topics;
        meta_.mem_in_use     = (double)snap.node.mem_in_use;
        meta_.mem_peak       = (double)snap.node.mem_peak;
        meta_.alloc_calls    = (double)snap.node.alloc_calls;
        meta_.evicted_unsent = (double)snap.node.evicted_unsent;
        meta_.bp_waits       = (double)snap.node.bp_waits;
        meta_.bp_waited_s    = snap.node.bp_waited_us / 1e6;
        meta_.shm_tx         = (double)snap.node.shm_tx;
        meta_.shm_rx         = (double)snap.node.shm_rx;
        meta_.last_error     = snap.node.last_error
                             ? (snap.node.last_error_text.empty() ? "(no text)"
                                                                  : snap.node.last_error_text)
                             : std::string();

        meta_.have_proc = snap.proc.have;
        meta_.pid       = (double)snap.proc.pid;
        if (snap.proc.have) {
            meta_.rss          = (double)snap.proc.rss;
            meta_.peak_rss     = (double)snap.proc.peak_rss;
            meta_.heap_total   = (double)snap.proc.heap_total;
            meta_.heap_free    = (double)snap.proc.heap_free;
            meta_.heap_min_free= (double)snap.proc.heap_min_free;
            meta_.heap_largest = (double)snap.proc.heap_largest_free_block;
        }
        /* CPU time is cumulative, so the share comes from the gap between two replies. It
           is recomputed only when a reply is new, and the last value holds in between. */
        if (snap.proc.have_cpu && snap.node.wall_us != impl_->prev_cpu_wall_us) {
            if (impl_->prev_cpu_wall_us && snap.node.wall_us > impl_->prev_cpu_wall_us) {
                const double span_us = (double)(snap.node.wall_us - impl_->prev_cpu_wall_us);
                const double used_us = (double)(snap.proc.cpu_us - impl_->prev_cpu_us);
                if (used_us >= 0.0) impl_->cpu_pct = 100.0 * used_us / span_us;
            }
            impl_->prev_cpu_us      = snap.proc.cpu_us;
            impl_->prev_cpu_wall_us = snap.node.wall_us;
        }
        meta_.cpu_pct = impl_->cpu_pct;

        const auto peers_it = snap.info.find("peers");
        if (peers_it != snap.info.end() && peers_it->second.is_array()) {
            for (const rant::MapItem& item : peers_it->second.as_array()) {
                if (!item.is_map()) continue;
                const rant::MapDict& d = item.as_map();
                MetaPeerRow row;
                row.name         = dict_string(d, "name");
                row.active       = dict_bool(d, "active");
                row.publish_to   = (int)dict_uint(d, "publishTo");
                row.receive_from = (int)dict_uint(d, "receiveFrom");
                row.samples      = (int)dict_uint(d, "rttSamples");
                if (row.samples) {
                    row.rtt_ms     = dict_uint(d, "rttUs") / 1e3;
                    row.jitter_ms  = dict_uint(d, "rttJitterUs") / 1e3;
                    row.rtt_min_ms = dict_uint(d, "rttMinUs") / 1e3;
                }
                meta_.peer_rows.push_back(std::move(row));
            }
        }
    }

    latest_log_ = lines.empty() ? LogRow() : Impl::row(lines.back());

    /* The log sidebar: this node's lines, newest first, filtered by the level pills. */
    log_.clear();
    if (!selected_name.empty()) {
        for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
            if (it->node != selected_name) continue;
            if (!(log_mask_ & (1u << it->level))) continue;
            log_.push_back(Impl::row(*it));
        }
    }
}

double Capture::traffic_hz(const std::string& key) const
{
    const auto it = traffic_hz_.find(key);
    return it == traffic_hz_.end() ? -1 : it->second;
}

void Capture::build_mesh(uint64_t now_us)
{
    impl_->graph_dirty = false;
    mesh_ = MeshView();
    traffic_hz_.clear();

    struct Sides {
        std::string      name;
        Kind             kind = Kind::Topic;
        bool             reliable = false;
        std::vector<int> providers, consumers;
    };
    std::map<std::string, Sides> names;
    for (const MachineRow& m : machines_)
        for (const NodeRow& n : m.nodes) {
            const auto track = impl_->tracks.find(n.id);
            if (track == impl_->tracks.end()) continue;
            const int at = (int)mesh_.nodes.size();
            const auto t = impl_->traffic.find(n.id);
            MeshNode node;
            node.id      = n.id;
            node.name    = n.name;
            node.host    = m.host;
            node.alive   = n.alive;
            node.reports = t != impl_->traffic.end() && t->second.at_us &&
                           now_us - t->second.at_us <= 3 * TRAFFIC_PERIOD_US;
            mesh_.nodes.push_back(std::move(node));
            for (const Impl::Track::End& e : track->second.ends) {
                Sides& sides   = names[key(e.kind, e.name)];
                sides.name     = e.name;
                sides.kind     = e.kind;
                sides.reliable = sides.reliable || e.reliable;
                if (e.provides) sides.providers.push_back(at);
                if (e.consumes) sides.consumers.push_back(at);
            }
        }

    /* The channels a name rides on, each with the side that sends on it. A call's rate is
       its requests: the answers and the progress only keep it active. */
    struct Lane {
        const char* suffix;
        bool        by_provider;
        bool        counted;
    };
    static const std::vector<Lane> TOPIC = { { "", true, true } };
    static const std::vector<Lane> VARIABLE = { { "", true, true }, { "@set", false, true } };
    static const std::vector<Lane> FUNCTION = { { "@req", false, true }, { "@rsp", true, false } };
    static const std::vector<Lane> TASK = { { "@req", false, true }, { "@prg", true, false }, { "@rsp", true, false } };

    for (const auto& entry : names) {
        const Sides&       sides = entry.second;
        const std::string& name  = sides.name;
        const std::vector<Lane>& lanes = sides.kind == Kind::Variable ? VARIABLE
                                       : sides.kind == Kind::Function ? FUNCTION
                                       : sides.kind == Kind::Task     ? TASK : TOPIC;

        /* The tree's rate: what every sender committed on the primary channel, or what the
           busiest receiver got when no sender reports. */
        double tx_sum = -1, rx_most = -1;
        for (const MeshNode& node : mesh_.nodes)
            if (const Impl::Channel* c = impl_->channel(node.id, name + (is_call(sides.kind) ? "@req" : ""), now_us)) {
                if (c->tx_hz >= 0) tx_sum = std::max(tx_sum, 0.0) + c->tx_hz;
                rx_most = std::max(rx_most, c->rx_hz);
            }
        if (tx_sum >= 0 || rx_most >= 0) traffic_hz_[entry.first] = std::max(tx_sum, rx_most);

        if (sides.consumers.empty())
            for (const int p : sides.providers) mesh_.unheard.push_back(MeshEnd{ p, name, sides.kind });
        if (sides.providers.empty())
            for (const int c : sides.consumers) mesh_.waiting.push_back(MeshEnd{ c, name, sides.kind });

        for (const int p : sides.providers)
            for (const int c : sides.consumers) {
                if (p == c) continue;
                MeshLink link;
                link.from     = p;
                link.to       = c;
                link.name     = name;
                link.kind     = sides.kind;
                link.reliable = sides.reliable;
                /* A lane carries at most what its sender sent and its receiver got, since
                   either may also talk to others. One side alone stands in for both. */
                uint64_t moved = 0;
                for (const Lane& lane : lanes) {
                    const std::string& sender   = mesh_.nodes[lane.by_provider ? p : c].id;
                    const std::string& receiver = mesh_.nodes[lane.by_provider ? c : p].id;
                    const Impl::Channel* s = impl_->channel(sender, name + lane.suffix, now_us);
                    const Impl::Channel* r = impl_->channel(receiver, name + lane.suffix, now_us);
                    const double sent = s ? s->tx_hz : -1, got = r ? r->rx_hz : -1;
                    const double hz   = sent >= 0 && got >= 0 ? std::min(sent, got) : std::max(sent, got);
                    if (hz >= 0 && lane.counted) link.hz = std::max(link.hz, 0.0) + hz;
                    if (sent >= 0 && lane.counted) link.sent_hz = std::max(link.sent_hz, 0.0) + sent;
                    /* The sender sent lately and the receiver got nothing from anyone well
                       before that. A response goes to one caller, so it proves nothing. */
                    const uint64_t lost_us = (uint64_t)(LOST_S * 1e6);
                    if (s && r && std::string(lane.suffix) != "@rsp" && s->tx_moved_us &&
                        now_us - s->tx_moved_us < (uint64_t)(ACTIVE_S * 1e6) &&
                        s->tx_moved_us > std::max(r->rx_moved_us, r->first_us) + lost_us)
                        link.lost = true;
                    const uint64_t at = s && r ? std::min(s->tx_moved_us, r->rx_moved_us)
                                      : s ? s->tx_moved_us : r ? r->rx_moved_us : 0;
                    moved = std::max(moved, at);
                }
                link.active = moved && now_us - moved < (uint64_t)(ACTIVE_S * 1e6);
                mesh_.links.push_back(std::move(link));
            }
    }
    mesh_epoch_++;
}
