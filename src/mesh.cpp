#include "mesh.hpp"

#include "canvas.hpp"
#include "format.hpp"
#include "tree.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Core/ElementInstancer.h>
#include <RmlUi/Core/Factory.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <tuple>

namespace {

using Kind = Capture::Kind;

Capture* source = nullptr;

/* Sizes in dp at zoom 1. A box is as wide as its name, and a link rests this much longer
   than the two half boxes it joins. An arrow bows this far off the straight line when the
   arrow back is shown too, so the two never overlap. */
constexpr float BOX_H = 26, BOX_PAD = 10, BOX_DOT = 13, NAME_DP = 12;
constexpr float LINK_REST = 90, BOW = 16;
constexpr float ZOOM_MIN = 0.2f, ZOOM_MAX = 2.5f, FIT_MAX = 1.2f;

/* The layout cools by this share each tick and stops below COLD. */
constexpr float COOLING = 0.0228f, COLD = 0.002f;

struct Body {
    Rml::Vector2f p{ 0, 0 }, v{ 0, 0 };
    float w = 0;            /* the box width in dp, 0 until measured */
    bool  placed = false;
    bool  pinned = false;   /* placed by hand, so the layout leaves it */
};

/* Every link from one node to another, drawn as one arrow. */
struct Edge {
    int              a = 0, b = 0;   /* mesh node indices, the provider first */
    std::vector<int> links;          /* mesh link indices, busiest first */
    double           hz = -1;
    bool             active = false;
    bool             lost = false;   /* one of its names is sent and not received */
    bool             twin = false;   /* the arrow back is shown too */
    bool             mixed = false;  /* its names are of more than one kind */
    Kind             kind = Kind::Topic;
};

struct State {
    MeshFilter filter;
    MeshPick   pick;
    std::map<std::string, Body> bodies;   /* by key */

    uint32_t                 epoch = ~0u;
    bool                     dirty = true;
    std::vector<std::string> keys;    /* per mesh node */
    std::vector<int>         shown;   /* mesh node indices */
    std::vector<int>         joined;  /* the shown with an arrow, which the layout moves */
    std::vector<int>         loose;   /* the shown without, parked in rows under the rest */
    std::vector<char>        lit;     /* per mesh node: near the selection, or all while none */
    std::vector<Edge>        edges;
    std::string              shape;   /* what is shown, so a change warms the layout */
    int                      links = 0;

    float         heat = 1;
    Rml::Vector2f centre{ 0, 0 };
    float         zoom = 1;
    bool          fit = true;
};
State st;

int index_of(const std::string& key)
{
    if (key.empty()) return -1;
    const auto it = std::find(st.keys.begin(), st.keys.end(), key);
    return it == st.keys.end() ? -1 : (int)(it - st.keys.begin());
}

bool kind_kept(unsigned cats, Kind kind)
{
    if (!(cats & MESH_KINDS)) return true;
    const unsigned bit = kind == Kind::Function ? MESH_FUNCTION : kind == Kind::Variable ? MESH_VARIABLE
                       : kind == Kind::Task     ? MESH_TASK     : MESH_TOPIC;
    return (cats & bit) != 0;
}

/* Where a node joining the layout starts: beside its placed neighbours, else on a ring
   round the middle, spread by its key so no two start on one spot. */
Rml::Vector2f start_at(const std::string& key, const std::vector<Rml::Vector2f>& near)
{
    const size_t  h     = std::hash<std::string>()(key);
    const float   angle = (float)(h % 6283) / 1000.f;
    const Rml::Vector2f out(std::cos(angle), std::sin(angle));
    if (near.empty()) return out * (160.f + (float)(h / 6283 % 120));
    Rml::Vector2f mid(0, 0);
    for (const Rml::Vector2f& p : near) mid += p;
    return mid / (float)near.size() + out * 70.f;
}

/* The shown graph from the mesh, the filter and the selection. */
void rebuild()
{
    const Capture::MeshView& mesh = source->mesh();
    const MeshFilter&        f    = st.filter;
    const size_t             n    = mesh.nodes.size();
    st.epoch = source->mesh_epoch();
    st.dirty = false;

    /* A node's key is its name and host, which outlive the uuid a restart changes. */
    st.keys.clear();
    std::map<std::string, int> taken;
    for (const Capture::MeshNode& node : mesh.nodes) {
        std::string key = node.name + "@" + node.host;
        const int   k   = ++taken[key];
        if (k > 1) key += "#" + std::to_string(k);
        st.keys.push_back(key);
    }
    for (auto it = st.bodies.begin(); it != st.bodies.end();)
        it = index_of(it->first) >= 0 ? std::next(it) : st.bodies.erase(it);
    for (std::string* key : { &st.pick.node, &st.pick.from, &st.pick.to })
        if (!key->empty() && index_of(*key) < 0) st.pick = MeshPick();

    auto hit = [&f](const std::string& s) { return contains_ci(s, f.text); };
    std::vector<char>          kept(mesh.links.size(), 0);
    std::vector<std::set<int>> around(n);
    for (size_t i = 0; i < mesh.links.size(); i++) {
        const Capture::MeshLink& l = mesh.links[i];
        if (!kind_kept(f.cats, l.kind) || ((f.cats & MESH_ACTIVE) && !l.active)) continue;
        if ((f.cats & MESH_NO_SYSTEM) && is_system(l.name)) continue;
        if (!f.text.empty() && !hit(l.name) && !hit(mesh.nodes[l.from].name) && !hit(mesh.nodes[l.to].name)) continue;
        kept[i] = 1;
        around[l.from].insert(l.to);
        around[l.to].insert(l.from);
    }
    std::vector<char> on(n, 0);
    for (size_t i = 0; i < n; i++)
        on[i] = (f.text.empty() || hit(mesh.nodes[i].name) || !around[i].empty()) &&
                !((f.cats & MESH_CONNECTED) && around[i].empty()) &&
                !((f.cats & MESH_NO_LEAVES) && around[i].size() == 1);

    /* The selection lights what is within the hops of a node, or the two ends of a pair. */
    st.lit.assign(n, 1);
    const int node = index_of(st.pick.node), from = index_of(st.pick.from), to = index_of(st.pick.to);
    bool selecting = false;
    if (node >= 0 && on[node]) {
        selecting = true;
        st.lit.assign(n, 0);
        st.lit[node] = 1;
        std::vector<int> frontier{ node };
        for (int hop = 0; !frontier.empty() && (f.hops <= 0 || hop < f.hops); hop++) {
            std::vector<int> next;
            for (const int u : frontier)
                for (const int w : around[u])
                    if (on[w] && !st.lit[w]) {
                        st.lit[w] = 1;
                        next.push_back(w);
                    }
            frontier.swap(next);
        }
    } else if (from >= 0 && to >= 0 && on[from] && on[to]) {
        selecting = true;
        st.lit.assign(n, 0);
        st.lit[from] = st.lit[to] = 1;
    }
    if (selecting && f.focus)
        for (size_t i = 0; i < n; i++) on[i] = on[i] && st.lit[i];

    std::map<std::pair<int, int>, Edge> pairs;
    for (size_t i = 0; i < mesh.links.size(); i++) {
        const Capture::MeshLink& l = mesh.links[i];
        if (!kept[i] || !on[l.from] || !on[l.to]) continue;
        Edge& e = pairs[{ l.from, l.to }];
        if (e.links.empty()) e.kind = l.kind;
        e.mixed = e.mixed || e.kind != l.kind;
        e.a = l.from;
        e.b = l.to;
        e.links.push_back((int)i);
        if (l.hz >= 0) e.hz = std::max(e.hz, 0.0) + l.hz;
        e.active = e.active || l.active;
        e.lost   = e.lost || l.lost;
    }
    st.edges.clear();
    st.links = 0;
    for (auto& entry : pairs) {
        Edge& e = entry.second;
        e.twin  = pairs.count({ e.b, e.a }) > 0;
        std::sort(e.links.begin(), e.links.end(), [&mesh](int x, int y) { return mesh.links[x].hz > mesh.links[y].hz; });
        st.links += (int)e.links.size();
        st.edges.push_back(std::move(e));
    }

    st.shown.clear();
    std::string shape;
    for (size_t i = 0; i < n; i++)
        if (on[i]) {
            st.shown.push_back((int)i);
            shape += st.keys[i] + ",";
        }
    shape += "|";
    for (const Edge& e : st.edges) shape += st.keys[e.a] + ">" + st.keys[e.b] + ",";
    if (shape != st.shape) {
        st.shape = shape;
        st.heat  = std::max(st.heat, 0.5f);
    }

    std::vector<char> linked(n, 0);
    for (const Edge& e : st.edges) linked[e.a] = linked[e.b] = 1;
    st.joined.clear();
    st.loose.clear();
    for (const int i : st.shown) (linked[i] ? st.joined : st.loose).push_back(i);
    std::sort(st.loose.begin(), st.loose.end(), [&mesh](int x, int y) { return mesh.nodes[x].name < mesh.nodes[y].name; });

    for (const int i : st.shown) {
        Body& body = st.bodies[st.keys[i]];
        if (body.placed) continue;
        std::vector<Rml::Vector2f> near;
        for (const int w : around[i]) {
            const auto other = st.bodies.find(st.keys[w]);
            if (other != st.bodies.end() && other->second.placed) near.push_back(other->second.p);
        }
        body.p      = start_at(st.keys[i], near);
        body.placed = true;
    }
}

void refresh()
{
    if (source && (st.dirty || st.epoch != source->mesh_epoch())) rebuild();
}

Body& body_of(int node)
{
    return st.bodies[st.keys[node]];
}

/* One tick of the layout: nodes push apart, links pull to their rest length, an arrow
   leans its consumer to the right of its provider so the graph reads as a flow, and
   everything drifts to the middle. Then no two boxes overlap. */
void tick()
{
    const float heat  = st.heat;
    const int   count = (int)st.joined.size();
    std::vector<Body*> b(count);
    std::map<int, int> slot;
    for (int i = 0; i < count; i++) {
        b[i] = &body_of(st.joined[i]);
        slot[st.joined[i]] = i;
    }

    for (int i = 0; i < count; i++)
        for (int j = i + 1; j < count; j++) {
            Rml::Vector2f d = b[j]->p - b[i]->p;
            float l2 = d.x * d.x + d.y * d.y;
            if (l2 < 1) {
                d  = Rml::Vector2f((float)(j - i), 1);
                l2 = d.x * d.x + d.y * d.y;
            }
            if (l2 > 500.f * 500.f) continue;   /* far apart, they leave each other be */
            const float k = 700.f * heat / std::max(l2, 400.f);
            b[i]->v -= d * k;
            b[j]->v += d * k;
        }

    /* Each pair of neighbours is one spring, however many arrows join them. */
    std::set<std::pair<int, int>> springs;
    std::vector<int> degree(count, 0);
    for (const Edge& e : st.edges) {
        const int a = slot[e.a], c = slot[e.b];
        if (springs.insert({ std::min(a, c), std::max(a, c) }).second) {
            degree[a]++;
            degree[c]++;
        }
    }
    for (const auto& s : springs) {
        Body& a = *b[s.first];
        Body& c = *b[s.second];
        const Rml::Vector2f d = (c.p + c.v) - (a.p + a.v);
        const float len  = std::max(1.f, std::sqrt(d.x * d.x + d.y * d.y));
        const float rest = LINK_REST + (a.w + c.w) / 4 + BOX_H;
        const float k    = (len - rest) / len * heat * 0.7f / (float)std::min(degree[s.first], degree[s.second]);
        const float bias = (float)degree[s.first] / (float)(degree[s.first] + degree[s.second]);
        c.v -= d * k * bias;
        a.v += d * k * (1 - bias);
    }
    for (const Edge& e : st.edges) {
        if (e.twin) continue;
        Body& a = *b[slot[e.a]];
        Body& c = *b[slot[e.b]];
        const float want = LINK_REST * 0.6f + (a.w + c.w) / 2, dx = c.p.x - a.p.x;
        if (dx >= want) continue;
        const float push = (want - dx) * 0.03f * heat;
        c.v.x += push;
        a.v.x -= push;
    }

    for (Body* body : b) {
        if (body->pinned) {
            body->v = Rml::Vector2f(0, 0);
            continue;
        }
        body->v -= body->p * (0.02f * heat);
        body->v *= 0.6f;
        body->p += body->v;
    }

    for (int i = 0; i < count; i++)
        for (int j = i + 1; j < count; j++) {
            Body& p = *b[i];
            Body& q = *b[j];
            const Rml::Vector2f d = q.p - p.p;
            const float ox = (p.w + q.w) / 2 + 16 - std::fabs(d.x);
            const float oy = BOX_H + 14 - std::fabs(d.y);
            if (ox <= 0 || oy <= 0 || (p.pinned && q.pinned)) continue;
            const float share_p = p.pinned ? 0.f : q.pinned ? 1.f : 0.5f;
            Rml::Vector2f move = ox < oy ? Rml::Vector2f(d.x < 0 ? -ox : ox, 0) : Rml::Vector2f(0, d.y < 0 ? -oy : oy);
            move *= 0.5f;
            p.p -= move * share_p;
            q.p += move * (1 - share_p);
        }

    st.heat -= st.heat * COOLING;
    if (st.heat < COLD) st.heat = 0;
}

/* The nodes with no arrow sit in rows under the others, by name, easing into place. One
   the user dragged stays where it was put. */
void park()
{
    float x0 = 0, x1 = 0, bottom = 0;
    bool  any = false;
    for (const int i : st.joined) {
        const Body& b = body_of(i);
        x0 = any ? std::min(x0, b.p.x - b.w / 2) : b.p.x - b.w / 2;
        x1 = any ? std::max(x1, b.p.x + b.w / 2) : b.p.x + b.w / 2;
        bottom = any ? std::max(bottom, b.p.y + BOX_H / 2) : b.p.y + BOX_H / 2;
        any = true;
    }
    const float gap = 14, width = std::max(x1 - x0, 480.f), middle = (x0 + x1) / 2;
    float y = any ? bottom + 60 : 0;
    for (size_t first = 0; first < st.loose.size();) {
        size_t last = first;
        float  run  = 0;
        while (last < st.loose.size()) {
            const float w = body_of(st.loose[last]).w + (last > first ? gap : 0);
            if (last > first && run + w > width) break;
            run += w;
            last++;
        }
        float x = middle - run / 2;
        for (size_t k = first; k < last; k++) {
            Body& b = body_of(st.loose[k]);
            const Rml::Vector2f target(x + b.w / 2, y);
            x += b.w + gap;
            if (!b.pinned) b.p += (target - b.p) * 0.25f;
        }
        y += BOX_H + 12;
        first = last;
    }
}

/* The world, in dp at zoom 1, onto the element's pixels. */
struct View {
    Rml::Vector2f half, centre;
    float         s = 1;   /* pixels per world dp */

    Rml::Vector2f screen(Rml::Vector2f p) const { return half + (p - centre) * s; }
    Rml::Vector2f world(Rml::Vector2f q) const { return centre + (q - half) / s; }
};

/* Where a segment from inside a box to outside it crosses the box's edge. */
Rml::Vector2f leave(Rml::Vector2f in, Rml::Vector2f out, Rml::Vector2f c, float hw, float hh)
{
    float t = 1;
    const Rml::Vector2f d = out - in;
    if (d.x != 0) {
        const float tx = ((d.x > 0 ? c.x + hw : c.x - hw) - in.x) / d.x;
        if (tx >= 0) t = std::min(t, tx);
    }
    if (d.y != 0) {
        const float ty = ((d.y > 0 ? c.y + hh : c.y - hh) - in.y) / d.y;
        if (ty >= 0) t = std::min(t, ty);
    }
    return in + d * t;
}

bool inside(Rml::Vector2f p, Rml::Vector2f c, float hw, float hh)
{
    return std::fabs(p.x - c.x) <= hw && std::fabs(p.y - c.y) <= hh;
}

/* An arrow on screen from the edge of one box to the edge of the other. Empty when the
   boxes overlap. */
std::vector<Rml::Vector2f> path_of(const Edge& e, const View& v)
{
    const Body& A = body_of(e.a);
    const Body& B = body_of(e.b);
    const Rml::Vector2f pa = v.screen(A.p), pb = v.screen(B.p), d = pb - pa;
    const float len = std::sqrt(d.x * d.x + d.y * d.y);
    if (len < 1) return {};
    const Rml::Vector2f perp(-d.y / len, d.x / len);
    const Rml::Vector2f ctrl = (pa + pb) * 0.5f + perp * (e.twin ? BOW * 2 * v.s : 0.f);

    constexpr int STEPS = 32;
    std::vector<Rml::Vector2f> points;
    for (int i = 0; i <= STEPS; i++) {
        const float t = (float)i / STEPS, u = 1 - t;
        points.push_back(pa * (u * u) + ctrl * (2 * u * t) + pb * (t * t));
    }
    const float gap = 3;
    const float ahw = A.w * v.s / 2 + gap, ahh = BOX_H * v.s / 2 + gap;
    const float bhw = B.w * v.s / 2 + gap, bhh = BOX_H * v.s / 2 + gap;
    int first = 0, last = STEPS;
    while (first <= STEPS && inside(points[first], pa, ahw, ahh)) first++;
    while (last >= 0 && inside(points[last], pb, bhw, bhh)) last--;
    if (first == 0 || last == STEPS || first > last) return {};
    std::vector<Rml::Vector2f> out;
    out.push_back(leave(points[first - 1], points[first], pa, ahw, ahh));
    for (int i = first; i <= last; i++) out.push_back(points[i]);
    out.push_back(leave(points[last + 1], points[last], pb, bhw, bhh));
    return out;
}

float length_of(const std::vector<Rml::Vector2f>& path)
{
    float total = 0;
    for (size_t i = 1; i < path.size(); i++) {
        const Rml::Vector2f d = path[i] - path[i - 1];
        total += std::sqrt(d.x * d.x + d.y * d.y);
    }
    return total;
}

/* The point at a distance along a path. */
Rml::Vector2f along(const std::vector<Rml::Vector2f>& path, float at)
{
    for (size_t i = 1; i < path.size(); i++) {
        const Rml::Vector2f d = path[i] - path[i - 1];
        const float len = std::sqrt(d.x * d.x + d.y * d.y);
        if (at <= len && len > 0) return path[i - 1] + d * (at / len);
        at -= len;
    }
    return path.back();
}

/* The piece of a path from one distance to another, as points. */
std::vector<Rml::Vector2f> piece(const std::vector<Rml::Vector2f>& path, float from, float to)
{
    std::vector<Rml::Vector2f> out{ along(path, from) };
    float at = 0;
    for (size_t i = 1; i < path.size(); i++) {
        const Rml::Vector2f d = path[i] - path[i - 1];
        at += std::sqrt(d.x * d.x + d.y * d.y);
        if (at > from && at < to) out.push_back(path[i]);
    }
    out.push_back(along(path, to));
    return out;
}

float distance_to(const std::vector<Rml::Vector2f>& path, Rml::Vector2f p)
{
    float best = 1e9f;
    for (size_t i = 1; i < path.size(); i++) {
        const Rml::Vector2f a = path[i - 1], d = path[i] - a;
        const float l2 = d.x * d.x + d.y * d.y;
        const float t  = l2 > 0 ? std::max(0.f, std::min(1.f, ((p.x - a.x) * d.x + (p.y - a.y) * d.y) / l2)) : 0.f;
        const Rml::Vector2f q = a + d * t - p;
        best = std::min(best, std::sqrt(q.x * q.x + q.y * q.y));
    }
    return best;
}

Rml::Colourb kind_ink(Kind kind)
{
    switch (kind) {
    case Kind::Function: return ink.accent;
    case Kind::Variable: return ink.amber;
    case Kind::Task:     return ink.green;
    default:             return ink.dim;
    }
}

/* A rate as a label says it, empty while unknown. */
std::string rate_text(double hz)
{
    return hz >= 0 ? format_rate(hz) : std::string();
}

class MeshElement : public Rml::Element, public Rml::EventListener {
public:
    explicit MeshElement(const Rml::String& tag) : Rml::Element(tag)
    {
        for (const Rml::EventId id : EVENTS) AddEventListener(id, this);
    }

    ~MeshElement() override
    {
        for (const Rml::EventId id : EVENTS) RemoveEventListener(id, this);
    }

    void ProcessEvent(Rml::Event& event) override
    {
        const Rml::Vector2f at = Rml::Vector2f(event.GetParameter<float>("mouse_x", 0.f), event.GetParameter<float>("mouse_y", 0.f)) -
                                 GetAbsoluteOffset(Rml::BoxArea::Content);
        switch (event.GetId()) {
        case Rml::EventId::Mousemove:
            if (!dragging_) hover(at);
            break;
        case Rml::EventId::Mouseout:
            if (!dragging_) hover_node_ = hover_edge_ = -1;
            break;
        case Rml::EventId::Mousedown:
            if (event.GetParameter<int>("button", 0) != 0) break;
            hover(at);
            grabbed_ = hover_node_;
            moved_   = 0;
            last_    = at;
            break;
        case Rml::EventId::Dragstart:
            dragging_ = true;
            if (grabbed_ >= 0 && grabbed_ < (int)st.keys.size()) body_of(grabbed_).pinned = true;
            break;
        case Rml::EventId::Drag: {
            const Rml::Vector2f d = at - last_;
            last_ = at;
            moved_ += std::fabs(d.x) + std::fabs(d.y);
            if (grabbed_ >= 0 && grabbed_ < (int)st.keys.size()) {
                body_of(grabbed_).p += d / view_.s;
                st.heat = std::max(st.heat, 0.3f);
            } else {
                st.centre -= d / view_.s;
            }
            st.fit = false;
            break;
        }
        case Rml::EventId::Dragend:
            dragging_ = false;
            break;
        case Rml::EventId::Click:
            /* a press that moved was a drag, not a pick */
            if (moved_ > 4) break;
            hover(at);
            if (hover_node_ >= 0) mesh_select(MeshPick{ st.keys[hover_node_], {}, {} });
            else if (hover_edge_ >= 0)
                mesh_select(MeshPick{ {}, st.keys[st.edges[hover_edge_].a], st.keys[st.edges[hover_edge_].b] });
            else mesh_select(MeshPick());
            break;
        case Rml::EventId::Dblclick:
            hover(at);
            if (hover_node_ >= 0) {
                body_of(hover_node_).pinned = false;
                st.heat = std::max(st.heat, 0.3f);
            } else if (hover_edge_ < 0) {
                mesh_fit();
            }
            break;
        case Rml::EventId::Mousescroll: {
            /* the point under the pointer stays put */
            const Rml::Vector2f under = view_.world(at);
            const float wheel = event.GetParameter<float>("wheel_delta_y", 0.f);
            st.zoom = std::max(ZOOM_MIN, std::min(ZOOM_MAX, st.zoom * std::pow(1.15f, -wheel)));
            view_.s = st.zoom * ratio_;
            st.centre = under - (at - view_.half) / view_.s;
            st.fit = false;
            event.StopPropagation();
            break;
        }
        default:
            break;
        }
    }

protected:
    void OnUpdate() override
    {
        Rml::Element::OnUpdate();
        refresh();
    }

    void OnRender() override
    {
        Rml::Element::OnRender();
        if (!source) return;
        refresh();
        Canvas c(*this, shapes_);
        if (c.w() < 1 || c.h() < 1) return;
        read_ink(*this);
        ratio_ = c.dp(1);

        const Capture::MeshView& mesh = source->mesh();
        if (mesh.nodes.empty() || st.shown.empty()) {
            c.text(mesh.nodes.empty() ? "no nodes discovered" : "nothing matches the filter", c.w() / 2, c.h() / 2,
                   Canvas::Center, ink.faint, 12);
            c.render();
            paths_.clear();
            return;
        }

        for (const int i : st.shown) {
            Body& body = body_of(i);
            if (body.w <= 0) body.w = BOX_PAD * 2 + BOX_DOT + c.text_width(mesh.nodes[i].name, NAME_DP) / ratio_;
        }
        for (int k = 0; k < 2 && st.heat > 0; k++) tick();
        park();
        frame(c);
        view_.half   = Rml::Vector2f(c.w() / 2, c.h() / 2);
        view_.centre = st.centre;
        view_.s      = st.zoom * ratio_;

        draw_edges(c);
        draw_nodes(c);
        draw_legend(c);
        c.render();
    }

private:
    static constexpr Rml::EventId EVENTS[] = {
        Rml::EventId::Mousemove, Rml::EventId::Mouseout,  Rml::EventId::Mousedown, Rml::EventId::Dragstart,
        Rml::EventId::Drag,      Rml::EventId::Dragend,   Rml::EventId::Click,     Rml::EventId::Dblclick,
        Rml::EventId::Mousescroll,
    };

    /* While fitting, the view eases toward the box round every shown node. */
    void frame(const Canvas& c)
    {
        if (!st.fit) return;
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (const int i : st.shown) {
            const Body& b = body_of(i);
            x0 = std::min(x0, b.p.x - b.w / 2);
            x1 = std::max(x1, b.p.x + b.w / 2);
            y0 = std::min(y0, b.p.y - BOX_H / 2);
            y1 = std::max(y1, b.p.y + BOX_H / 2);
        }
        const float margin = 40, w = c.w() / ratio_, h = c.h() / ratio_ - 24;   /* the legend's strip */
        const float zoom = std::max(ZOOM_MIN, std::min(FIT_MAX, std::min((w - 2 * margin) / std::max(1.f, x1 - x0),
                                                                          (h - 2 * margin) / std::max(1.f, y1 - y0))));
        const Rml::Vector2f centre((x0 + x1) / 2, (y0 + y1) / 2 + 12 / zoom);
        st.centre += (centre - st.centre) * 0.2f;
        st.zoom   += (zoom - st.zoom) * 0.2f;
    }

    bool picked_edge(const Edge& e) const
    {
        const int from = index_of(st.pick.from), to = index_of(st.pick.to);
        return (e.a == from && e.b == to) || (e.a == to && e.b == from);
    }

    void draw_edges(Canvas& c)
    {
        paths_.assign(st.edges.size(), {});
        const int    node = index_of(st.pick.node);
        const double now  = Capture::now_s();
        const float  z    = std::max(0.6f, std::min(1.3f, st.zoom));
        struct Tag {
            Rml::Vector2f at;
            std::string   text;
            Rml::Vector2f side;   /* a bowed arrow's tag moves this way off its curve */
            bool          ahead;  /* hovered or picked: it may cover a box */
        };
        std::vector<Tag> tags;
        for (size_t i = 0; i < st.edges.size(); i++) {
            const Edge& e = st.edges[i];
            std::vector<Rml::Vector2f> path = path_of(e, view_);
            if (path.size() < 2) continue;
            paths_[i] = path;
            const bool lit = st.lit[e.a] && st.lit[e.b];
            const bool hot = (int)i == hover_edge_ || picked_edge(e) || (node >= 0 && (e.a == node || e.b == node));
            const float fade = lit ? 1.f : 0.15f;
            const Rml::Colourb colour = e.lost ? ink.red : e.mixed ? ink.dim : kind_ink(e.kind);
            const bool known = e.hz >= 0 || e.active || e.lost;

            float width = 1.2f;
            if (e.active && e.hz > 0) width += std::min(2.2f, 0.7f * (float)std::log10(1 + e.hz));
            if (hot || e.lost) width += 0.6f;
            const Rml::Colourb line = faded(colour, fade * (hot || e.lost ? 1.f : !known ? 0.55f : e.active ? 0.8f : 0.35f));

            /* the head, then the line stopping at its base so the two never overlap */
            const float total = length_of(path), head = std::min(total * 0.5f, c.dp(8) * z);
            const Rml::Vector2f tip = path.back(), base = along(path, total - head);
            const Rml::Vector2f d = tip - base;
            const float dl = std::max(1e-3f, std::sqrt(d.x * d.x + d.y * d.y));
            const Rml::Vector2f side(-d.y / dl * head * 0.45f, d.x / dl * head * 0.45f);
            c.triangle(tip, base + side, base - side, line);
            const std::vector<Rml::Vector2f> body = piece(path, 0, total - head);
            if (known) {
                for (size_t k = 1; k < body.size(); k++) c.line(body[k - 1], body[k], line, width * z);
            } else {
                const float dash = c.dp(4) * z, run = total - head;
                for (float t = 0; t < run; t += 2 * dash) {
                    const std::vector<Rml::Vector2f> bit = piece(path, t, std::min(run, t + dash));
                    for (size_t k = 1; k < bit.size(); k++) c.line(bit[k - 1], bit[k], line, width * z);
                }
            }

            /* Dots run along an arrow while traffic flows, closer and quicker as it grows. */
            /* A lost arrow carries a cross at its middle instead of dots. */
            if (e.lost) {
                const Rml::Vector2f m = along(path, total / 2);
                const float r = c.dp(5) * z, k = r * 0.45f;
                c.dot(m, faded(ink.red, fade), 5 * z);
                c.line(m.x - k, m.y - k, m.x + k, m.y + k, faded(ink.panel, fade), 1.4f * z);
                c.line(m.x - k, m.y + k, m.x + k, m.y - k, faded(ink.panel, fade), 1.4f * z);
            } else if (e.active && e.hz > 0) {
                const float lg      = (float)std::log10(1 + e.hz);
                const float spacing = std::max(16.f, std::min(150.f, 150.f / (1 + lg))) * view_.s;
                const float speed   = (40.f + 30.f * lg) * view_.s;
                const Rml::Colourb dot = faded(e.kind == Kind::Topic || e.mixed ? ink.text : colour, fade);
                for (float t = std::fmod((float)now * speed, spacing); t < total - head; t += spacing)
                    c.dot(along(path, t), dot, 2.2f * z);
            }

            if (hot) {
                const Capture::MeshLink& first = source->mesh().links[e.links[0]];
                std::string text = e.links.size() == 1 ? first.name : std::to_string(e.links.size()) + " names";
                const std::string rate = e.lost ? "not received" : rate_text(e.hz);
                if (!rate.empty()) text += "  " + rate;
                /* the hovered and the picked win a place over the rest */
                const bool ahead = (int)i == hover_edge_ || picked_edge(e);
                const Rml::Vector2f span = path.back() - path.front();
                const float sl = std::max(1e-3f, std::sqrt(span.x * span.x + span.y * span.y));
                const Rml::Vector2f side = e.twin ? Rml::Vector2f(-span.y / sl, span.x / sl) : Rml::Vector2f(0, 0);
                tags.insert(ahead ? tags.begin() : tags.end(),
                            Tag{ along(path, total / 2) + Rml::Vector2f(0, e.lost ? -c.dp(14) * z : 0), text, side, ahead });
            }
        }
        /* Tags last, so no line crosses one. A tag that would overlap another, or a box
           unless it is hovered or picked, is left out, since text draws over every shape. */
        std::vector<Area> boxes;
        for (const int n : st.shown) {
            const Body& b = body_of(n);
            const Rml::Vector2f at = view_.screen(b.p);
            const float w = b.w * view_.s, h = BOX_H * view_.s;
            boxes.push_back(Area{ at.x - w / 2, at.y - h / 2, w, h });
        }
        auto overlap = [](const Area& a, const Area& b) {
            return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
        };
        std::vector<Area> placed;
        for (const Tag& tag : tags) {
            const float w = c.text_width(tag.text, 10) + c.dp(10), h = c.dp(16);
            const Rml::Vector2f at = tag.at + tag.side * (std::fabs(tag.side.x) * w / 2 + std::fabs(tag.side.y) * h / 2 + c.dp(4));
            const Area box{ at.x - w / 2, at.y - h / 2, w, h };
            auto hits = [&](const Area& a) { return overlap(box, a); };
            if (std::any_of(placed.begin(), placed.end(), hits)) continue;
            if (!tag.ahead && std::any_of(boxes.begin(), boxes.end(), hits)) continue;
            placed.push_back(box);
            c.rounded(box.x, box.y, w, h, 3, ink.panel);
            c.text(tag.text, at.x, at.y + c.dp(3.5f), Canvas::Center, ink.text, 10);
        }
    }

    void draw_nodes(Canvas& c)
    {
        const Capture::MeshView& mesh = source->mesh();
        const int node = index_of(st.pick.node);
        const int from = index_of(st.pick.from), to = index_of(st.pick.to);
        boxes_.clear();
        /* the picked and the hovered draw last, over their neighbours */
        std::vector<int> order = st.shown;
        std::stable_partition(order.begin(), order.end(), [&](int i) { return i != node && i != hover_node_; });

        const float name_px = std::round(c.dp(NAME_DP) * st.zoom);
        for (const int i : order) {
            const Body& b = body_of(i);
            const Rml::Vector2f at = view_.screen(b.p);
            const float w = b.w * view_.s, h = BOX_H * view_.s, x = at.x - w / 2, y = at.y - h / 2;
            const float fade = st.lit[i] ? 1.f : 0.25f;
            const bool picked = i == node || i == from || i == to;
            /* the pick is a fill, never an outline, as rows are */
            const Rml::Colourb edge = i == hover_node_ ? ink.dim : ink.border;
            c.rounded(x - 1, y - 1, w + 2, h + 2, 5 * st.zoom, faded(edge, fade));
            c.rounded(x, y, w, h, 5 * st.zoom, faded(ink.panel, fade));
            if (picked) c.rounded(x, y, w, h, 5 * st.zoom, faded(ink.accent, 0.3f * fade));
            const Capture::MeshNode& n = mesh.nodes[i];
            c.dot(x + (BOX_PAD + 3) * view_.s, at.y, faded(n.alive ? ink.green : ink.amber, fade), 3.5f * st.zoom);
            if (name_px >= 7)
                c.text(n.name, x + (BOX_PAD + BOX_DOT) * view_.s, at.y + name_px * 0.35f, Canvas::Left,
                       faded(ink.text, fade), name_px / ratio_);
            else if (i == hover_node_)
                c.text(n.name, at.x, y - c.dp(5), Canvas::Center, ink.text, 11);
            boxes_.push_back(Box{ i, x, y, w, h });
        }
    }

    /* What each colour means, and how much is shown, along the bottom. */
    void draw_legend(Canvas& c)
    {
        float x = c.dp(12);
        const float y = c.h() - c.dp(12);
        const struct {
            const char*  word;
            Rml::Colourb colour;
        } kinds[] = { { "topic", ink.dim }, { "function", ink.accent }, { "variable", ink.amber }, { "task", ink.green } };
        for (const auto& k : kinds) {
            c.line(x, y - c.dp(3.5f), x + c.dp(14), y - c.dp(3.5f), k.colour, 2);
            x += c.dp(19);
            c.text(k.word, x, y, Canvas::Left, ink.dim, 10);
            x += c.text_width(k.word, 10) + c.dp(14);
        }
        for (float t = 0; t < 14; t += 6) c.line(x + c.dp(t), y - c.dp(3.5f), x + c.dp(std::min(14.f, t + 3)), y - c.dp(3.5f), ink.dim, 1.2f);
        x += c.dp(19);
        c.text("no counters", x, y, Canvas::Left, ink.dim, 10);
        x += c.text_width("no counters", 10) + c.dp(14);
        c.dot(x + c.dp(5), y - c.dp(3.5f), ink.red, 5);
        x += c.dp(15);
        c.text("not received", x, y, Canvas::Left, ink.dim, 10);

        const int total = (int)source->mesh().nodes.size(), shown = (int)st.shown.size();
        std::string count = shown == total ? std::to_string(total) + " nodes" : std::to_string(shown) + " of " + std::to_string(total) + " nodes";
        count += ", " + std::to_string(st.links) + (st.links == 1 ? " link" : " links");
        c.text(count, c.w() - c.dp(12), y, Canvas::Right, ink.faint, 10);
    }

    /* What is under a point: a box first, then the nearest arrow within reach. */
    void hover(Rml::Vector2f at)
    {
        hover_node_ = hover_edge_ = -1;
        for (auto it = boxes_.rbegin(); it != boxes_.rend(); ++it)
            if (at.x >= it->x && at.x <= it->x + it->w && at.y >= it->y && at.y <= it->y + it->h) {
                hover_node_ = it->node;
                break;
            }
        if (hover_node_ < 0) {
            float best = 6 * ratio_;
            for (size_t i = 0; i < paths_.size() && i < st.edges.size(); i++) {
                if (paths_[i].size() < 2) continue;
                const float d = distance_to(paths_[i], at);
                if (d < best) {
                    best        = d;
                    hover_edge_ = (int)i;
                }
            }
        }
        const char* cursor = hover_node_ >= 0 ? "move" : hover_edge_ >= 0 ? "pointer" : "";
        if (cursor != cursor_) {
            cursor_ = cursor;
            if (*cursor) SetProperty("cursor", cursor);
            else RemoveProperty("cursor");
        }
    }

    struct Box {
        int   node;
        float x, y, w, h;
    };

    Rml::Mesh  shapes_;
    View       view_;
    float      ratio_ = 1;
    std::vector<std::vector<Rml::Vector2f>> paths_;   /* each edge's arrow as last drawn */
    std::vector<Box> boxes_;                          /* each box as last drawn, in draw order */
    int           hover_node_ = -1, hover_edge_ = -1, grabbed_ = -1;
    bool          dragging_ = false;
    float         moved_ = 0;
    Rml::Vector2f last_{ 0, 0 };
    const char*   cursor_ = "";
};

Rml::ElementInstancerGeneric<MeshElement> instancer;

/* A system name while the funnel hides them. It is a view setting rather than a search,
   so the sidebar follows it too. */
bool name_hidden(const std::string& name)
{
    return (st.filter.cats & MESH_NO_SYSTEM) && is_system(name);
}

/* The rows of a set of links grouped by name, each with the nodes at the end the
   context does not already say: its consumers, or its providers. */
std::vector<MeshRow> rows_of(const std::vector<int>& links, bool from, bool to)
{
    const Capture::MeshView& mesh = source->mesh();
    struct Group {
        std::string           name;
        Kind                  kind = Kind::Topic;
        std::set<std::string> from, to;
        double                hz = -1, sent = -1;
        bool                  active = false, lost = false;
    };
    std::map<std::string, Group> groups;
    for (const int i : links) {
        const Capture::MeshLink& l = mesh.links[i];
        Group& g = groups[Capture::key(l.kind, l.name)];
        g.name = l.name;
        g.kind = l.kind;
        if (from) g.from.insert(mesh.nodes[l.from].name);
        if (to) g.to.insert(mesh.nodes[l.to].name);
        g.hz     = std::max(g.hz, l.hz);
        g.sent   = std::max(g.sent, l.sent_hz);
        g.active = g.active || l.active;
        g.lost   = g.lost || l.lost;
    }
    auto join = [](const std::set<std::string>& names) {
        std::string out;
        int         k = 0;
        for (const std::string& name : names) {
            if (k == 3) return out + " +" + std::to_string(names.size() - 3);
            out += (k++ ? ", " : "") + name;
        }
        return out;
    };
    std::vector<MeshRow> rows;
    for (const auto& entry : groups) {
        MeshRow row;
        row.key    = entry.first;
        row.name   = entry.second.name;
        row.kind   = entry.second.kind;
        row.from   = join(entry.second.from);
        row.to     = join(entry.second.to);
        row.rate   = entry.second.lost ? format_rate(entry.second.sent) + " lost" : format_rate(entry.second.hz);
        row.active = entry.second.active;
        row.lost   = entry.second.lost;
        rows.push_back(std::move(row));
    }
    return rows;
}

/* Names with nobody on the other side, grouped, with the rate their nodes report while
   there is any, and the nodes that want each when asked. */
std::vector<MeshRow> ends_of(const std::vector<Capture::MeshEnd>& ends, int node, bool name_nodes)
{
    const Capture::MeshView& mesh = source->mesh();
    std::map<std::string, MeshRow> rows;
    for (const Capture::MeshEnd& end : ends) {
        if ((node >= 0 && end.node != node) || name_hidden(end.name)) continue;
        const std::string key = Capture::key(end.kind, end.name);
        MeshRow& row = rows[key];
        row.key  = key;
        row.name = end.name;
        row.kind = end.kind;
        const double hz = source->traffic_hz(key);
        row.rate = hz > 0 ? format_rate(hz) : std::string();
        if (name_nodes) row.to += (row.to.empty() ? "" : ", ") + mesh.nodes[end.node].name;
    }
    std::vector<MeshRow> out;
    for (auto& entry : rows) out.push_back(std::move(entry.second));
    return out;
}

void add_group(MeshDetails& out, const std::string& title, std::vector<MeshRow> rows)
{
    if (!rows.empty()) out.groups.push_back(MeshGroup{ title, std::move(rows) });
}

} /* namespace */

void mesh_init(Capture& capture)
{
    source = &capture;
    Rml::Factory::RegisterElementInstancer("mesh-graph", &instancer);
}

void mesh_set_filter(const MeshFilter& filter)
{
    if (filter == st.filter) return;
    st.filter = filter;
    st.dirty  = true;
}

const MeshPick& mesh_pick()
{
    return st.pick;
}

void mesh_select(const MeshPick& pick)
{
    if (pick == st.pick) return;
    st.pick  = pick;
    st.dirty = true;
}

void mesh_fit()
{
    st.fit = true;
}

void mesh_relayout()
{
    for (auto& entry : st.bodies) entry.second.pinned = false;
    st.heat = 1;
    st.fit  = true;
}

MeshDetails mesh_details()
{
    MeshDetails out;
    if (!source) return out;
    refresh();
    const Capture::MeshView& mesh = source->mesh();
    const int node = index_of(st.pick.node), from = index_of(st.pick.from), to = index_of(st.pick.to);
    std::vector<int> kept;
    for (size_t i = 0; i < mesh.links.size(); i++)
        if (!name_hidden(mesh.links[i].name)) kept.push_back((int)i);

    if (node >= 0) {
        const Capture::MeshNode& n = mesh.nodes[node];
        out.mode    = 1;
        out.title   = n.name;
        out.host    = n.host;
        out.alive   = n.alive;
        out.reports = n.reports;
        std::vector<int> sends, receives;
        for (const int i : kept) {
            if (mesh.links[i].from == node) sends.push_back(i);
            if (mesh.links[i].to == node) receives.push_back(i);
        }
        add_group(out, "Sends", rows_of(sends, false, true));
        add_group(out, "Receives", rows_of(receives, true, false));
        add_group(out, "Nobody consumes", ends_of(mesh.unheard, node, false));
        add_group(out, "No provider", ends_of(mesh.waiting, node, false));
        return out;
    }
    if (from >= 0 && to >= 0) {
        out.mode = 2;
        out.from = mesh.nodes[from].name;
        out.to   = mesh.nodes[to].name;
        std::vector<int> there, back;
        for (const int i : kept) {
            if (mesh.links[i].from == from && mesh.links[i].to == to) there.push_back(i);
            if (mesh.links[i].from == to && mesh.links[i].to == from) back.push_back(i);
        }
        add_group(out, out.from + " to " + out.to, rows_of(there, false, false));
        add_group(out, out.to + " to " + out.from, rows_of(back, false, false));
        return out;
    }

    int    active = 0;
    double published = -1;
    for (const int i : kept) active += mesh.links[i].active ? 1 : 0;
    for (const Capture::TopicRow& topic : source->topics()) {
        if (name_hidden(topic.name)) continue;
        const double hz = source->traffic_hz(topic.key);
        if (hz >= 0) published = std::max(published, 0.0) + hz;
    }
    out.nodes   = std::to_string(mesh.nodes.size());
    out.links   = std::to_string(kept.size());
    out.active  = std::to_string(active);
    out.traffic = format_rate(published);

    std::vector<int> lost, busy;
    for (const int i : kept) {
        if (mesh.links[i].lost) lost.push_back(i);
        else if (mesh.links[i].hz > 0) busy.push_back(i);
    }
    std::vector<MeshRow> dropped;
    for (const int i : lost) {
        const Capture::MeshLink& l = mesh.links[i];
        MeshRow row;
        row.key  = Capture::key(l.kind, l.name);
        row.name = l.name;
        row.kind = l.kind;
        row.from = mesh.nodes[l.from].name;
        row.to   = mesh.nodes[l.to].name;
        row.rate = format_rate(l.sent_hz) + " lost";
        row.lost = true;
        dropped.push_back(std::move(row));
    }
    add_group(out, "Sent, not received", std::move(dropped));
    /* By quarter decades of rate, then by name, so the order holds while rates wobble and a
       row stays under the pointer. */
    auto band = [&mesh](int i) { return (int)std::floor(std::log10(mesh.links[i].hz) * 4); };
    std::sort(busy.begin(), busy.end(), [&mesh, &band](int x, int y) {
        if (band(x) != band(y)) return band(x) > band(y);
        const Capture::MeshLink &a = mesh.links[x], &b = mesh.links[y];
        return std::tie(a.name, mesh.nodes[a.from].name, mesh.nodes[a.to].name) <
               std::tie(b.name, mesh.nodes[b.from].name, mesh.nodes[b.to].name);
    });
    if (busy.size() > 8) busy.resize(8);
    std::vector<MeshRow> busiest;
    for (const int i : busy) {
        const Capture::MeshLink& l = mesh.links[i];
        MeshRow row;
        row.key    = Capture::key(l.kind, l.name);
        row.name   = l.name;
        row.kind   = l.kind;
        row.from   = mesh.nodes[l.from].name;
        row.to     = mesh.nodes[l.to].name;
        row.rate   = format_rate(l.hz);
        row.active = l.active;
        busiest.push_back(std::move(row));
    }
    add_group(out, "Busiest", std::move(busiest));
    add_group(out, "Waiting for a provider", ends_of(mesh.waiting, -1, true));
    return out;
}
