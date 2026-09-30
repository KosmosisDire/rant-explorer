#include "viz.hpp"

#include "canvas.hpp"
#include "format.hpp"
#include "image.hpp"
#include "values.hpp"
#include "video.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Core/CallbackTexture.h>
#include <RmlUi/Core/ElementInstancer.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/FontEngineInterface.h>
#include <RmlUi/Core/Geometry.h>
#include <RmlUi/Core/Mesh.h>
#include <RmlUi/Core/MeshUtilities.h>
#include <RmlUi/Core/RenderManager.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

namespace {

using ValueNode = Capture::ValueNode;
using Nodes     = std::vector<ValueNode>;

Capture* source = nullptr;

/* x, y and z. */
Rml::Colourb axis_colour(int i)
{
    return i == 0 ? ink.red : i == 1 ? ink.green : ink.blue;
}

Rml::Colourb palette(long long i)
{
    const Rml::Colourb colours[] = { ink.accent, ink.amber, ink.green, ink.blue, ink.purple, ink.gray };
    const long long n = (long long)(sizeof colours / sizeof colours[0]);
    return colours[((i % n) + n) % n];
}

/* The strips kept free for text at the top and bottom of a drawing, in dp. */
constexpr float TOP = 22, BOT = 16;

/* How much history a plot shows and a trail keeps, and how far ahead a twist's path looks. */
constexpr double PLOT_SECONDS = 10, TRAIL_SECONDS = 30, TWIST_AHEAD = 2;

/* A picture zooms in until one of its texels is this many dp. */
constexpr float ZOOM_TEXEL = 32;

/* An array up to this long is labelled per element. */
constexpr size_t LABELLED = 16;

/* ------------------------------------------------------------------ formatting */

/* A value as a person reads it: a time or a span for those types, else the tree's text. */
std::string human(const ValueNode& node)
{
    if (node.std_name == "Timestamp") return format_timestamp(node.integer);
    if (node.std_name == "Duration")  return format_span(node.integer);
    return value_text(node);
}

std::string print(const char* fmt, double v)
{
    char buf[48];
    std::snprintf(buf, sizeof buf, fmt, v);
    return buf;
}

std::string f3(double v) { return print("%.3f", v); }

/* A grid label on a value axis. */
std::string axis_label(double v, bool is_float)
{
    if (is_float || std::fabs(v - std::round(v)) > 1e-9) return f3(v);
    return print("%.0f", v);
}

/* A scale written short: 120, 12.5, 1.25. */
std::string short_number(double x)
{
    return x >= 100 ? print("%.0f", x) : x >= 10 ? print("%.1f", x) : print("%.2f", x);
}

/* A round step near x, so a grid can be read as a ruler. */
double nice(double x)
{
    if (!(x > 0)) return 1;
    const double p = std::pow(10.0, std::floor(std::log10(x))), m = x / p;
    return (m <= 1 ? 1 : m <= 2 ? 2 : m <= 5 ? 5 : 10) * p;
}

/* ------------------------------------------------------------------ the value */

/* A branch in one line for a table cell: an array as [a, b], a struct as (x, y), nested
   as deep as it goes and cut short past BRIEF characters. */
constexpr size_t BRIEF = 60;

void brief_into(const Nodes& nodes, int index, std::string& out)
{
    const ValueNode& node = nodes[index];
    if (node.kind != ValueNode::Struct && node.kind != ValueNode::Array) {
        out += node.kind == ValueNode::Gap ? "..." : human(node);
        return;
    }
    out += node.kind == ValueNode::Array ? '[' : '(';
    bool first = true;
    for (const int k : children_of(nodes, index)) {
        if (out.size() > BRIEF) break;
        if (!first) out += ", ";
        first = false;
        brief_into(nodes, k, out);
    }
    out += node.kind == ValueNode::Array ? ']' : ')';
}

std::string brief(const Nodes& nodes, int index)
{
    std::string out;
    brief_into(nodes, index, out);
    if (out.size() > BRIEF) out = out.substr(0, BRIEF) + "...";
    return out;
}

int kid(const Nodes& nodes, int index, const char* name)
{
    for (const int k : children_of(nodes, index))
        if (nodes[k].name == name) return k;
    return -2;
}

double number(const Nodes& nodes, int index, const char* name)
{
    const int k = kid(nodes, index, name);
    return k >= 0 ? nodes[k].number : 0;
}

std::string child_path(const std::string& path, const char* name)
{
    return path.empty() ? std::string(name) : path + "." + name;
}

struct V3 {
    double x = 0, y = 0, z = 0;
};

struct Quat {
    double x = 0, y = 0, z = 0, w = 1;
};

V3 vec(const Nodes& nodes, int index)
{
    return V3{ number(nodes, index, "x"), number(nodes, index, "y"), number(nodes, index, "z") };
}

Quat quat(const Nodes& nodes, int index)
{
    return Quat{ number(nodes, index, "x"), number(nodes, index, "y"), number(nodes, index, "z"),
                 number(nodes, index, "w") };
}

V3 operator+(const V3& a, const V3& b) { return V3{ a.x + b.x, a.y + b.y, a.z + b.z }; }
V3 operator-(const V3& a, const V3& b) { return V3{ a.x - b.x, a.y - b.y, a.z - b.z }; }
V3 operator*(const V3& a, double k) { return V3{ a.x * k, a.y * k, a.z * k }; }
V3 cross(const V3& a, const V3& b) { return V3{ a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
double length(const V3& a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }

Quat operator*(const Quat& a, const Quat& b)
{
    return Quat{ a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                 a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}

/* The rotation an angular velocity w makes in t seconds. */
Quat turned(const V3& w, double t)
{
    const double angle = length(w) * t;
    if (angle < 1e-12) return Quat{};
    const double k = std::sin(angle / 2) / length(w);
    return Quat{ w.x * k, w.y * k, w.z * k, std::cos(angle / 2) };
}

V3 rotate(const Quat& q, const V3& v)
{
    const double n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    const double s = n > 0 ? 1 / n : 1, x = q.x * s, y = q.y * s, z = q.z * s, w = n > 0 ? q.w * s : 1;
    const double tx = 2 * (y * v.z - z * v.y), ty = 2 * (z * v.x - x * v.z), tz = 2 * (x * v.y - y * v.x);
    return V3{ v.x + w * tx + (y * tz - z * ty), v.y + w * ty + (z * tx - x * tz), v.z + w * tz + (x * ty - y * tx) };
}

/* Roll, pitch and yaw in degrees, each "12.3°". */
std::string euler(const Quat& q)
{
    const double n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    const double s = n > 0 ? 1 / n : 1, x = q.x * s, y = q.y * s, z = q.z * s, w = n > 0 ? q.w * s : 1;
    const double sp = 2 * (w * y - z * x), deg = 180 / 3.14159265358979;
    const double roll  = std::atan2(2 * (w * x + y * z), 1 - 2 * (x * x + y * y)) * deg;
    const double pitch = (std::fabs(sp) >= 1 ? std::copysign(3.14159265358979 / 2, sp) : std::asin(sp)) * deg;
    const double yaw   = std::atan2(2 * (w * z + x * y), 1 - 2 * (y * y + z * z)) * deg;
    return "roll " + print("%.1f\xC2\xB0", roll) + "  pitch " + print("%.1f\xC2\xB0", pitch) +
           "  yaw " + print("%.1f\xC2\xB0", yaw);
}

/* ------------------------------------------------------------------ drawing kit */

/* Dots of one colour and size, one per cell of half their radius: a trail costs the area it
   covers, not the messages it holds, and looks the same. */
class Dots {
public:
    Dots(Canvas& c, Rml::Colourb colour, float radius_dp)
        : c_(c), colour_(colour), radius_(radius_dp), cell_(std::max(1.f, c.dp(radius_dp) / 2)),
          cols_((int)std::ceil(c.w() / cell_) + 1), rows_((int)std::ceil(c.h() / cell_) + 1),
          taken_((size_t)cols_ * rows_, 0)
    {
    }

    void add(Rml::Vector2f p)
    {
        if (!(p.x >= 0 && p.y >= 0)) return;   /* off the drawing, or NaN */
        const int col = (int)(p.x / cell_), row = (int)(p.y / cell_);
        if (col >= cols_ || row >= rows_) return;
        uint8_t& cell = taken_[(size_t)row * cols_ + col];
        if (cell) return;
        cell = 1;
        c_.dot(p, colour_, radius_);
    }

private:
    Canvas&              c_;
    Rml::Colourb         colour_;
    float                radius_, cell_;
    int                  cols_, rows_;
    std::vector<uint8_t> taken_;
};

/* A time series cut to what a pixel column can show: its first, lowest, highest and last
   points in order. Drawn, it covers the same pixels as every point would. */
class Columns {
public:
    void add(Rml::Vector2f p)
    {
        const float col = std::floor(p.x);
        if (n_ && col == col_) {
            if (p.y < lo_.p.y) lo_ = { p, n_ };
            if (p.y > hi_.p.y) hi_ = { p, n_ };
            last_ = { p, n_ };
            n_++;
            return;
        }
        flush();
        col_ = col;
        first_ = lo_ = hi_ = last_ = { p, 0 };
        n_ = 1;
    }

    const std::vector<Rml::Vector2f>& points()
    {
        flush();
        return out_;
    }

private:
    struct At {
        Rml::Vector2f p;
        int           i = 0;
    };

    void flush()
    {
        if (!n_) return;
        At keep[4] = { first_, lo_.i < hi_.i ? lo_ : hi_, lo_.i < hi_.i ? hi_ : lo_, last_ };
        int drawn = -1;
        for (const At& a : keep)
            if (a.i > drawn) {
                out_.push_back(a.p);
                drawn = a.i;
            }
        n_ = 0;
    }

    std::vector<Rml::Vector2f> out_;
    float                      col_ = 0;
    int                        n_ = 0;
    At                         first_, lo_, hi_, last_;
};

/* A bound moves toward its target a little each frame, so a new extreme never rescales a
   picture in one jump. The rate matches the mock's 0.15 per 40 ms tick. */
double ease(double current, double target, double dt)
{
    return current + (target - current) * (1 - std::pow(0.85, dt / 0.04));
}

/* Cuts a segment to a rectangle, Liang and Barsky. False when none of it is inside. */
bool clip(float& ax, float& ay, float& bx, float& by, float left, float top, float right, float bottom)
{
    const float dx = bx - ax, dy = by - ay;
    float t0 = 0, t1 = 1;
    const float p[4] = { -dx, dx, -dy, dy };
    const float q[4] = { ax - left, right - ax, ay - top, bottom - ay };
    for (int i = 0; i < 4; i++) {
        if (p[i] == 0) {
            if (q[i] < 0) return false;
            continue;
        }
        const float r = q[i] / p[i];
        if (p[i] < 0) t0 = std::max(t0, r);
        else          t1 = std::min(t1, r);
        if (t0 > t1) return false;
    }
    const float x0 = ax + t0 * dx, y0 = ay + t0 * dy;
    bx = ax + t1 * dx;
    by = ay + t1 * dy;
    ax = x0;
    ay = y0;
    return true;
}

bool inside(Rml::Vector2f p, float left, float top, float right, float bottom)
{
    return p.x >= left && p.x <= right && p.y >= top && p.y <= bottom;
}

/* The time axis of a strip: a line every two seconds, each named along the bottom, now at
   the right. */
template <class X>
void time_grid(Canvas& c, X x_of, double now, float y0, float y1)
{
    for (int sec = 2; sec <= 10; sec += 2) {
        const float x = x_of(now - sec);
        c.line(x, y0, x, y1, ink.grid);
        c.text("-" + std::to_string(sec) + " s", x, c.h() - c.dp(5), sec == 10 ? Canvas::Left : Canvas::Center, ink.faint);
    }
    c.text("now", x_of(now), c.h() - c.dp(5), Canvas::Right, ink.faint);
}

/* Horizontal lines at round steps across a value range, labelled in the left gutter. */
template <class Y>
void value_grid(Canvas& c, float x0, float x1, double lo, double hi, Y y_of, bool is_float)
{
    const double step = nice((hi - lo) / 4);
    for (double v = std::ceil(lo / step) * step; v <= hi + 1e-9; v += step) {
        c.line(x0, y_of(v), x1, y_of(v), ink.grid);
        c.text(axis_label(v, is_float), x0 - c.dp(6), y_of(v) + c.dp(3), Canvas::Right, ink.faint);
    }
}

/* The current value at the top right of an area, clear of the pin and close buttons. */
void corner(Canvas& c, const Area& a, const std::string& s, Rml::Colourb colour = ink.text, float size = 14)
{
    c.text(s, a.x + a.w - c.dp(54), a.y + c.dp(17), Canvas::Right, colour, size);
}

/* Components along the bottom of an area: the left in the text colour, the right faint. */
void foot(Canvas& c, const Area& a, const std::string& left, const std::string& right = {})
{
    if (!left.empty())  c.text(left, a.x + c.dp(8), a.y + a.h - c.dp(5), Canvas::Left, ink.dim);
    if (!right.empty()) c.text(right, a.x + a.w - c.dp(8), a.y + a.h - c.dp(5), Canvas::Right, ink.faint);
}

/* ------------------------------------------------------------------ bounds */

/* The reach of a scene: the largest magnitude seen since the card opened, with a margin,
   eased. It only grows. */
struct Reach {
    bool   init = false;
    double seen = 0, eased = 0;

    double grow(std::initializer_list<double> values, double dt, double margin = 1.08)
    {
        for (const double v : values) seen = std::max(seen, std::fabs(v));
        const double target = std::max(seen, 1e-3) * margin;
        eased = init ? ease(eased, target, dt) : target;
        init  = true;
        return eased;
    }
};

/* ------------------------------------------------------------------ the 3D scene */

/* The camera: a yaw and an elevation in degrees, and a zoom. At rest it is isometric and
   the reach meets the edge. Dragging orbits it, the wheel zooms, a double click resets. */
struct Camera {
    double a = 45, e = 35, zoom = 1;
};

/* A view of a cube around the origin reaching r, z up, scaled so the reach meets the
   area's edge with the origin centred between the text strips. Depth is toward the
   viewer, so a scene can paint back to front. */
struct Scene {
    double ca = 1, sa = 0, ce = 1, se = 0, k = 1, cx = 0, cy = 0, r = 1;

    Rml::Vector2f operator()(double x, double y, double z) const
    {
        const double u = x * ca - y * sa, v = x * sa + y * ca;
        return { (float)(cx + u * k), (float)(cy + (v * se - z * ce) * k) };
    }
    Rml::Vector2f operator()(const V3& p) const { return (*this)(p.x, p.y, p.z); }
    double depth(const V3& p) const { return (p.x * sa + p.y * ca) * ce + p.z * se; }
    bool   front(const V3& p) const { return depth(p) >= 0; }
};

Scene scene(Canvas& c, const Area& a, double r, const Camera& cam)
{
    const double rad = 3.14159265358979 / 180;
    Scene s;
    s.ca = std::cos(cam.a * rad); s.sa = std::sin(cam.a * rad);
    s.ce = std::cos(cam.e * rad); s.se = std::sin(cam.e * rad);
    s.r  = r;
    s.k  = std::min(a.w - c.dp(16), a.h - c.dp(TOP + BOT + 4)) / (2 * r) * cam.zoom;
    s.cx = a.x + a.w / 2;
    s.cy = a.y + c.dp(TOP) + (a.h - c.dp(TOP + BOT)) / 2;
    /* the floor grid at a round step, running off the sides where it will */
    const double step = nice(2 * r / 4);
    for (double m = std::ceil(-r / step) * step; m <= r + 1e-9; m += step) {
        c.line(s(m, -r, 0), s(m, r, 0), ink.grid);
        c.line(s(-r, m, 0), s(r, m, 0), ink.grid);
    }
    return s;
}

/* The axes from the origin toward the reach, each named at its end. */
void axes(Canvas& c, const Scene& s)
{
    const double r = s.r * 0.92;
    const V3 ends[3] = { { r, 0, 0 }, { 0, r, 0 }, { 0, 0, r } };
    const char* names[3] = { "x", "y", "z" };
    for (int i = 0; i < 3; i++) {
        c.line(s(0, 0, 0), s(ends[i]), axis_colour(i));
        const Rml::Vector2f e = s(ends[i]);
        c.text(names[i], e.x + (i == 2 ? 0 : c.dp(7)), e.y + (i == 2 ? -c.dp(5) : c.dp(4)), Canvas::Center, axis_colour(i));
    }
}

/* A point's trail in the air, and its foot on the floor with a dashed line up to it,
   the part on one side of the depth split. Nothing else is projected. */
void shadow(Canvas& c, const Scene& s, const V3& v, const std::vector<V3>* trail, bool front)
{
    if (trail) {
        Dots dots(c, ink.trail, 1.5f);
        for (const V3& p : *trail)
            if (s.front(p) == front) dots.add(s(p));
    }
    if (s.front(v) != front) return;
    c.dot(s(v.x, v.y, 0), ink.ground, 2);
    c.dashed(s(v.x, v.y, 0), s(v), ink.ground);
}

/* Draws a scene back to front: what is behind, the axes, what is in front. */
template <class Body>
void layered(Canvas& c, const Scene& s, const V3& v, const std::vector<V3>* trail, Body body)
{
    for (const bool front : { false, true }) {
        shadow(c, s, v, trail, front);
        if (s.front(v) == front) body();
        if (!front) axes(c, s);
    }
}

/* A rotation as three arrows along its turned axes. */
void triad(Canvas& c, const Scene& s, const V3& at, const Quat& q, double len)
{
    const V3 units[3] = { { len, 0, 0 }, { 0, len, 0 }, { 0, 0, len } };
    for (int i = 0; i < 3; i++) {
        const V3 d = rotate(q, units[i]);
        c.arrow(s(at), s(at.x + d.x, at.y + d.y, at.z + d.z), axis_colour(i), 2.5f);
    }
}

/* A turn about an axis through the origin: an arc of radius rho sweeping sweep radians the
   way the turn goes by the right hand rule, with a head at its end. */
void curl(Canvas& c, const Scene& s, const V3& axis, double rho, double sweep, Rml::Colourb colour)
{
    const V3 up = std::fabs(axis.z) < 0.9 ? V3{ 0, 0, 1 } : V3{ 1, 0, 0 };
    const V3 side = cross(up, axis), a = side * (1 / length(side)), b = cross(axis, a);
    const int steps = 32;
    Rml::Vector2f last = s(a * rho);
    for (int i = 1; i <= steps; i++) {
        const double t = sweep * i / steps;
        const Rml::Vector2f at = s((a * std::cos(t) + b * std::sin(t)) * rho);
        if (i < steps) c.line(last, at, colour, 2);
        else c.arrow(last, at, colour, 2);
        last = at;
    }
}

/* ------------------------------------------------------------------ the element */

/* A picture pans and zooms. */
bool is_picture(Visual v)
{
    return v == Visual::Image || v == Visual::Video;
}

bool is_scene(Visual v)
{
    switch (v) {
    case Visual::Vec3: case Visual::Quat: case Visual::Transform: case Visual::Twist:
    case Visual::Vec3s: case Visual::Transforms:
        return true;
    default:
        return false;
    }
}

bool is_html(Visual v)
{
    switch (v) {
    case Visual::None: case Visual::Text: case Visual::Time: case Visual::Duration:
    case Visual::Hex: case Visual::Chips: case Visual::Kv: case Visual::Cells:
    case Visual::Table: case Visual::Matrix: case Visual::Multi:
        return true;
    default:
        return false;
    }
}

/* A table wants the box its columns and rows fill. Past TABLE_ROWS it asks for that
   height and scrolls inside. */
constexpr double TABLE_COL = 90, TABLE_ROW = 20;
constexpr size_t TABLE_ROWS = 12;

Tile table_tile(size_t cols, size_t rows)
{
    const size_t shown = std::max<size_t>(1, std::min(rows, TABLE_ROWS));
    return Tile{ std::max<size_t>(1, cols) * TABLE_COL / ((shown + 1) * TABLE_ROW), 1 };
}

class VizElement : public Rml::Element, public Rml::EventListener {
public:
    explicit VizElement(const Rml::String& tag) : Rml::Element(tag)
    {
        for (const Rml::EventId id : GESTURES) AddEventListener(id, this);
    }

    ~VizElement() override
    {
        for (const Rml::EventId id : GESTURES) RemoveEventListener(id, this);
    }

    /* A scene's or a picture's gestures, gathered here and taken on the next draw. The
       wheel over either zooms it and never scrolls the pane. */
    void ProcessEvent(Rml::Event& event) override
    {
        if (!is_scene(visual_) && !is_picture(visual_)) return;
        const Rml::Vector2f at(event.GetParameter<float>("mouse_x", 0.f), event.GetParameter<float>("mouse_y", 0.f));
        switch (event.GetId()) {
        case Rml::EventId::Dragstart:
            last_ = at;
            break;
        case Rml::EventId::Drag:
            turn_ += at - last_;
            last_ = at;
            break;
        case Rml::EventId::Mousescroll:
            wheel_ += event.GetParameter<float>("wheel_delta_y", 0.f);
            wheel_at_ = at;
            event.StopPropagation();
            break;
        case Rml::EventId::Dblclick:
            reset_ = true;
            break;
        default:
            break;
        }
    }

protected:
    void OnUpdate() override
    {
        Rml::Element::OnUpdate();
        const std::string path = GetAttribute<Rml::String>("path", "");
        topic_ = GetAttribute<Rml::String>("topic", "");
        found_ = -2;
        Visual visual = Visual::None;
        const Capture::WatchView* watch = source ? source->view(topic_) : nullptr;
        if (watch) {
            const Nodes& nodes = source->whole(topic_);
            found_ = find_node(nodes, *watch, path);
            visual = visual_at(*watch, nodes, found_);
            if (found_ == -1) {
                root_.kind     = ValueNode::Struct;
                root_.std_name = watch->root_std;
                root_.depth    = -1;
            }
        }
        if (path != path_ || visual != visual_ || !built_) build(path, visual);
        if (found_ >= -1) fill(source->whole(topic_));
    }

    void OnRender() override
    {
        Rml::Element::OnRender();
        if (!source || found_ < -1 || is_html(visual_)) return;
        const Nodes& nodes = source->whole(topic_);
        if (found_ >= (int)nodes.size()) return;

        const double now = Capture::now_s();
        dt_ = last_frame_ > 0 ? std::min(now - last_frame_, 0.5) : 1.0;
        last_frame_ = now;

        Canvas c(*this, shapes_);
        if (c.w() < 1 || c.h() < 1) return;
        read_ink(*this);

        const ValueNode& node = found_ < 0 ? root_ : nodes[found_];
        switch (visual_) {
        case Visual::Plot:       plot(c, node); break;
        case Visual::State:      state(c, node); break;
        case Visual::Image:      picture(c, nodes); break;
        case Visual::Video:      video(c); break;
        case Visual::Bars:       bars(c, nodes, node); break;
        case Visual::Vec2:       vec2(c, nodes); break;
        case Visual::Vec3:       vec3(c, nodes); break;
        case Visual::Quat:       quaternion(c, nodes); break;
        case Visual::Transform:  transform(c, nodes); break;
        case Visual::Twist:      twist(c, nodes); break;
        case Visual::Geo:        geo(c, nodes); break;
        case Visual::Color:      colour(c, nodes); break;
        case Visual::Rect:       rects(c, nodes, false); break;
        case Visual::Joints:     joints(c, nodes); break;
        case Visual::Vec2s:      vec2s(c, nodes); break;
        case Visual::Vec3s:      vec3s(c, nodes); break;
        case Visual::Transforms: transforms(c, nodes); break;
        case Visual::Geos:       geos(c, nodes); break;
        case Visual::Rects:      rects(c, nodes, true); break;
        case Visual::Colors:     colours(c, nodes); break;
        default: break;
        }
        c.render();
        turn_  = Rml::Vector2f(0, 0);
        wheel_ = 0;
        reset_ = false;
    }

private:
    static constexpr Rml::EventId GESTURES[] = { Rml::EventId::Dragstart, Rml::EventId::Drag,
                                                 Rml::EventId::Mousescroll, Rml::EventId::Dblclick };

    /* ---------------------------------------------------------------- building */

    void build(const std::string& path, Visual visual)
    {
        path_    = path;
        visual_  = visual;
        built_   = true;
        html_key_.assign(1, '\x01');   /* no key matches, so the first fill builds */
        slots_.clear();
        texts_.clear();
        lamps_.clear();
        lamp_states_.clear();
        seen_ = eased_ = false;
        reach_[0] = reach_[1] = Reach();
        cam_ = Camera();
        frame_ = FrameBounds();
        rect_ = RectBounds();
        scale_[0] = scale_[1] = scale_[2] = Reach();
        texture_ = Rml::CallbackTexture();
        pixels_.reset();
        frame_seq_ = ~0ull;
        decoder_.reset();
        last_frame_ = 0;

        SetClass("html", is_html(visual));
        SetClass("orbit", is_scene(visual));
        SetClass("pans", is_picture(visual));
        view_frame_ = Rml::Vector2f(0, 0);   /* the next picture starts fitted */
        if (!is_html(visual)) SetInnerRML("");
        if (visual == Visual::None) html("none", "<span class=\"faint\">waiting for a value</span>");
    }

    /* Replaces the children when the key moves, and collects the elements whose text fill
       writes (class slot) and the lamps it colours. */
    void html(const std::string& key, const std::string& rml)
    {
        if (key == html_key_) return;
        html_key_ = key;
        SetInnerRML(rml);
        Rml::ElementList found;
        GetElementsByClassName(found, "slot");
        slots_.assign(found.begin(), found.end());
        texts_.assign(slots_.size(), std::string(1, '\x01'));
        paths_.assign(slots_.size(), std::string(1, '\x01'));
        found.clear();
        GetElementsByClassName(found, "lamp");
        lamps_.assign(found.begin(), found.end());
        lamp_states_.assign(lamps_.size(), -1);
    }

    void put(size_t slot, const std::string& text)
    {
        if (slot >= slots_.size() || texts_[slot] == text) return;
        texts_[slot] = text;
        slots_[slot]->SetInnerRML(Rml::StringUtilities::EncodeRml(text));
    }

    /* A slot that shows one field, which a right click copies whole, not as shortened. */
    void put(size_t slot, const std::string& text, const std::string& path)
    {
        put(slot, text);
        if (slot >= slots_.size() || paths_[slot] == path) return;
        paths_[slot] = path;
        slots_[slot]->SetAttribute("cell-path", path);
    }

    void fill(const Nodes& nodes)
    {
        const ValueNode& node = found_ < 0 ? root_ : nodes[found_];
        const std::vector<int> children = children_of(nodes, found_);
        const std::string count = std::to_string(children.size());
        switch (visual_) {
        case Visual::Text:
            html("text", "<div class=\"big text mono slot\"></div>");
            put(0, value_text(node));
            break;
        case Visual::Time:
        case Visual::Duration:
            html("big", "<div class=\"big mono slot\"></div>");
            put(0, visual_ == Visual::Time ? format_timestamp(node.integer) : format_span(node.integer));
            break;
        case Visual::Hex: {
            html("hex", "<div class=\"hex mono slot\"></div>");
            std::string out;
            const std::string& head = node.text;
            for (size_t row = 0; row * 16 < head.size() && row < 4; row++) {
                for (size_t i = row * 16; i < head.size() && i < row * 16 + 16; i++) {
                    char byte[4];
                    std::snprintf(byte, sizeof byte, "%02x ", (unsigned char)head[i]);
                    out += byte;
                }
                out += "\n";
            }
            put(0, out + (node.count > head.size() ? "... " : "") + format_bytes(node.count));
            break;
        }
        case Visual::Chips: {
            std::string rml = "<div class=\"chips\">";
            for (size_t i = 0; i < children.size(); i++) rml += "<span class=\"chip mono slot\"></span>";
            if (children.empty()) rml += "<span class=\"faint\">empty</span>";
            html("chips:" + count, rml + "</div>");
            for (size_t i = 0; i < children.size(); i++) put(i, human(nodes[children[i]]), nodes[children[i]].path);
            break;
        }
        case Visual::Kv: {
            std::string rml = "<div class=\"pairs\">";
            for (size_t i = 0; i < children.size(); i++)
                rml += "<div class=\"pair\"><span class=\"pair-key slot\"></span><span class=\"pair-value slot\"></span></div>";
            html("kv:" + count, rml + "</div>");
            for (size_t i = 0; i < children.size(); i++) {
                put(i * 2, nodes[children[i]].name);
                put(i * 2 + 1, brief(nodes, children[i]), nodes[children[i]].path);
            }
            break;
        }
        case Visual::Cells: {
            std::string rml = "<div class=\"cells\">";
            for (size_t i = 0; i < children.size(); i++)
                rml += "<span class=\"cell\"><span class=\"lamp\"></span><span class=\"idx mono\">" + std::to_string(i) + "</span></span>";
            html("cells:" + count, rml + "</div>");
            for (size_t i = 0; i < children.size() && i < lamps_.size(); i++) {
                const int on = nodes[children[i]].number != 0 ? 1 : 0;
                if (lamp_states_[i] == on) continue;
                lamp_states_[i] = on;
                lamps_[i]->SetClass("on", on != 0);
            }
            break;
        }
        case Visual::Matrix: {
            const size_t n = (size_t)std::lround(std::sqrt((double)children.size()));
            std::string rml = "<div class=\"matrix mono\">";
            for (size_t r = 0; r < n; r++) {
                rml += "<div class=\"matrix-row\">";
                for (size_t col = 0; col < n; col++) rml += "<span class=\"slot\"></span>";
                rml += "</div>";
            }
            html("matrix:" + count, rml + "</div>");
            for (size_t i = 0; i < children.size() && i < n * n; i++)
                put(i, value_text(nodes[children[i]]), nodes[children[i]].path);
            break;
        }
        case Visual::Table: {
            /* a column per member of the element, a row per element */
            const std::vector<int> columns = children.empty() ? std::vector<int>() : children_of(nodes, children[0]);
            std::string key = "table:" + count, rml = "<div class=\"scroll-table\"><table><tr>";
            for (const int col : columns) {
                key += ":" + nodes[col].name;
                rml += "<th>" + Rml::StringUtilities::EncodeRml(nodes[col].name) + "</th>";
            }
            rml += "</tr>";
            for (size_t r = 0; r < children.size(); r++) {
                rml += "<tr>";
                for (size_t col = 0; col < columns.size(); col++) rml += "<td class=\"mono slot\"></td>";
                rml += "</tr>";
            }
            html(key, rml + "</table></div>");
            size_t slot = 0;
            for (const int row : children)
                for (const int cell : children_of(nodes, row)) put(slot++, brief(nodes, cell), nodes[cell].path);
            break;
        }
        case Visual::Multi: {
            /* a small copy of the element's own visual per element, each with its own bounds */
            std::string rml = "<div class=\"multi\">";
            for (size_t i = 0; i < children.size(); i++)
                rml += "<div class=\"multi-cell\"><span class=\"idx mono\">" + std::to_string(i) +
                       "</span><viz topic=\"" + Rml::StringUtilities::EncodeRml(topic_) + "\" path=\"" +
                       Rml::StringUtilities::EncodeRml(nodes[children[i]].path) + "\"></viz></div>";
            html("multi:" + count, rml + "</div>");
            break;
        }
        default:
            break;
        }
    }

    /* ---------------------------------------------------------------- traces */

    const std::deque<Capture::TracePoint>& trace(const std::string& path) const
    {
        static const std::deque<Capture::TracePoint> none;
        const std::deque<Capture::TracePoint>* t = source->trace_of(topic_, path);
        return t ? *t : none;
    }

    /* The points of up to three traces that arrived together, oldest first. Each message
       adds to all of them, so they line up from the newest end. */
    std::vector<V3> trail(const std::string& x, const std::string& y, const std::string& z = {}) const
    {
        const auto& tx = trace(x);
        const auto& ty = trace(y);
        const auto& tz = z.empty() ? tx : trace(z);
        const size_t n = std::min({ tx.size(), ty.size(), tz.size() });
        std::vector<V3> out(n);
        for (size_t i = 0; i < n; i++) {
            out[i].x = tx[tx.size() - n + i].v;
            out[i].y = ty[ty.size() - n + i].v;
            out[i].z = z.empty() ? 0 : tz[tz.size() - n + i].v;
        }
        return out;
    }

    /* The camera, moved by the gestures since the last draw and left where they put it.
       Half a degree per dp dragged, down looks from higher, up to straight down or up. */
    Camera& camera()
    {
        const float ratio = GetContext() ? GetContext()->GetDensityIndependentPixelRatio() : 1.f;
        cam_.a -= turn_.x / ratio * 0.5;
        cam_.e  = std::max(-90.0, std::min(90.0, cam_.e + turn_.y / ratio * 0.5));
        if (wheel_ != 0) cam_.zoom = std::max(0.25, std::min(8.0, cam_.zoom * std::pow(1.15, -wheel_)));
        if (reset_) cam_ = Camera();
        return cam_;
    }

    /* ---------------------------------------------------------------- over time */

    /* The value over the last ten seconds against a grid of round steps. The range is
       every value seen since the card opened plus a margin, eased so it never jumps. */
    void plot(Canvas& c, const ValueNode& node)
    {
        const auto& points = trace(path_);
        const double now = Capture::now_s();
        for (const Capture::TracePoint& p : points) {
            if (!seen_) { lo_seen_ = hi_seen_ = p.v; seen_ = true; }
            lo_seen_ = std::min(lo_seen_, p.v);
            hi_seen_ = std::max(hi_seen_, p.v);
        }
        if (!seen_) { lo_seen_ = hi_seen_ = node.number; seen_ = true; }
        double lo = lo_seen_, hi = hi_seen_;
        if (hi - lo < 1e-9) { lo -= 1; hi += 1; }
        else { const double m = (hi - lo) * 0.1; lo -= m; hi += m; }
        ease_range(lo, hi);

        const float top = c.dp(TOP), bottom = c.h() - c.dp(BOT);
        const float x0 = c.dp(60), x1 = c.w() - c.dp(10);
        auto X = [&](double t) { return (float)(x0 + (x1 - x0) * (1 - (now - t) / PLOT_SECONDS)); };
        auto Y = [&](double v) { return (float)(top + (bottom - top) * (1 - (v - lo_) / (hi_ - lo_))); };

        time_grid(c, X, now, top, bottom);
        value_grid(c, x0, x1, lo_, hi_, Y, node.is_float);
        /* The line, cut to the plot so the tail slides off the left edge and a new extreme
           the range has not eased out to yet stays inside. */
        Columns columns;
        for (const Capture::TracePoint& p : points) columns.add({ X(p.t), Y(p.v) });
        const std::vector<Rml::Vector2f>& line = columns.points();
        for (size_t i = 1; i < line.size(); i++) {
            float ax = line[i - 1].x, ay = line[i - 1].y, bx = line[i].x, by = line[i].y;
            if (clip(ax, ay, bx, by, x0, top, x1, bottom)) c.line(ax, ay, bx, by, ink.accent, 1.5f);
        }
        if (!points.empty()) {
            const float x = X(points.back().t), y = Y(points.back().v);
            if (y >= top && y <= bottom) c.dot(x, y, ink.accent);
        }
        corner(c, c.all(), value_text(node));
    }

    void ease_range(double lo, double hi)
    {
        if (!eased_) { lo_ = lo; hi_ = hi; eased_ = true; }
        lo_ = ease(lo_, lo, dt_);
        hi_ = ease(hi_, hi, dt_);
    }

    /* A bool or an enum over ten seconds: a band per run of equal values on whole pixels.
       A pixel shows the first run to reach it, so a fast toggle costs the width. */
    void state(Canvas& c, const ValueNode& node)
    {
        const auto& points = trace(path_);
        const double now = Capture::now_s();
        const float  left = c.dp(10), right = c.w() - c.dp(10);
        auto X = [&](double t) {
            return std::max(left, (float)(left + (right - left) * (1 - (now - t) / PLOT_SECONDS)));
        };
        auto colour = [&](double v) {
            if (node.kind == ValueNode::Bool) return v != 0 ? ink.green : ink.border;
            return palette((long long)v);
        };
        const float top = c.dp(TOP) + c.dp(4), bottom = c.h() - c.dp(BOT);
        float drawn = -1;
        for (size_t i = 0; i < points.size();) {
            size_t j = i;
            while (j + 1 < points.size() && points[j + 1].v == points[i].v) j++;
            const float a = std::max(drawn, std::floor(X(points[i].t)));
            const float b = std::ceil(X(j + 1 < points.size() ? points[j + 1].t : now));
            if (b > a) {
                c.rect(a, top, b - a, bottom - top, colour(points[i].v));
                drawn = b;
            }
            i = j + 1;
        }
        time_grid(c, X, now, c.dp(TOP) + c.dp(2), bottom);
        corner(c, c.all(), value_text(node));
    }

    /* ---------------------------------------------------------------- arrays of numbers */

    /* Bars from zero, the range grown from every value seen, labelled while few. */
    void bars(Canvas& c, const Nodes& nodes, const ValueNode& node)
    {
        const std::vector<int> items = children_of(nodes, found_);
        const size_t n = items.size();
        if (!seen_) { lo_seen_ = hi_seen_ = 0; seen_ = true; }
        for (const int i : items) {
            lo_seen_ = std::min(lo_seen_, nodes[i].number);
            hi_seen_ = std::max(hi_seen_, nodes[i].number);
        }
        ease_range(lo_seen_, hi_seen_);
        const double lo = lo_, hi = hi_ - lo_ > 1e-12 ? hi_ : lo_ + 1;
        const float top = c.dp(TOP), bottom = c.h() - c.dp(BOT), x0 = c.dp(60), x1 = c.w() - c.dp(10);
        auto Y = [&](double v) { return (float)(top + (bottom - top) * (1 - (v - lo) / (hi - lo))); };
        const bool is_float = n && nodes[items[0]].is_float;
        value_grid(c, x0, x1, lo, hi, Y, is_float);
        const float gap = n <= LABELLED ? 0.3f : 0, width = n ? (x1 - x0) / n : 0;
        const float base = Y(std::max(lo_seen_, std::min(0.0, hi_seen_)));
        /* Bars sharing a pixel column draw as one, spanning their extremes and the base. */
        for (size_t i = 0; i < n;) {
            /* whole pixel edges, so dense neighbours meet without a seam */
            const float x = std::floor(x0 + i * width + width * gap / 2);
            float right = std::floor(x0 + i * width + width * (1 - gap / 2));
            float y_top = std::min(base, Y(nodes[items[i]].number)), y_bot = std::max(base, Y(nodes[items[i]].number));
            const size_t first = i;
            for (i++; i < n && std::floor(x0 + i * width + width * gap / 2) == x; i++) {
                const float y = Y(nodes[items[i]].number);
                y_top = std::min(y_top, y);
                y_bot = std::max(y_bot, y);
                right = std::floor(x0 + i * width + width * (1 - gap / 2));
            }
            c.rect(x, y_top, std::max(1.f, right - x), std::max(1.f, y_bot - y_top), ink.accent);
            if (n <= LABELLED) c.text(std::to_string(first), x + width * (1 - gap) / 2, c.h() - c.dp(5), Canvas::Center, ink.faint);
        }
        (void)node;
        foot(c, c.all(), {}, format_number((double)n) + " values");
    }

    /* ---------------------------------------------------------------- planes */

    /* The box of a 2D scene fitted to an area, with the grid and both axes. */
    struct Plane {
        double k = 1, cx = 0, cy = 0;
        Rml::Vector2f operator()(double x, double y) const { return { (float)(cx + x * k), (float)(cy - y * k) }; }
    };

    Plane plane(Canvas& c, const Area& a, double r)
    {
        Plane p;
        p.k  = std::min(a.w - c.dp(16), a.h - c.dp(TOP + BOT + 4)) / (2 * r);
        p.cx = a.x + a.w / 2;
        p.cy = a.y + c.dp(TOP) + (a.h - c.dp(TOP + BOT)) / 2;
        const double step = nice(r / 2);
        for (double m = std::ceil(-r / step) * step; m <= r + 1e-9; m += step) {
            c.line(p(m, -r), p(m, r), ink.grid);
            c.line(p(-r, m), p(r, m), ink.grid);
        }
        c.line(p(-r, 0), p(r, 0), axis_colour(0));
        c.line(p(0, -r), p(0, r), axis_colour(1));
        return p;
    }

    void vec2(Canvas& c, const Nodes& nodes)
    {
        const V3 v = vec(nodes, found_);
        const double r = reach_[0].grow({ v.x, v.y }, dt_);
        const Plane p = plane(c, c.all(), r);
        Dots dots(c, ink.trail, 1.5f);
        for (const V3& t : trail(child_path(path_, "x"), child_path(path_, "y"))) dots.add(p(t.x, t.y));
        c.arrow(p(0, 0), p(v.x, v.y), ink.accent, 2.5f);
        foot(c, c.all(), "x " + f3(v.x) + "  y " + f3(v.y));
    }

    void vec2s(Canvas& c, const Nodes& nodes)
    {
        const std::vector<int> items = children_of(nodes, found_);
        std::vector<V3> v;
        for (const int i : items) v.push_back(vec(nodes, i));
        for (const V3& e : v) reach_[0].seen = std::max({ reach_[0].seen, std::fabs(e.x), std::fabs(e.y) });
        const Plane p = plane(c, c.all(), reach_[0].grow({}, dt_));
        for (size_t i = 0; i < v.size(); i++) {
            c.arrow(p(0, 0), p(v[i].x, v[i].y), palette((long long)i), 2.5f);
            const Rml::Vector2f tip = p(v[i].x, v[i].y);
            c.text(std::to_string(i), tip.x + c.dp(6), tip.y - c.dp(4), Canvas::Left, ink.dim);
        }
        foot(c, c.all(), format_number((double)v.size()) + " vectors");
    }

    /* ---------------------------------------------------------------- scenes */

    /* An arrow from the origin in a scene, its foot, drop line and trail. */
    void vec3(Canvas& c, const Nodes& nodes)
    {
        const V3 v = vec(nodes, found_);
        const double r = reach_[0].grow({ v.x, v.y, v.z }, dt_);
        const Scene s = scene(c, c.all(), r, camera());
        const std::vector<V3> t = trail(child_path(path_, "x"), child_path(path_, "y"), child_path(path_, "z"));
        layered(c, s, v, &t, [&] { c.arrow(s(0, 0, 0), s(v), ink.accent, 2.5f); });
        /* a narrow scene has no room for the components */
        if (c.w() >= c.dp(260)) foot(c, c.all(), "x " + f3(v.x) + "  y " + f3(v.y) + "  z " + f3(v.z));
    }

    void quaternion(Canvas& c, const Nodes& nodes)
    {
        const Quat q = quat(nodes, found_);
        const Scene s = scene(c, c.all(), 1, camera());
        axes(c, s);
        triad(c, s, V3{}, q, 0.9);
        foot(c, c.all(), euler(q));
    }

    void transform(Canvas& c, const Nodes& nodes)
    {
        const int ti = kid(nodes, found_, "translation"), ri = kid(nodes, found_, "rotation");
        const V3 t = ti >= 0 ? vec(nodes, ti) : V3{};
        const Quat q = ri >= 0 ? quat(nodes, ri) : Quat{};
        const double r = reach_[0].grow({ t.x, t.y, t.z }, dt_);
        const Scene s = scene(c, c.all(), r, camera());
        const std::string base = child_path(path_, "translation");
        const std::vector<V3> tr = trail(base + ".x", base + ".y", base + ".z");
        layered(c, s, t, &tr, [&] {
            c.line(s(0, 0, 0), s(t), ink.faint);
            triad(c, s, t, q, 2 * r * 0.15);
        });
        foot(c, c.all(), "x " + f3(t.x) + "  y " + f3(t.y) + "  z " + f3(t.z) + "  " + euler(q));
    }

    /* A twist in the moving body's frame: velocity arrow, turn curl, the path while it
       holds, and the turn axis. The path stops short of a second full turn. */
    void twist(Canvas& c, const Nodes& nodes)
    {
        const int li = kid(nodes, found_, "linear"), ai = kid(nodes, found_, "angular");
        const V3 v = li >= 0 ? vec(nodes, li) : V3{}, w = ai >= 0 ? vec(nodes, ai) : V3{};
        const double spin = length(w);
        const bool turning = spin > 1e-9;
        const double ahead = turning ? std::min(TWIST_AHEAD, 6.28318530717959 / spin) : TWIST_AHEAD;
        /* a screw, stepped with each step's travel turned by its middle */
        std::vector<V3> path{ V3{} };
        Quat q;
        const int steps = 64;
        const double dt = ahead / steps;
        for (int i = 0; i < steps; i++) {
            path.push_back(path.back() + rotate(q * turned(w, dt / 2), v * dt));
            q = q * turned(w, dt);
        }
        /* The view holds the path, and the turn centre while it is near enough to read as a
           turn: a far one would shrink the scene for good, since the reach only grows. */
        const V3 centre = turning ? cross(w, v) * (1 / (spin * spin)) : V3{};
        double extent = length(v);
        for (const V3& p : path) extent = std::max(extent, length(p));
        auto hold = [this](const V3& p) { reach_[0].seen = std::max({ reach_[0].seen, std::fabs(p.x), std::fabs(p.y), std::fabs(p.z) }); };
        for (const V3& p : path) hold(p);
        if (turning && length(centre) <= 2 * extent) hold(centre);
        const double r = reach_[0].grow({ v.x, v.y, v.z }, dt_);
        const Scene s = scene(c, c.all(), r, camera());

        if (turning) {
            const V3 axis = w * (1 / spin);
            /* a turn centre this far out is a straight run, and its line would leave the view */
            if (length(centre) < 20 * r) {
                c.dashed(s(centre - axis * r), s(centre + axis * r), ink.dim);
                c.dot(s(centre), ink.dim, 3);
            }
        }
        /* its own colour, since a forward run lies along the x axis */
        for (size_t i = 1; i < path.size(); i++) c.line(s(path[i - 1]), s(path[i]), ink.purple, 1.5f);
        c.dot(s(path.back()), ink.purple, 2.5f);
        /* the curl sweeps most of a turn at the fastest turn seen */
        const double fastest = reach_[1].grow({ spin }, dt_, 1.0);
        if (turning) curl(c, s, w * (1 / spin), 0.15 * r, 5 * spin / fastest, ink.amber);
        layered(c, s, v, nullptr, [&] { c.arrow(s(0, 0, 0), s(v), ink.accent, 2.5f); });
        foot(c, c.all(), "v " + f3(length(v)) + " m/s  \xCF\x89 " + f3(spin) + " rad/s", "next " + print("%.1f", ahead) + " s");
    }

    void vec3s(Canvas& c, const Nodes& nodes)
    {
        const std::vector<int> items = children_of(nodes, found_);
        std::vector<V3> v;
        for (const int i : items) v.push_back(vec(nodes, i));
        for (const V3& e : v) reach_[0].seen = std::max({ reach_[0].seen, std::fabs(e.x), std::fabs(e.y), std::fabs(e.z) });
        const Scene s = scene(c, c.all(), reach_[0].grow({}, dt_), camera());
        std::vector<size_t> order(v.size());
        for (size_t i = 0; i < order.size(); i++) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return s.depth(v[a]) < s.depth(v[b]); });
        bool axes_drawn = false;
        for (const size_t i : order) {
            if (!axes_drawn && s.front(v[i])) { axes(c, s); axes_drawn = true; }
            shadow(c, s, v[i], nullptr, s.front(v[i]));
            c.arrow(s(0, 0, 0), s(v[i]), palette((long long)i), 2.5f);
            const Rml::Vector2f tip = s(v[i]);
            c.text(std::to_string(i), tip.x + c.dp(6), tip.y - c.dp(4), Canvas::Left, ink.dim);
        }
        if (!axes_drawn) axes(c, s);
        foot(c, c.all(), format_number((double)v.size()) + " vectors");
    }

    /* A chain: consecutive transforms joined in the air, a small triad at each. */
    void transforms(Canvas& c, const Nodes& nodes)
    {
        const std::vector<int> items = children_of(nodes, found_);
        std::vector<V3> t;
        std::vector<Quat> q;
        for (const int i : items) {
            const int ti = kid(nodes, i, "translation"), ri = kid(nodes, i, "rotation");
            t.push_back(ti >= 0 ? vec(nodes, ti) : V3{});
            q.push_back(ri >= 0 ? quat(nodes, ri) : Quat{});
        }
        for (const V3& e : t) reach_[0].seen = std::max({ reach_[0].seen, std::fabs(e.x), std::fabs(e.y), std::fabs(e.z) });
        const double r = reach_[0].grow({}, dt_);
        const Scene s = scene(c, c.all(), r, camera());
        std::vector<size_t> order(t.size());
        for (size_t i = 0; i < order.size(); i++) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return s.depth(t[a]) < s.depth(t[b]); });
        bool axes_drawn = false;
        for (const size_t i : order) {
            if (!axes_drawn && s.front(t[i])) { axes(c, s); axes_drawn = true; }
            shadow(c, s, t[i], nullptr, s.front(t[i]));
            if (i) c.line(s(t[i - 1]), s(t[i]), ink.dim, 1.5f);
            triad(c, s, t[i], q[i], 2 * r * 0.08);
            const Rml::Vector2f at = s(t[i]);
            c.text(std::to_string(i), at.x + c.dp(8), at.y - c.dp(6), Canvas::Left, ink.dim);
        }
        if (!axes_drawn) axes(c, s);
        foot(c, c.all(), format_number((double)t.size()) + " transforms");
    }

    /* ---------------------------------------------------------------- maps */

    /* A metric north up plane over every point seen, eased, with a grid and a north mark.
       Draw is handed the projection and the clip rectangle. */
    template <class Draw>
    void map(Canvas& c, const std::vector<V3>& points, Draw draw)
    {
        /* x is latitude, y longitude */
        for (const V3& p : points) {
            if (!frame_.init) { frame_.lat0 = frame_.lat1 = p.x; frame_.lon0 = frame_.lon1 = p.y; frame_.init = true; }
            frame_.lat0 = std::min(frame_.lat0, p.x); frame_.lat1 = std::max(frame_.lat1, p.x);
            frame_.lon0 = std::min(frame_.lon0, p.y); frame_.lon1 = std::max(frame_.lon1, p.y);
        }
        if (!frame_.init) return;
        const double lat = points.empty() ? frame_.lat0 : points.back().x;
        const double m_lat = 111320, m_lon = 111320 * std::cos(lat * 3.14159265358979 / 180);
        const double span_m = std::max({ (frame_.lon1 - frame_.lon0) * m_lon, (frame_.lat1 - frame_.lat0) * m_lat, 20.0 }) * 1.3;
        const double clon = (frame_.lon0 + frame_.lon1) / 2, clat = (frame_.lat0 + frame_.lat1) / 2;
        if (!frame_.eased) { frame_.span = span_m; frame_.clon = clon; frame_.clat = clat; frame_.eased = true; }
        frame_.span = ease(frame_.span, span_m, dt_);
        frame_.clon = ease(frame_.clon, clon, dt_);
        frame_.clat = ease(frame_.clat, clat, dt_);

        const float left = c.dp(10), right = c.w() - c.dp(10), top = c.dp(TOP), bottom = c.h() - c.dp(BOT);
        const double k = std::min(right - left, bottom - top) / frame_.span;
        const double cx = c.w() / 2, cy = top + (bottom - top) / 2;
        auto P = [&](double la, double lo) {
            return Rml::Vector2f((float)(cx + (lo - frame_.clon) * m_lon * k), (float)(cy - (la - frame_.clat) * m_lat * k));
        };
        const double step = nice(frame_.span / 4);
        for (double m = -std::ceil(c.w() / k / step) * step; m <= c.w() / k; m += step) {
            const float x = (float)(cx + m * k);
            if (x >= left && x <= right) c.line(x, top, x, bottom, ink.grid);
        }
        for (double m = -std::ceil(c.h() / k / step) * step; m <= c.h() / k; m += step) {
            const float y = (float)(cy + m * k);
            if (y >= top && y <= bottom) c.line(left, y, right, y, ink.grid);
        }
        draw(P, left, top, right, bottom);
        c.text("N", c.w() - c.dp(14), top + c.dp(10), Canvas::Center, ink.dim);
        c.arrow({ c.w() - c.dp(14), top + c.dp(30) }, { c.w() - c.dp(14), top + c.dp(15) }, ink.dim, 1);
    }

    void geo(Canvas& c, const Nodes& nodes)
    {
        const V3 now{ number(nodes, found_, "lat"), number(nodes, found_, "lon"), number(nodes, found_, "alt") };
        std::vector<V3> points = trail(child_path(path_, "lat"), child_path(path_, "lon"));
        if (points.empty()) points.push_back(now);
        map(c, points, [&](auto P, float left, float top, float right, float bottom) {
            /* a point within half a pixel of the last one drawn adds nothing to the path */
            Rml::Vector2f from = points.empty() ? Rml::Vector2f() : P(points[0].x, points[0].y);
            for (size_t i = 1; i < points.size(); i++) {
                Rml::Vector2f a = from, b = P(points[i].x, points[i].y);
                if (i + 1 < points.size() && std::fabs(b.x - a.x) < 0.5f && std::fabs(b.y - a.y) < 0.5f) continue;
                from = b;
                if (clip(a.x, a.y, b.x, b.y, left, top, right, bottom)) c.line(a, b, ink.accent, 1.5f);
            }
            const Rml::Vector2f at = P(now.x, now.y);
            if (inside(at, left, top, right, bottom)) c.dot(at, ink.accent, 4);
        });
        foot(c, c.all(), "lat " + print("%.6f", now.x) + "  lon " + print("%.6f", now.y) + "  alt " + print("%.1f", now.z) + " m");
    }

    void geos(Canvas& c, const Nodes& nodes)
    {
        std::vector<V3> points;
        for (const int i : children_of(nodes, found_)) points.push_back(V3{ number(nodes, i, "lat"), number(nodes, i, "lon"), 0 });
        map(c, points, [&](auto P, float left, float top, float right, float bottom) {
            for (size_t i = 0; i < points.size(); i++) {
                const Rml::Vector2f at = P(points[i].x, points[i].y);
                if (!inside(at, left, top, right, bottom)) continue;
                c.dot(at, palette((long long)i), 4);
                c.text(std::to_string(i), at.x + c.dp(7), at.y - c.dp(5), Canvas::Left, ink.dim);
            }
        });
        foot(c, c.all(), format_number((double)points.size()) + " points");
    }

    /* ---------------------------------------------------------------- flat things */

    static Rml::Colourb rgba(const Nodes& nodes, int index)
    {
        auto channel = [&](const char* name) { return (Rml::byte)std::max(0.0, std::min(255.0, number(nodes, index, name))); };
        return Rml::Colourb(channel("r"), channel("g"), channel("b"), channel("a"));
    }

    void colour(Canvas& c, const Nodes& nodes)
    {
        const Rml::Colourb col = rgba(nodes, found_);
        c.rect(c.dp(10), c.dp(TOP), c.w() - c.dp(20), c.h() - c.dp(TOP + BOT), col);
        char hex[16];
        std::snprintf(hex, sizeof hex, "#%02x%02x%02x%02x", col.red, col.green, col.blue, col.alpha);
        foot(c, c.all(), hex, std::to_string(col.red) + " " + std::to_string(col.green) + " " +
                              std::to_string(col.blue) + " " + std::to_string(col.alpha));
    }

    void colours(Canvas& c, const Nodes& nodes)
    {
        const std::vector<int> items = children_of(nodes, found_);
        if (items.empty()) return;
        const float width = (c.w() - c.dp(20)) / items.size();
        for (size_t i = 0; i < items.size(); i++) {
            c.rect(c.dp(10) + i * width + 1, c.dp(TOP), width - 2, c.h() - c.dp(TOP + BOT), rgba(nodes, items[i]));
            c.text(std::to_string(i), c.dp(10) + i * width + width / 2, c.h() - c.dp(5), Canvas::Center, ink.faint);
        }
    }

    /* Rects inside a frame that grows to every corner seen and the origin. Only the scale
       eases, so a rect never leaves the frame. The frame's size is at the bottom right. */
    void rects(Canvas& c, const Nodes& nodes, bool many)
    {
        std::vector<int> items;
        if (many) items = children_of(nodes, found_);
        else items.push_back(found_);
        struct R { double x, y, w, h; };
        std::vector<R> list;
        for (const int i : items)
            list.push_back(R{ number(nodes, i, "x"), number(nodes, i, "y"), number(nodes, i, "w"), number(nodes, i, "h") });
        for (const R& r : list) {
            rect_.x0 = std::min({ rect_.x0, r.x, 0.0 });
            rect_.y0 = std::min({ rect_.y0, r.y, 0.0 });
            rect_.x1 = std::max({ rect_.x1, r.x + r.w, 0.0 });
            rect_.y1 = std::max({ rect_.y1, r.y + r.h, 0.0 });
        }
        const double bw = std::max(rect_.x1 - rect_.x0, 1.0), bh = std::max(rect_.y1 - rect_.y0, 1.0);
        const double target = std::min((c.w() - c.dp(20)) / bw, (c.h() - c.dp(TOP + BOT)) / bh);
        rect_.k = rect_.eased ? ease(rect_.k, target, dt_) : target;
        rect_.eased = true;
        const double k = rect_.k;
        const double ox = (c.w() - bw * k) / 2, oy = c.dp(TOP) + (c.h() - c.dp(TOP + BOT) - bh * k) / 2;
        const double step = nice(std::max(bw, bh) / 5);
        for (double m = step; m < bw; m += step) c.line((float)(ox + m * k), (float)oy, (float)(ox + m * k), (float)(oy + bh * k), ink.grid);
        for (double m = step; m < bh; m += step) c.line((float)ox, (float)(oy + m * k), (float)(ox + bw * k), (float)(oy + m * k), ink.grid);
        c.frame((float)ox, (float)oy, (float)(bw * k), (float)(bh * k), ink.faint);
        for (size_t i = 0; i < list.size(); i++) {
            const R& r = list[i];
            const Rml::Colourb col = many ? palette((long long)i) : ink.accent;
            const float x = (float)(ox + (r.x - rect_.x0) * k), y = (float)(oy + (r.y - rect_.y0) * k);
            c.frame(x, y, (float)(r.w * k), (float)(r.h * k), col, 1.5f);
            if (many) c.text(std::to_string(i), x + c.dp(4), y + c.dp(11), Canvas::Left, col);
        }
        const std::string size = print("%.0f", bw) + " x " + print("%.0f", bh);
        if (many) foot(c, c.all(), format_number((double)list.size()) + " rects", size);
        else if (!list.empty())
            foot(c, c.all(), "x " + print("%.0f", list[0].x) + "  y " + print("%.0f", list[0].y) + "  w " +
                             print("%.0f", list[0].w) + "  h " + print("%.0f", list[0].h), size);
    }

    /* One row per joint and a column each for position, velocity and effort: a bar from
       the centre, each column scaled to the largest magnitude it has seen, written above. */
    void joints(Canvas& c, const Nodes& nodes)
    {
        const char* names[3] = { "position", "velocity", "effort" };
        std::vector<int> columns[3];
        size_t rows = 0;
        for (int f = 0; f < 3; f++) {
            const int k = kid(nodes, found_, names[f]);
            if (k >= 0) columns[f] = children_of(nodes, k);
            rows = std::max(rows, columns[f].size());
        }
        const float gutter = c.dp(24), gap = c.dp(10), top = c.dp(TOP) + c.dp(14), bottom = c.h() - c.dp(8);
        const float col_w = (c.w() - gutter - c.dp(8) - 2 * gap) / 3;
        const float row_h = rows ? std::min(c.dp(22), (bottom - top) / rows) : 0, bar_h = std::max(1.f, row_h - c.dp(4));
        for (int f = 0; f < 3; f++) {
            double biggest = 0;
            for (const int i : columns[f]) biggest = std::max(biggest, std::fabs(nodes[i].number));
            scale_[f].seen = std::max(scale_[f].seen, biggest);
            const double m = scale_[f].grow({}, dt_, 1.0);
            const float x = gutter + f * (col_w + gap);
            c.text(std::string(names[f]) + "  \xC2\xB1" + short_number(m), x, c.dp(TOP) + c.dp(6), Canvas::Left, ink.faint);
            for (size_t r = 0; r < columns[f].size(); r++) {
                const float y = top + r * row_h;
                const double v = nodes[columns[f][r]].number;
                c.rect(x, y, col_w, bar_h, ink.panel);
                for (int q = 1; q < 4; q++) c.line(x + col_w * q / 4, y, x + col_w * q / 4, y + bar_h, ink.grid);
                const float mid = x + col_w / 2, len = (float)(std::max(-1.0, std::min(1.0, v / m)) * col_w / 2);
                c.rect(std::min(mid, mid + len), y, std::fabs(len), bar_h, ink.accent);
                c.line(mid, y, mid, y + bar_h, ink.border);
                c.text(f3(v), x + col_w - c.dp(5), y + bar_h / 2 + c.dp(3.5f), Canvas::Right, ink.text);
            }
        }
        for (size_t r = 0; r < rows; r++)
            c.text(std::to_string(r), gutter - c.dp(6), top + r * row_h + bar_h / 2 + c.dp(3.5f), Canvas::Right, ink.faint);
    }

    /* ---------------------------------------------------------------- pictures */

    /* A standard Image, decoded once per message into a texture and fitted inside the
       card, keeping its shape. */
    void picture(Canvas& c, const Nodes& nodes)
    {
        const Capture::WatchView* watch = source->view(topic_);
        if (watch && watch->seq != frame_seq_) {
            frame_seq_ = watch->seq;
            problem_.clear();
            Image image = image_of(nodes, found_, watch->message, &problem_);
            show(image);
        }
        fit(c, c.h() - c.dp(BOT), problem_.empty() ? "waiting for a frame" : problem_);
    }

    /* A VideoFrame stream: every frame since the last draw goes to the decoder, and its
       newest picture is fitted as an Image's is, over the codec and size. A decoder is
       for one stream, so another topic gets a new one. */
    void video(Canvas& c)
    {
        if (!decoder_ || decoder_topic_ != topic_) {
            decoder_       = std::make_unique<VideoDecoder>();
            decoder_topic_ = topic_;
            decoded_seq_   = 0;
            texture_       = Rml::CallbackTexture();
            pixels_.reset();
        }
        bool lost = false;
        std::vector<Capture::Packet> packets = source->take(topic_, path_, lost);
        if (!packets.empty() || lost) decoder_->push(std::move(packets), lost);
        Image image;
        if (decoder_->picture(image, decoded_seq_)) show(image);

        const VideoDecoder::Info info = decoder_->info();
        std::string left = info.codec;
        if (info.width > 0) left += "  " + std::to_string(info.width) + " x " + std::to_string(info.height);
        fit(c, c.h() - c.dp(BOT), info.problem.empty() ? "waiting for a frame" : info.problem);
        foot(c, c.all(), left, texture_ ? info.problem : std::string());
    }

    /* The picture drawn from now on, none when it is invalid. */
    void show(Image& image)
    {
        texture_ = Rml::CallbackTexture();
        pixels_.reset();
        Rml::RenderManager* rm = image.valid() ? GetRenderManager() : nullptr;
        if (!rm) return;
        image_premultiply(image);
        pixels_     = std::make_shared<std::vector<uint8_t>>(std::move(image.rgba));
        frame_size_ = Rml::Vector2i(image.width, image.height);
        auto pixels = pixels_;
        const Rml::Vector2i size = frame_size_;
        texture_ = rm->MakeCallbackTexture([pixels, size](const Rml::CallbackTextureInterface& ti) {
            return ti.GenerateTexture(Rml::Span<const Rml::byte>(pixels->data(), pixels->size()), size);
        });
    }

    /* The picture fitted inside the card above bottom, keeping its shape, or why there is
       none in its middle. The wheel zooms about the pointer until a texel is ZOOM_TEXEL dp,
       a drag pans while zoomed, and a double click or a new frame size fits it again. */
    void fit(Canvas& c, float bottom, const std::string& why)
    {
        const float x0 = c.dp(8), y0 = c.dp(24), w = c.w() - c.dp(16), h = bottom - y0;
        SetClass("zoomed", texture_ && zoom_ > 1);
        if (!texture_ || w < 1 || h < 1) {
            c.text(why, c.w() / 2, c.h() / 2, Canvas::Center, ink.faint, 11);
            return;
        }
        const Rml::Vector2f frame((float)frame_size_.x, (float)frame_size_.y);
        const Rml::Vector2f middle(x0 + w / 2, y0 + h / 2);
        const float k = std::min(w / frame.x, h / frame.y);
        if (reset_ || frame != view_frame_) {
            zoom_       = 1;
            centre_     = frame * 0.5f;
            view_frame_ = frame;
        }
        if (wheel_ != 0) {
            const Rml::Vector2f p     = wheel_at_ - c.origin() - middle;
            const Rml::Vector2f under = centre_ + p / (k * zoom_);
            const float most = std::max(1.f, c.dp(ZOOM_TEXEL) / k);
            zoom_   = std::max(1.f, std::min(most, zoom_ * std::pow(1.15f, -wheel_)));
            centre_ = under - p / (k * zoom_);
        }
        centre_ -= turn_ / (k * zoom_);
        const float s = k * zoom_;
        const Rml::Vector2f seen(std::min(frame.x, w / s), std::min(frame.y, h / s));
        centre_ = Rml::Vector2f(std::max(seen.x / 2, std::min(frame.x - seen.x / 2, centre_.x)),
                                std::max(seen.y / 2, std::min(frame.y - seen.y / 2, centre_.y)));
        const Rml::Vector2f size = seen * s;
        c.picture(std::round(middle.x - size.x / 2), std::round(middle.y - size.y / 2), std::round(size.x),
                  std::round(size.y), texture_, frame_size_, (centre_ - seen * 0.5f) / frame,
                  (centre_ + seen * 0.5f) / frame);
    }

public:
    /* What the card wants from the tiling: the aspect of what it draws, and how much it
       minds another. A picture wants its frame's and letterboxes when it cannot have it. */
    Tile shape() const
    {
        const Nodes* nodes = source && found_ >= -1 ? &source->whole(topic_) : nullptr;
        const size_t children = nodes ? children_of(*nodes, found_).size() : 0;
        switch (visual_) {
        case Visual::Image:
        case Visual::Video:
            return Tile{ frame_size_.x > 0 && frame_size_.y > 0 ? (double)frame_size_.x / frame_size_.y : 4.0 / 3, 4 };
        case Visual::Plot: case Visual::State: case Visual::Bars:
            return Tile{ 3, 1 };
        case Visual::Text: case Visual::Time: case Visual::Duration: case Visual::Hex:
        case Visual::Chips: case Visual::Cells: case Visual::Colors: case Visual::None:
            return Tile{ 3, 0.5 };
        case Visual::Color:
            return Tile{ 1.5, 0.5 };
        case Visual::Vec3: case Visual::Quat: case Visual::Transform: case Visual::Twist:
        case Visual::Vec3s: case Visual::Transforms:
            return Tile{ 1, 1.5 };
        case Visual::Vec2: case Visual::Vec2s: case Visual::Rect: case Visual::Rects:
            return Tile{ 1.3, 1 };
        case Visual::Geo: case Visual::Geos:
            return Tile{ 1.5, 1 };
        case Visual::Kv:
            return table_tile(2, children);
        case Visual::Joints:
            return table_tile(4, children);
        case Visual::Matrix: {
            const size_t n = (size_t)std::lround(std::sqrt((double)children));
            return table_tile(n, n);
        }
        case Visual::Table: {
            const std::vector<int> rows = nodes ? children_of(*nodes, found_) : std::vector<int>();
            return table_tile(rows.empty() ? 1 : children_of(*nodes, rows[0]).size(), rows.size());
        }
        default:
            return Tile{ 1.5, 1 };
        }
    }

    /* What a video card shows now, for Copy image. Premultiplied, which an opaque video
       picture does not change. */
    Image shown_picture() const
    {
        Image image;
        if (visual_ != Visual::Video || !pixels_) return image;
        image.width  = frame_size_.x;
        image.height = frame_size_.y;
        image.rgba   = *pixels_;
        return image;
    }

private:

    /* ---------------------------------------------------------------- state */

    std::string                path_;
    std::string                topic_;   /* the subscribed name the value comes from */
    Visual                     visual_ = Visual::None;
    bool                       built_ = false;
    int                        found_ = -2;      /* the node index, -1 the struct root, -2 none */
    ValueNode                  root_;

    std::string                html_key_;        /* what the children were built for */
    std::vector<Rml::Element*> slots_;           /* the elements whose text fill writes */
    std::vector<std::string>   texts_;           /* what each slot holds now */
    std::vector<std::string>   paths_;           /* the field each slot shows, for Copy cell */
    std::vector<Rml::Element*> lamps_;
    std::vector<int>           lamp_states_;

    double        dt_ = 0, last_frame_ = 0;
    /* A scene's gestures since the last draw, in window pixels and wheel steps. */
    Rml::Vector2f last_, turn_{ 0, 0 }, wheel_at_{ 0, 0 };
    float         wheel_ = 0;
    bool          reset_ = false;

    /* a picture's view: its zoom over the fit, the texel at the middle, and the frame size
       they were for */
    float         zoom_ = 1;
    Rml::Vector2f centre_{ 0, 0 }, view_frame_{ 0, 0 };

    /* a plot's or a bar chart's range: the extremes seen and the eased bounds drawn */
    bool   seen_ = false, eased_ = false;
    double lo_seen_ = 0, hi_seen_ = 0, lo_ = 0, hi_ = 0;

    Reach  reach_[2];                             /* a scene's reach, and a twist's fastest turn */
    Camera cam_;
    Reach  scale_[3];                             /* the joints' column scales */

    struct FrameBounds {
        bool   init = false, eased = false;
        double lat0 = 0, lat1 = 0, lon0 = 0, lon1 = 0, span = 0, clon = 0, clat = 0;
    } frame_;
    struct RectBounds {
        bool   eased = false;
        double x0 = 0, y0 = 0, x1 = 0, y1 = 0, k = 1;
    } rect_;

    /* a picture's texture, remade when a new message arrives, and its pixels */
    Rml::CallbackTexture texture_;
    std::shared_ptr<std::vector<uint8_t>> pixels_;
    Rml::Vector2i        frame_size_;
    uint64_t             frame_seq_ = ~0ull;
    std::string          problem_;

    /* a video's decoder, for the topic it was made for, and the newest picture taken */
    std::unique_ptr<VideoDecoder> decoder_;
    std::string          decoder_topic_;
    uint64_t             decoded_seq_ = 0;

    Rml::Mesh            shapes_;   /* the canvas mesh, kept so its memory is reused */
};

Rml::ElementInstancerGeneric<VizElement> instancer;

} /* namespace */

void viz_init(Capture& capture)
{
    source = &capture;
    Rml::Factory::RegisterElementInstancer("viz", &instancer);
}

VizFeeds viz_feeds(const Capture::WatchView& watch, const std::set<std::string>& shown)
{
    VizFeeds out;
    auto add = [&out](const std::string& path, std::initializer_list<const char*> names) {
        for (const char* name : names) out.traces.push_back(Capture::TraceSpec{ child_path(path, name), TRAIL_SECONDS });
    };
    for (const std::string& path : shown) {
        switch (visual_at(watch, watch.nodes, find_node(watch.nodes, watch, path))) {
        case Visual::Plot:
        case Visual::State:     out.traces.push_back(Capture::TraceSpec{ path, PLOT_SECONDS }); break;
        case Visual::Vec2:      add(path, { "x", "y" }); break;
        case Visual::Vec3:      add(path, { "x", "y", "z" }); break;
        case Visual::Transform: add(child_path(path, "translation"), { "x", "y", "z" }); break;
        case Visual::Geo:       add(path, { "lat", "lon" }); break;
        case Visual::Video:     out.streams.push_back(path); break;
        default: break;
        }
    }
    return out;
}

Tile viz_tile(Rml::Element* element)
{
    const auto* viz = dynamic_cast<const VizElement*>(element);
    return viz ? viz->shape() : Tile();
}

Image viz_picture(Rml::Element* element)
{
    const auto* viz = dynamic_cast<const VizElement*>(element);
    return viz ? viz->shown_picture() : Image();
}
