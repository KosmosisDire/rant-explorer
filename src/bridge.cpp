#include "bridge.hpp"

#include "values.hpp"

#include <emscripten.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

/* From rant_web.cpp, the unit that holds the implementation. */
extern "C" const rant::detail::RantSchema* rant_web_schema_compile(const char* text, const char** why);
extern "C" const rant::detail::RantSchema* rant_web_schema_parse(const void* wire, size_t size);

/* The page side: web/explorer.js sets globalThis.rantWeb before the module starts. The sink
   turns what the page hands over into the calls exported below, in this module's memory. */
EM_JS_DEPS(bridge, "$stringToNewUTF8,$UTF8ToString");

EM_JS(int, js_attach, (), {
    const web = globalThis.rantWeb;
    if (!web) return 0;
    const text = (s) => stringToNewUTF8(s ?? "");
    const bytes = (b) => {
        const p = _malloc(Math.max(1, b.length));
        HEAPU8.set(b, p);
        return p;
    };
    const with_text = (s, f) => { const p = text(s); try { f(p); } finally { _free(p); } };
    const with_bytes = (b, f) => { const p = bytes(b); try { f(p); } finally { _free(p); } };
    web.sink = {
        peers: (json) => with_text(json, (p) => _bridge_peers(p)),
        entities: (json) => with_text(json, (p) => _bridge_entities(p)),
        ready: (id, json) => with_text(json, (p) => _bridge_ready(id, p)),
        failed: (id, why) => with_text(why, (p) => _bridge_failed(id, p)),
        message: (id, data, peer, recvUs) =>
            with_bytes(data, (p) => _bridge_message(id, p, data.length, peer >>> 0, recvUs)),
        result: (call, status, data, message) => with_bytes(data, (p) => with_text(status, (s) =>
            with_text(message, (m) => _bridge_result(call, s, p, data.length, m)))),
        progress: (call, data, recvUs) =>
            with_bytes(data, (p) => _bridge_progress(call, p, data.length, recvUs)),
        meta: (req, json) => with_text(json, (p) => _bridge_meta(req, p)),
        log: (level, node, wallUs, line) =>
            with_text(node, (n) => with_text(line, (t) => _bridge_log(level, n, wallUs, t))),
        event: (line) => with_text(line, (p) => _bridge_event(p)),
    };
    web.attached();
    return 1;
});

EM_JS(void, js_open, (int id, const char* kind, const char* name, const char* spec), {
    globalThis.rantWeb.open(id, UTF8ToString(kind), UTF8ToString(name), JSON.parse(UTF8ToString(spec)));
});
EM_JS(void, js_close, (int id), { globalThis.rantWeb.close(id); });
EM_JS(void, js_refresh, (int id), { globalThis.rantWeb.refresh(id); });
EM_JS(int, js_matches, (int id), { return globalThis.rantWeb.matches(id); });
EM_JS(void, js_send, (int id, const uint8_t* data, int size), {
    globalThis.rantWeb.send(id, HEAPU8.slice(data, data + size));
});
EM_JS(void, js_call, (int id, int call, const uint8_t* data, int size), {
    globalThis.rantWeb.call(id, call, HEAPU8.slice(data, data + size));
});
EM_JS(void, js_cancel, (int call), { globalThis.rantWeb.cancel(call); });
EM_JS(void, js_meta, (int req, int peer, int sections), {
    globalThis.rantWeb.meta(req, peer >>> 0, sections >>> 0);
});

namespace bridge {
namespace {

using RawSchema = rant::detail::RantSchema;

/* One handle's entity and what the bridge said of it. */
struct Entry {
    std::string    name;
    bool           variable = false;
    MessageHandler handler;
    rant::Schema   value, req, rsp, prg;   /* from the create reply, empty while untyped */
    int            matches = 0;
};

struct Pending {
    int             handle = 0;
    ProgressHandler progress;
    ResponseHandler done;
};

struct State {
    bool                                           attached = false;
    std::string                                    error;
    std::map<int, Entry>                           handles;
    int                                            next_handle = 0;
    std::map<uint32_t, Pending>                    calls;
    uint32_t                                       next_call = 0;
    std::map<int, std::function<void(const rant::MetaSnapshot&)>> metas;
    int                                            next_meta = 0;
    std::vector<rant::Peer>                        peers;
    std::map<uint32_t, std::vector<rant::Entity>>  entities;   /* by peer id */
    std::vector<rant::Entity>                      mesh;
    uint32_t                                       epoch = 0;
    std::map<std::string, const RawSchema*>        schemas;    /* by schema text */
    std::function<void(const Event&)>              on_event;
    std::function<void(const rant::LogLine&)>      on_log;
};

State& state()
{
    static State s;
    return s;
}

/* ---- reading what the page sends: JSON through the clipboard's reader ---- */

const Pasted* member(const Pasted& object, const char* key)
{
    if (object.kind != Pasted::Object) return nullptr;
    for (size_t i = 0; i < object.keys.size(); i++)
        if (object.keys[i] == key) return &object.items[i];
    return nullptr;
}

std::string text_of(const Pasted& object, const char* key)
{
    const Pasted* p = member(object, key);
    return p && p->kind == Pasted::Text ? p->text : std::string();
}

double number_of(const Pasted& object, const char* key)
{
    const Pasted* p = member(object, key);
    return p && p->kind == Pasted::Number ? std::strtod(p->text.c_str(), nullptr) : 0;
}

uint64_t whole_of(const Pasted& object, const char* key)
{
    const Pasted* p = member(object, key);
    return p && p->kind == Pasted::Number ? std::strtoull(p->text.c_str(), nullptr, 10) : 0;
}

bool flag_of(const Pasted& object, const char* key)
{
    const Pasted* p = member(object, key);
    return p && p->kind == Pasted::Bool && p->text == "true";
}

/* A JSON value as Rant's map tree, for a meta reply's info. */
rant::MapItem map_item(const Pasted& p)
{
    rant::MapItem m;
    switch (p.kind) {
    case Pasted::Bool:   m.value = p.text == "true"; break;
    case Pasted::Text:   m.value = p.text; break;
    case Pasted::Number:
        if (p.text.find_first_of(".eE") != std::string::npos) m.value = std::strtod(p.text.c_str(), nullptr);
        else if (p.text[0] == '-') m.value = (int64_t)std::strtoll(p.text.c_str(), nullptr, 10);
        else m.value = (uint64_t)std::strtoull(p.text.c_str(), nullptr, 10);
        break;
    case Pasted::List: {
        rant::MapList list;
        for (const Pasted& item : p.items) list.push_back(map_item(item));
        m.value = std::move(list);
        break;
    }
    case Pasted::Object: {
        rant::MapDict dict;
        for (size_t i = 0; i < p.keys.size(); i++) dict.emplace(p.keys[i], map_item(p.items[i]));
        m.value = std::move(dict);
        break;
    }
    default: break;
    }
    return m;
}

/* A schema block's text compiled once and kept. A text Rant refuses reads as bytes. */
rant::Schema schema_of(const Pasted* block)
{
    if (!block || block->kind != Pasted::Object) return {};
    const std::string dsl = text_of(*block, "dsl");
    if (dsl.empty()) return {};
    State& s = state();
    auto it = s.schemas.find(dsl);
    if (it == s.schemas.end()) {
        const char* why = nullptr;
        const RawSchema* raw = rant_web_schema_compile(dsl.c_str(), &why);
        if (!raw) std::fprintf(stderr, "bridge: a schema did not compile: %s\n", why ? why : "?");
        it = s.schemas.emplace(dsl, raw).first;
    }
    return rant::Schema(it->second);
}

rant::EntityKind entity_kind(const std::string& word)
{
    if (word == "function") return rant::EntityKind::Function;
    if (word == "variable") return rant::EntityKind::Variable;
    if (word == "task") return rant::EntityKind::Task;
    return rant::EntityKind::Topic;
}

rant::Entity entity_of(const Pasted& e)
{
    rant::Entity out;
    out.kind        = entity_kind(text_of(e, "kind"));
    out.name        = text_of(e, "name");
    out.provides    = flag_of(e, "provides");
    out.consumes    = flag_of(e, "consumes");
    out.reliable    = flag_of(e, "reliable");
    out.writable    = flag_of(e, "writable");
    out.forceable   = flag_of(e, "forceable");
    out.cancellable = flag_of(e, "cancellable");
    out.exclusive   = flag_of(e, "exclusive");
    out.multi       = flag_of(e, "multi");
    out.incomplete  = flag_of(e, "incomplete");
    out.conflict    = flag_of(e, "conflict");
    out.providers   = (uint16_t)whole_of(e, "providers");
    out.consumers   = (uint16_t)whole_of(e, "consumers");
    out.provider    = (uint32_t)whole_of(e, "provider");
    out.from        = text_of(e, "from");
    out.generation  = std::strtoull(text_of(e, "generation").c_str(), nullptr, 16);
    out.schema          = schema_of(member(e, "schema"));
    out.rsp_schema      = schema_of(member(e, "rsp"));
    out.progress_schema = schema_of(member(e, "prg"));
    return out;
}

std::vector<rant::Entity> entities_of(const Pasted* list)
{
    std::vector<rant::Entity> out;
    if (list && list->kind == Pasted::List)
        for (const Pasted& e : list->items) out.push_back(entity_of(e));
    return out;
}

rant::CallStatus call_status(const std::string& word)
{
    if (word == "ok") return rant::CallStatus::Ok;
    if (word == "app_error") return rant::CallStatus::AppError;
    if (word == "no_handler") return rant::CallStatus::NoHandler;
    if (word == "timeout") return rant::CallStatus::Timeout;
    if (word == "peer_lost") return rant::CallStatus::PeerLost;
    if (word == "no_provider") return rant::CallStatus::NoProvider;
    return rant::CallStatus::Cancelled;
}

/* The node and proc numbers Rant pulls out of a meta body, read the same way. */
void meta_numbers(rant::MetaSnapshot& snap)
{
    auto section = [&snap](const char* name) -> const rant::MapDict* {
        const auto it = snap.info.find(name);
        return it != snap.info.end() && it->second.is_map() ? &it->second.as_map() : nullptr;
    };
    auto u = [](const rant::MapDict& m, const char* k) {
        const auto j = m.find(k);
        return j == m.end() ? uint64_t(0) : j->second.as_uint();
    };
    if (const rant::MapDict* m = section("node")) {
        const auto name = m->find("name");
        if (name != m->end()) snap.node.name = name->second.as_string();
        const auto why = m->find("last_error_text");
        if (why != m->end()) snap.node.last_error_text = why->second.as_string();
        snap.node.uptime_us      = u(*m, "uptime_us");
        snap.node.wall_us        = u(*m, "wall_us");
        snap.node.mem_in_use     = u(*m, "mem_in_use");
        snap.node.mem_peak       = u(*m, "mem_peak");
        snap.node.alloc_calls    = u(*m, "alloc_calls");
        snap.node.evicted_unsent = u(*m, "evicted_unsent");
        snap.node.bp_waited_us   = u(*m, "bp_waited_us");
        snap.node.bp_waits       = u(*m, "bp_waits");
        snap.node.peers          = u(*m, "peers");
        snap.node.max_peers      = u(*m, "max_peers");
        snap.node.topics         = u(*m, "topics");
        snap.node.max_topics     = u(*m, "max_topics");
        snap.node.shm_tx         = u(*m, "shm_tx");
        snap.node.shm_rx         = u(*m, "shm_rx");
        snap.node.last_error     = u(*m, "last_error");
    }
    if (const rant::MapDict* m = section("proc")) {
        snap.proc.have          = true;
        snap.proc.have_cpu      = m->find("cpu_us") != m->end();
        snap.proc.pid           = u(*m, "pid");
        snap.proc.cpu_us        = u(*m, "cpu_us");
        snap.proc.rss           = u(*m, "rss");
        snap.proc.peak_rss      = u(*m, "peak_rss");
        snap.proc.heap_total    = u(*m, "heap_total");
        snap.proc.heap_free     = u(*m, "heap_free");
        snap.proc.heap_min_free = u(*m, "heap_min_free");
        snap.proc.heap_largest_free_block = u(*m, "heap_largest_free_block");
    }
}

/* The schema a value on this handle was written in: its writer's own when the mesh says,
   since Rant hands over the writer's bytes, else the handle's, else the mesh's. */
const RawSchema* writer_schema(const Entry& e, uint32_t peer)
{
    const State& s = state();
    const rant::EntityKind kind = e.variable ? rant::EntityKind::Variable : rant::EntityKind::Topic;
    if (!e.variable && peer) {
        const auto found = s.entities.find(peer);
        if (found != s.entities.end())
            for (const rant::Entity& x : found->second)
                if (x.kind == kind && x.name == e.name && x.provides && x.schema) return x.schema.raw();
    }
    if (e.value) return e.value.raw();
    for (const rant::Entity& x : s.mesh)
        if (x.kind == kind && x.name == e.name && x.schema) return x.schema.raw();
    return nullptr;
}

std::string json_text(const std::string& text)
{
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') out += '\\';
        if ((unsigned char)c < 0x20) {
            char esc[8];
            std::snprintf(esc, sizeof esc, "\\u%04x", c);
            out += esc;
            continue;
        }
        out += c;
    }
    return out + "\"";
}

int open_entity(const char* kind, const std::string& name, const std::string& spec, bool variable,
                MessageHandler handler)
{
    State& s = state();
    if (!s.attached) return 0;
    const int id = ++s.next_handle;
    Entry& e   = s.handles[id];
    e.name     = name;
    e.variable = variable;
    e.handler  = std::move(handler);
    js_open(id, kind, name.c_str(), spec.c_str());
    return id;
}

} /* namespace */

/* ---- handles ---- */

int Handle::match_count() const
{
    return id_ ? js_matches(id_) : 0;
}

rant::SendStatus Handle::close()
{
    if (!id_) return rant::SendStatus::NoTopic;
    state().handles.erase(id_);
    js_close(id_);
    id_ = 0;
    return rant::SendStatus::Ok;
}

bool Handle::refresh()
{
    if (id_) js_refresh(id_);
    return id_ != 0;
}

rant::SendStatus send(int id, rant::Bytes value)
{
    if (!state().handles.count(id)) return rant::SendStatus::NoTopic;
    js_send(id, value.data(), (int)value.size());
    return rant::SendStatus::Ok;
}

void listen(int id, MessageHandler handler)
{
    const auto it = state().handles.find(id);
    if (it != state().handles.end()) it->second.handler = std::move(handler);
}

rant::SendStatus call(int id, rant::Bytes request, ProgressHandler progress, ResponseHandler done, uint32_t* call_id)
{
    State& s = state();
    const auto it = s.handles.find(id);
    if (it == s.handles.end()) return rant::SendStatus::NoTopic;
    if (!it->second.req) return rant::SendStatus::Schema;   /* the bridge has not typed it yet */
    const uint32_t c = ++s.next_call;
    s.calls[c] = Pending{ id, std::move(progress), std::move(done) };
    if (call_id) *call_id = c;
    js_call(id, (int)c, request.data(), (int)request.size());
    return rant::SendStatus::Ok;
}

rant::SendStatus cancel(int id, uint32_t call)
{
    const auto it = state().calls.find(call);
    if (it == state().calls.end() || it->second.handle != id) return rant::SendStatus::State;
    js_cancel((int)call);
    return rant::SendStatus::Ok;
}

/* ---- the mesh ---- */

std::vector<rant::Peer> Reflection::peers() const { return state().peers; }

uint32_t Reflection::epoch() const { return state().epoch; }

std::vector<rant::Entity> Reflection::entities(uint32_t peer) const
{
    const auto it = state().entities.find(peer);
    return it == state().entities.end() ? std::vector<rant::Entity>() : it->second;
}

std::vector<rant::Entity> Reflection::mesh() const { return state().mesh; }

rant::SendStatus Reflection::meta_async(uint32_t peer, std::function<void(const rant::MetaSnapshot&)> done,
                                        uint32_t sections) const
{
    State& s = state();
    if (!s.attached) return rant::SendStatus::NoTopic;
    const int req = ++s.next_meta;
    s.metas[req] = std::move(done);
    js_meta(req, (int)peer, (int)sections);
    return rant::SendStatus::Ok;
}

/* ---- the node ---- */

Node::Node(const std::string&, const rant::NodeOptions&)
{
    State& s = state();
    s.attached = js_attach() != 0;
    if (!s.attached) s.error = "the page holds no bridge connection";
}

bool Node::valid() const { return state().attached; }

std::string Node::last_error() const { return state().error; }

void Node::on_event(std::function<void(const Event&)> handler) { state().on_event = std::move(handler); }

bool Node::on_log(std::function<void(const rant::LogLine&)> handler)
{
    state().on_log = std::move(handler);
    return true;
}

rant::Schema Node::schema_from_wire(rant::Bytes wire) const
{
    return rant::Schema(rant_web_schema_parse(wire.data(), wire.size()));
}

/* An untyped handle hears each value in its writer's schema. A typed one asks the bridge to
   keep values as wire bytes, since the capture encodes and decodes with Rant itself. */
int Node::open_topic(const char* kind, const std::string& name, const rant::Qos& qos,
                     const rant::Schema& schema, MessageHandler handler)
{
    std::string spec = "{\"reliable\":";
    spec += qos.reliability == rant::Reliability::Reliable ? "true" : "false";
    if (qos.catch_up) spec += ",\"catchUp\":" + std::to_string(qos.catch_up);
    if (schema) spec += ",\"schema\":" + json_text(schema.to_dsl()) + ",\"raw\":true";
    spec += "}";
    return open_entity(kind, name, spec, false, std::move(handler));
}

int Node::open_variable(const std::string& name, bool read_only, bool reflect)
{
    std::string spec = "{\"readOnly\":";
    spec += read_only ? "true" : "false";
    if (reflect) spec += ",\"reflect\":true,\"raw\":true";
    spec += "}";
    return open_entity("remote_variable", name, spec, true, nullptr);
}

int Node::open_call(const char* kind, const std::string& name)
{
    return open_entity(kind, name, "{\"reflect\":true,\"raw\":true}", false, nullptr);
}

} /* namespace bridge */

/* ---- what the page calls ---- */

using bridge::state;

extern "C" {

/* Every peer with its link, a few times a second. */
EMSCRIPTEN_KEEPALIVE void bridge_peers(const char* json)
{
    const Pasted list = read_pasted(json);
    std::vector<rant::Peer> peers;
    for (const Pasted& p : list.items) {
        rant::Peer peer;
        peer.id            = (uint32_t)bridge::whole_of(p, "id");
        peer.name          = bridge::text_of(p, "name");
        peer.address       = bridge::text_of(p, "address");
        peer.active        = bridge::flag_of(p, "active");
        peer.last_heard_us = bridge::whole_of(p, "lastHeardUs");
        peer.catching_up   = bridge::flag_of(p, "catchingUp");
        peer.fragment_size = (uint16_t)bridge::whole_of(p, "fragmentSize");
        peer.rtt_min_us    = (uint32_t)bridge::whole_of(p, "rttMinUs");
        /* without the peer's own epoch, the mesh's stands in, so its lists are read again */
        const Pasted* epoch = bridge::member(p, "epoch");
        peer.epoch = epoch ? (uint32_t)bridge::whole_of(p, "epoch") : state().epoch;
        if (const Pasted* rtt = bridge::member(p, "rtt")) {
            peer.rtt_us        = (uint32_t)bridge::whole_of(*rtt, "us");
            peer.rtt_jitter_us = (uint32_t)bridge::whole_of(*rtt, "jitterUs");
            peer.rtt_samples   = (uint32_t)bridge::whole_of(*rtt, "samples");
        }
        /* a uuid when the bridge gives one, else the id, which holds while the peer does */
        const std::string uuid = bridge::text_of(p, "uuid");
        for (size_t i = 0; i < 16; i++)
            peer.uuid[i] = uuid.size() == 32 ? (uint8_t)std::strtoul(uuid.substr(i * 2, 2).c_str(), nullptr, 16)
                         : i < 4 ? (uint8_t)(peer.id >> (24 - 8 * i)) : 0;
        peers.push_back(std::move(peer));
    }
    state().peers.swap(peers);
}

/* The whole mesh and each peer's own entities, whenever the bridge's epoch moved. */
EMSCRIPTEN_KEEPALIVE void bridge_entities(const char* json)
{
    const Pasted all = read_pasted(json);
    bridge::State& s = state();
    s.mesh = bridge::entities_of(bridge::member(all, "mesh"));
    s.entities.clear();
    if (const Pasted* by_peer = bridge::member(all, "peers"))
        for (size_t i = 0; i < by_peer->keys.size(); i++)
            s.entities[(uint32_t)std::strtoul(by_peer->keys[i].c_str(), nullptr, 10)] =
                bridge::entities_of(&by_peer->items[i]);
    s.epoch++;
}

/* A create or refresh reply: the schemas the handle took. */
EMSCRIPTEN_KEEPALIVE void bridge_ready(int id, const char* json)
{
    const auto it = state().handles.find(id);
    if (it == state().handles.end()) return;
    const Pasted reply = read_pasted(json);
    bridge::Entry& e = it->second;
    e.value = bridge::schema_of(bridge::member(reply, "schema"));
    e.req   = bridge::schema_of(bridge::member(reply, "req"));
    e.rsp   = bridge::schema_of(bridge::member(reply, "rsp"));
    e.prg   = bridge::schema_of(bridge::member(reply, "prg"));
}

EMSCRIPTEN_KEEPALIVE void bridge_failed(int id, const char* why)
{
    std::fprintf(stderr, "bridge: handle %d failed: %s\n", id, why);
    state().error = why;
}

EMSCRIPTEN_KEEPALIVE void bridge_message(int id, const uint8_t* data, int size, uint32_t peer, double recv_us)
{
    const auto it = state().handles.find(id);
    if (it == state().handles.end() || !it->second.handler) return;
    const bridge::Entry& e = it->second;
    e.handler(bridge::MessageView(rant::Bytes(data, (size_t)size), bridge::writer_schema(e, peer), (uint64_t)recv_us));
}

EMSCRIPTEN_KEEPALIVE void bridge_result(uint32_t call, const char* status, const uint8_t* data, int size,
                                        const char* message)
{
    bridge::State& s = state();
    const auto it = s.calls.find(call);
    if (it == s.calls.end()) return;
    const bridge::Pending pending = std::move(it->second);
    s.calls.erase(it);
    const auto h = s.handles.find(pending.handle);
    const rant::detail::RantSchema* schema = h == s.handles.end() ? nullptr : h->second.rsp.raw();
    if (pending.done)
        pending.done(bridge::ResponseView<rant::Bytes>(bridge::call_status(status), message,
                     bridge::MessageView(rant::Bytes(data, (size_t)size), schema, 0)));
}

EMSCRIPTEN_KEEPALIVE void bridge_progress(uint32_t call, const uint8_t* data, int size, double recv_us)
{
    bridge::State& s = state();
    const auto it = s.calls.find(call);
    if (it == s.calls.end() || !it->second.progress) return;
    const auto h = s.handles.find(it->second.handle);
    const rant::detail::RantSchema* schema = h == s.handles.end() ? nullptr : h->second.prg.raw();
    it->second.progress(bridge::ProgressView<rant::Bytes>(
        bridge::MessageView(rant::Bytes(data, (size_t)size), schema, (uint64_t)recv_us)));
}

EMSCRIPTEN_KEEPALIVE void bridge_meta(int req, const char* json)
{
    bridge::State& s = state();
    const auto it = s.metas.find(req);
    if (it == s.metas.end()) return;
    const auto done = std::move(it->second);
    s.metas.erase(it);
    const Pasted reply = read_pasted(json);
    rant::MetaSnapshot snap;
    snap.valid    = bridge::flag_of(reply, "valid");
    snap.status   = bridge::call_status(bridge::text_of(reply, "status"));
    snap.provider = (uint32_t)bridge::whole_of(reply, "provider");
    if (const Pasted* info = bridge::member(reply, "info")) snap.info = bridge::map_item(*info).as_map();
    bridge::meta_numbers(snap);
    if (done) done(snap);
}

EMSCRIPTEN_KEEPALIVE void bridge_log(int level, const char* node, double wall_us, const char* text)
{
    if (!state().on_log) return;
    rant::LogLine line;
    line.level   = (rant::LogLevel)level;
    line.node    = node;
    line.wall_us = (uint64_t)wall_us;
    line.text    = text;
    state().on_log(line);
}

EMSCRIPTEN_KEEPALIVE void bridge_event(const char* text)
{
    if (state().on_event) state().on_event(bridge::Event(text));
}

} /* extern "C" */
