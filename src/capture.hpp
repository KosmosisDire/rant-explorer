/* The Rant side of the explorer: one node on the mesh, refreshed once per frame into the
   plain structs below. The wrapper stays behind a pimpl so rant.hpp compiles in exactly
   one translation unit, and nothing here knows about RmlUi. */
#ifndef CAPTURE_HPP
#define CAPTURE_HPP

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

class Capture {
public:
    struct Options {
        std::string name;                  /* empty = the node generates one */
        std::string discovery_group;
        std::string multicast_interface;
        uint16_t    domain = 0;
        uint16_t    discovery_port = 0;
    };

    /* What a name on the mesh is. The document binds it as its word. */
    enum class Kind : uint8_t { Topic, Function, Variable, Task };
    static const char* kind_word(Kind kind);
    /* A function or a task is called, never subscribed. */
    static bool is_call(Kind kind) { return kind == Kind::Function || kind == Kind::Task; }

    /* An array longer than this keeps only its first and last few elements in the value
       nodes, with a Gap node for the rest. */
    static constexpr uint32_t ARRAY_WHOLE = 16;

    /* One discovered node. Negative means the value is not observable yet, which the UI
       spells as a dash. */
    struct NodeRow {
        std::string id;          /* the peer uuid in hex: the selection key, stable across polls */
        std::string name;
        std::string address;     /* "ip:port", the unicast locator */
        bool        alive = false;
        double      rtt_ms = -1, jitter_ms = -1, rtt_min_ms = -1;
        int         rtt_samples = 0;
        double      last_heard_s = -1;   /* since its last announce reached us */
        double      observed_s = -1;     /* since we first saw it */
        int         updates = 0;         /* announce changes we have counted */
        int         fragment_size = -1;
        int         publishes = 0, subscribes = 0;
    };

    /* The left list groups nodes by the host they run on. */
    struct MachineRow {
        std::string          host;
        std::vector<NodeRow> nodes;
    };

    /* One entry of the selected node's PUBLISHES or SUBSCRIBES list. */
    struct EndpointRow {
        std::string name;
        Kind        kind = Kind::Topic;
        bool        reliable = false;
        int         count = 0;   /* live endpoints on this side */

        bool operator==(const EndpointRow& o) const
        {
            return name == o.name && kind == o.kind && reliable == o.reliable && count == o.count;
        }
    };

    /* The selected node's own view of one of its peers, from its @rant/meta reply. */
    struct MetaPeerRow {
        std::string name;
        bool        active = false;
        double      rtt_ms = -1, jitter_ms = -1, rtt_min_ms = -1;
        int         samples = 0;
        int         publish_to = 0, receive_from = 0;
    };

    /* The selected node's internals, from the 1 Hz directed @rant/meta call. */
    struct MetaView {
        bool        fresh = false;      /* it answered, for this node */
        std::string status;             /* why not fresh, empty when it is */

        /* Counts are doubles so every member binds as one scalar type to the data model. */
        double   uptime_s = -1, age_s = -1;
        double   peers = 0, max_peers = 0, topics = 0, max_topics = 0;
        double   mem_in_use = -1, mem_peak = -1;
        double   alloc_calls = -1, evicted_unsent = -1;
        double   bp_waits = -1, bp_waited_s = -1;
        double   shm_tx = -1, shm_rx = -1;
        std::string last_error;

        bool     have_proc = false;
        double   cpu_pct = -1;
        double   pid = 0;
        double   rss = -1, peak_rss = -1;
        double   heap_total = -1, heap_free = -1, heap_min_free = -1, heap_largest = -1;

        std::vector<MetaPeerRow> peer_rows;
    };

    /* One name on the mesh, folded across every node that advertises it. */
    struct TopicRow {
        std::string name;
        Kind        kind = Kind::Topic;
        std::string note;        /* read-only, half advertised, schema conflict: joined, or empty */
        std::string from;        /* the node the schema was read from, empty while untyped */
        std::string type;        /* the advertised root type, empty while untyped */
        bool        reliable = false;
        bool        writable = false;   /* a variable whose owner takes remote sets */
        std::vector<std::string> nodes;   /* every node that provides or consumes it */

        bool operator==(const TopicRow& o) const
        {
            return name == o.name && kind == o.kind && note == o.note && from == o.from && type == o.type &&
                   reliable == o.reliable && writable == o.writable && nodes == o.nodes;
        }
    };

    /* One line off the mesh wide @rant/log stream, already filtered to the selection. */
    struct LogRow {
        int         level = 2;   /* 0 error, 1 warn, 2 info */
        std::string time;        /* local HH:MM:SS */
        std::string node;
        std::string text;

        bool operator==(const LogRow& o) const
        {
            return level == o.level && time == o.time && node == o.node && text == o.text;
        }
    };

    /* One node of the watched topic's latest value, parents before children, in the order
       the value tree lists them. A long array keeps its first five and last three elements
       and a Gap node stands for the rest. */
    struct ValueNode {
        /* A Part heads one part of a function or task: name is its label, text its state. */
        enum Kind : uint8_t { Number, Bool, Enum, Text, Struct, Array, Blob, Map, Gap, Part };
        Kind        kind = Number;
        std::string name;          /* "x", "[3]", or "value" for a root that is not a struct */
        std::string path;          /* "pose.translation.x", "points[3].id", "" for that root */
        std::string type;          /* the DSL spelling: "f64", "Double3", "f32[]", "string<16>" */
        std::string std_name;      /* the standard type's name, empty for any other type */
        int         depth = 0;
        bool        is_float = false, is_unsigned = false;
        double      number = 0;    /* every number, bool and enum, for drawing */
        int64_t     integer = 0;   /* an integer exactly, unsigned ones by bit pattern */
        uint32_t    count = 0;     /* array elements, blob bytes, map entries, or elements a gap hides */
        std::string text;          /* a string's contents, an enum's option name, a blob's first bytes */
        uint32_t    offset = 0;    /* where a blob's bytes start in WatchView::message */
        Kind        elem = Number; /* an array's element kind */
        std::string elem_std;      /* and the element's standard type name */

        /* Where the value lives, so an edit can be written back: the flat field index, the
           index in each enclosing struct array, and the index in a plain array or -1. */
        uint16_t              field = 0xFFFF;
        std::vector<uint32_t> elems;
        int32_t               element = -1;
        uint16_t              cap = 0;   /* a fixed string's capacity in bytes, 0 for unbounded */
        std::vector<std::string> options;   /* an enum's option names, in schema order */
        std::vector<int64_t>     option_values;
        bool        input = false;   /* a field of a request or a goal, which the user fills in */
        uint8_t     tone = 0;        /* a part's state: 0 plain, 1 ok, 2 bad, 3 busy */
    };

    /* One sample of a traced field: when it arrived, on our steady clock, and its value. */
    struct TracePoint {
        double t = 0;
        double v = 0;
    };

    /* One subscribed name and its latest value, decoded with the writer's schema. A function
       or task holds its parts instead: the request or goal to fill in, then what came back. */
    struct WatchView {
        std::string name;          /* empty while nothing is watched */
        Kind        kind = Kind::Topic;
        std::string status;        /* why no value shows yet, empty once one has arrived */
        std::string type;          /* the root type as the DSL spells it */
        std::string root_std;      /* the root's standard type name, empty for any other */
        bool        root_struct = false;   /* the root is a struct, so the tree lists its fields */
        uint64_t    seq = 0;       /* messages received since the watch began */
        bool        writable = false;   /* a variable this node can set */
        bool        publishing = false; /* a topic opened to publish, see compose() */
        uint64_t    schema = 0;         /* the newest message's schema hash, 0 while untyped */
        /* The schema and every array's length. When it moves the tree is rebuilt, otherwise
           only the values are written. */
        std::string shape;
        std::vector<ValueNode> nodes;
        std::vector<uint8_t>   message;   /* the newest message's bytes, for a blob's whole data */
        bool        draftable = false;   /* a message or an advertised schema to compose from */

        /* Arrivals per second over the last window, the p90 spread of their spacing in ms,
           and when the newest landed, on now_s(). Negative until measured. */
        double      rate_hz = -1, jitter_ms = -1, last_s = -1;

        /* A function's request or a task's goal: the default message the form edits. */
        std::vector<uint8_t> request;
        uint64_t    request_schema = 0;
        bool        calling = false;     /* a call or a run is in flight */
        double      progress = -1;       /* a running task's fraction done, negative when unknown */
        std::string error;               /* why the last call or run failed, whole, else empty */
    };

    /* What a composed message starts from: the newest message, else the default of the
       schema the mesh advertises, decoded as the tree shows it. */
    struct Draft {
        std::vector<uint8_t>   message;
        uint64_t               schema = 0;
        std::vector<ValueNode> nodes;
    };

    explicit Capture(const Options& opts);
    ~Capture();

    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;

    bool               ok()    const;
    const std::string& error() const { return error_; }

    /* The discovery group this node actually joined, for the DISCOVERY panel. */
    const std::string& discovery_group() const { return discovery_group_; }

    /* Refresh everything below. Once per frame. */
    void poll();

    /* Aim the endpoint lists, the meta poll and the log filter at one node, by NodeRow::id.
       An empty id watches nothing. */
    void select(const std::string& id);
    const std::string& selected() const { return selected_id_; }

    /* The one name the value tree shows, subscribed while it is watched. An empty name
       watches nothing. A subscription is untyped, so each message decodes with its
       writer's schema. A function or task is not subscribed: its form is built instead. */
    void watch(const std::string& name);
    const WatchView& watched() const;

    /* Keeps a topic or variable subscribed whether or not it is watched, for the topic
       tree's columns and for cards pinned from it. subscribed() is that choice, once its
       handle is live. */
    void subscribe(const std::string& name, bool on);
    bool subscribed(const std::string& name) const;
    /* The watch is live but not kept by subscribe(), so it ends when the watch moves. */
    bool previewing(const std::string& name) const;
    /* A subscribed or watched name's view, null for any other. */
    const WatchView* view(const std::string& name) const;

    /* The newest value with every array element, not only the tree's window. Decoded on
       the first call after a message and kept until the next, for cards that draw a whole
       array. Empty for a name that is not subscribed. */
    const std::vector<ValueNode>& whole(const std::string& name) const;

    /* The scalar fields of the watch that keep a history, by path, each with how many
       seconds it holds. Every message adds a point, so a plot misses nothing between
       frames. A trace keeps one point older than its span, so a line runs off the edge
       instead of ending short of it. */
    struct TraceSpec {
        std::string path;
        double      seconds = 10;
    };
    void trace(const std::string& name, const std::vector<TraceSpec>& specs);
    const std::deque<TracePoint>* trace_of(const std::string& name, const std::string& path) const;
    /* The clock trace points are on, in seconds. */
    static double now_s();

    /* One frame of a standard VideoFrame: its fields and a copy of its bitstream. */
    struct Packet {
        int                  codec = 0;   /* the codec enum's value, 0 when unstated */
        bool                 keyframe = false;
        uint32_t             width = 0, height = 0;
        int64_t              pts = 0;
        std::vector<uint8_t> data;
    };
    /* The VideoFrame fields a card decodes, by path. Every message queues its frame at each
       path until taken, since a decoder needs the whole stream in order. */
    void stream(const std::string& name, const std::vector<std::string>& paths);
    /* The frames queued at path, oldest first. lost says the queue overflowed since the
       last take, so the decoder must start over at a keyframe. */
    std::vector<Packet> take(const std::string& name, const std::string& path, bool& lost);

    /* A message in the schema with that hash, base with the edited nodes written into it by
       their addressing. Empty with the reason in error when a value does not fit its field. */
    std::vector<uint8_t> encode(const std::vector<uint8_t>& base, uint64_t schema,
                                const std::vector<ValueNode>& edits, std::string& error) const;

    /* Writes the watched variable. Empty on success, else why it was refused. */
    std::string set(const std::vector<uint8_t>& value);

    /* What composing the watched topic starts from. False while neither a message nor an
       advertised schema is known. */
    bool draft(Draft& out) const;

    /* While on, the watched topic is one typed topic that both reads and publishes, in the
       draft's schema. An untyped subscriber cannot share its name with a typed publisher,
       so the watch is reopened either way. */
    void compose(bool on, uint64_t schema);
    /* Sends one message on the watched topic. Empty on success, else why it was refused. */
    std::string publish(const std::vector<uint8_t>& message);

    /* Calls the watched function or runs the watched task with that request, and cancels
       the run. Empty on success, else why it was refused. */
    std::string call(const std::vector<uint8_t>& request);
    std::string cancel();

    /* Which log levels reach log(). Bit 0 error, 1 warn, 2 info. */
    void     set_log_levels(unsigned mask) { log_mask_ = mask; }

    const std::vector<MachineRow>&  machines()   const { return machines_; }
    int                             node_count() const { return node_count_; }
    const std::vector<EndpointRow>& publishes()  const { return publishes_; }
    const std::vector<EndpointRow>& subscribes() const { return subscribes_; }
    const MetaView&                 meta()       const { return meta_; }
    const std::vector<LogRow>&      log()        const { return log_; }
    const std::vector<TopicRow>&    topics()     const { return topics_; }
    /* Bumps when topics() was rebuilt, so a tree over it is rebuilt only then. */
    uint32_t                        topics_epoch() const { return topics_epoch_; }
    /* The newest line from any node at any level, empty text while there is none. */
    const LogRow&                   latest_log() const { return latest_log_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::vector<MachineRow>  machines_;
    int                      node_count_ = 0;
    std::vector<EndpointRow> publishes_, subscribes_;
    MetaView                 meta_;
    std::vector<LogRow>      log_;
    LogRow                   latest_log_;
    std::vector<TopicRow>    topics_;
    uint32_t                 topics_epoch_ = 0;
    WatchView                none_;   /* what watched() reads while nothing is */

    std::string selected_id_;
    std::string error_;
    std::string discovery_group_;
    unsigned    log_mask_ = 0x7;
};

#endif /* CAPTURE_HPP */
