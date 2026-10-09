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
#include <cctype>
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

/* x, y, z and w. */
Rml::Colourb component_colour(int i)
{
    return i < 3 ? axis_colour(i) : ink.amber;
}

Rml::Colourb palette(long long i)
{
    const Rml::Colourb colours[] = { ink.accent, ink.amber, ink.green, ink.blue, ink.purple, ink.gray };
    const long long n = (long long)(sizeof colours / sizeof colours[0]);
    return colours[((i % n) + n) % n];
}

/* The strips kept free for text at the top and bottom of a drawing, in dp. */
constexpr float TOP = 28, BOT = 22;

/* Text sizes in dp: a label, the value at the top right, and a value shown alone. */
constexpr float LABEL = 12, VALUE = 18, ALONE = 48;

/* Line widths in dp: an axis or an outline, a data line or a shape, an arrow. Grids are 1. */
constexpr float THIN = 1.5f, STROKE = 2.5f, ARROW = 3.5f;

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

/* A Pose: where it is and how it is turned. The identity for no node. */
struct Placed {
    V3   at;
    Quat turn;
};

Placed placed(const Nodes& nodes, int index)
{
    if (index < -1) return Placed{};
    const int p = kid(nodes, index, "position"), o = kid(nodes, index, "orientation");
    return Placed{ p >= 0 ? vec(nodes, p) : V3{}, o >= 0 ? quat(nodes, o) : Quat{} };
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
        c.text("-" + std::to_string(sec) + " s", x, c.h() - c.dp(6), sec == 10 ? Canvas::Left : Canvas::Center, ink.faint, LABEL);
    }
    c.text("now", x_of(now), c.h() - c.dp(6), Canvas::Right, ink.faint, LABEL);
}

/* Horizontal lines at round steps across a value range, labelled in the left gutter. */
template <class Y>
void value_grid(Canvas& c, float x0, float x1, double lo, double hi, Y y_of, bool is_float)
{
    const double step = nice((hi - lo) / 4);
    for (double v = std::ceil(lo / step) * step; v <= hi + 1e-9; v += step) {
        c.line(x0, y_of(v), x1, y_of(v), ink.grid);
        c.text(axis_label(std::fabs(v) < step * 1e-6 ? 0 : v, is_float), x0 - c.dp(6), y_of(v) + c.dp(4), Canvas::Right, ink.faint, LABEL);
    }
}

/* The left gutter a value grid labels in. */
constexpr float GUTTER = 72;

/* The current value at the top right of an area, clear of the pin and close buttons. */
void corner(Canvas& c, const Area& a, const std::string& s, Rml::Colourb colour = ink.text)
{
    c.text(s, a.x + a.w - c.dp(54), a.y + c.dp(21), Canvas::Right, colour, VALUE);
}

/* Components along the bottom of an area: the left dim, the right faint. What does not fit
   is left out, the right first. */
void foot(Canvas& c, const Area& a, const std::string& left, const std::string& right = {})
{
    const float room = a.w - c.dp(16), lw = c.text_width(left, LABEL), rw = c.text_width(right, LABEL);
    if (!left.empty() && lw <= room) c.text(left, a.x + c.dp(8), a.y + a.h - c.dp(6), Canvas::Left, ink.dim, LABEL);
    if (!right.empty() && (left.empty() ? rw : lw + c.dp(16) + rw) <= room)
        c.text(right, a.x + a.w - c.dp(8), a.y + a.h - c.dp(6), Canvas::Right, ink.faint, LABEL);
}

/* Values side by side across an area, each named above in its colour, as large as the
   widest lets them all be up to ALONE dp. */
void side_by_side(Canvas& c, const Area& a, const std::vector<std::string>& names, const std::vector<std::string>& texts,
                  const std::vector<Rml::Colourb>& colours)
{
    if (texts.empty()) return;
    const float column = (a.w - c.dp(16)) / texts.size(), room = column - c.dp(16);
    float wide = 0;
    for (const std::string& s : texts) wide = std::max(wide, c.text_width(s, ALONE));
    const float size = wide > room && wide > 0 ? std::max(LABEL, ALONE * room / wide) : ALONE;
    const float base = a.y + c.dp(TOP / 2) + a.h / 2 + c.dp(size * 0.35f);
    for (size_t i = 0; i < texts.size(); i++) {
        const float x = a.x + c.dp(8) + column * (i + 0.5f);
        c.text(names[i], x, base - c.dp(size + 6), Canvas::Center, colours[i], LABEL);
        c.text(texts[i], x, base, Canvas::Center, ink.text, size);
    }
}

/* A drawing's second reading, at the top right where a plot's value goes, while there is
   room beside a short card name. */
void note(Canvas& c, const Area& a, const std::string& s)
{
    if (c.text_width(s, LABEL) > a.w - c.dp(54 + 120)) return;
    c.text(s, a.x + a.w - c.dp(54), a.y + c.dp(19), Canvas::Right, ink.dim, LABEL);
}

/* An element's index beside the point it is drawn at, in its colour. */
void index_label(Canvas& c, Rml::Vector2f at, size_t i, Rml::Colourb colour)
{
    c.text(std::to_string(i), at.x + c.dp(8), at.y - c.dp(6), Canvas::Left, colour, LABEL);
}

void index_label(Canvas& c, Rml::Vector2f at, size_t i)
{
    index_label(c, at, i, palette((long long)i));
}

/* A value shown alone in the middle of an area, as large as fits up to ALONE dp. */
void alone(Canvas& c, const Area& a, const std::string& s, Rml::Colourb colour = ink.text)
{
    const float room = a.w - c.dp(24), wide = c.text_width(s, ALONE);
    const float size = wide > room && wide > 0 ? std::max(LABEL, ALONE * room / wide) : ALONE;
    c.text(s, a.x + a.w / 2, a.y + c.dp(TOP / 2) + a.h / 2 + c.dp(size * 0.35f), Canvas::Center, colour, size);
}

/* ------------------------------------------------------------------ bounds */

/* The reach of a drawing: the largest magnitude seen since the card opened, with a margin,
   eased. It only grows. A scene's is a distance from the origin, so what is inside it never
   leaves the view however the camera turns. */
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
        c.line(s(0, 0, 0), s(ends[i]), axis_colour(i), THIN);
        const Rml::Vector2f e = s(ends[i]);
        c.text(names[i], e.x + (i == 2 ? 0 : c.dp(8)), e.y + (i == 2 ? -c.dp(6) : c.dp(5)), Canvas::Center, axis_colour(i), LABEL);
    }
}

/* A point's trail in the air, and its foot on the floor with a dashed line up to it,
   the part on one side of the depth split. Nothing else is projected. */
void shadow(Canvas& c, const Scene& s, const V3& v, const std::vector<V3>* trail, bool front)
{
    if (trail) {
        Dots dots(c, ink.trail, 2);
        for (const V3& p : *trail)
            if (s.front(p) == front) dots.add(s(p));
    }
    if (s.front(v) != front) return;
    c.dot(s(v.x, v.y, 0), ink.ground, 3);
    c.dashed(s(v.x, v.y, 0), s(v), ink.ground, THIN);
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

/* Draws one body per point back to front, each with its foot and drop line, the axes
   between the points behind and the points in front. */
template <class Body>
void back_to_front(Canvas& c, const Scene& s, const std::vector<V3>& at, Body body)
{
    std::vector<size_t> order(at.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return s.depth(at[a]) < s.depth(at[b]); });
    bool axes_drawn = false;
    for (const size_t i : order) {
        if (!axes_drawn && s.front(at[i])) { axes(c, s); axes_drawn = true; }
        shadow(c, s, at[i], nullptr, s.front(at[i]));
        body(i);
    }
    if (!axes_drawn) axes(c, s);
}

/* A rotation as three arrows along its turned axes. */
void triad(Canvas& c, const Scene& s, const V3& at, const Quat& q, double len)
{
    const V3 units[3] = { { len, 0, 0 }, { 0, len, 0 }, { 0, 0, len } };
    for (int i = 0; i < 3; i++) {
        const V3 d = rotate(q, units[i]);
        c.arrow(s(at), s(at.x + d.x, at.y + d.y, at.z + d.z), axis_colour(i), ARROW);
    }
}

/* ------------------------------------------------------------------ solids */

V3 unit(const V3& v)
{
    const double n = length(v);
    return n > 1e-12 ? v * (1 / n) : V3{ 0, 0, 1 };
}

/* Two unit vectors square to a unit axis and to each other. */
void basis(const V3& axis, V3& a, V3& b)
{
    const V3 up = std::fabs(axis.z) < 0.9 ? V3{ 0, 0, 1 } : V3{ 1, 0, 0 };
    a = unit(cross(up, axis));
    b = cross(axis, a);
}

/* A circle in space around an axis, as screen points. */
std::vector<Rml::Vector2f> ring(const Scene& s, const V3& centre, const V3& axis, double radius)
{
    V3 a, b;
    basis(unit(axis), a, b);
    std::vector<Rml::Vector2f> out;
    for (int i = 0; i < 48; i++) {
        const double t = 6.28318530717959 * i / 48;
        out.push_back(s(centre + (a * std::cos(t) + b * std::sin(t)) * radius));
    }
    return out;
}

/* A circle on the screen, or the part of it from turn t0 to t1 in radians. */
std::vector<Rml::Vector2f> circle(Rml::Vector2f centre, float radius, double t0 = 0, double t1 = 6.28318530717959)
{
    std::vector<Rml::Vector2f> out;
    for (int i = 0; i <= 48; i++) {
        const double t = t0 + (t1 - t0) * i / 48;
        out.push_back({ centre.x + radius * (float)std::cos(t), centre.y + radius * (float)std::sin(t) });
    }
    return out;
}

/* The ring points furthest to each side of a screen direction, where an outline running
   along it touches the ring. */
std::pair<Rml::Vector2f, Rml::Vector2f> sides(const std::vector<Rml::Vector2f>& ring, Rml::Vector2f along)
{
    const Rml::Vector2f n(-along.y, along.x);
    std::pair<Rml::Vector2f, Rml::Vector2f> out{ ring[0], ring[0] };
    float lo = 1e30f, hi = -1e30f;
    for (const Rml::Vector2f& p : ring) {
        const float d = p.x * n.x + p.y * n.y;
        if (d < lo) { lo = d; out.first = p; }
        if (d > hi) { hi = d; out.second = p; }
    }
    return out;
}

/* A standard 3D shape read from its node. */
struct Solid {
    std::string     kind;      /* the standard name */
    V3              a, b;      /* the centre or the first end, and the second end or a normal */
    V3              size;      /* a box's edge lengths */
    Quat            turn;      /* an oriented box's */
    double          radius = 0;
    std::vector<V3> points;    /* a polygon's corners */
};

Solid solid(const Nodes& nodes, int index, const std::string& kind)
{
    Solid out;
    out.kind = kind;
    auto at = [&](const char* name) {
        const int k = kid(nodes, index, name);
        return k >= 0 ? vec(nodes, k) : V3{};
    };
    out.radius = number(nodes, index, "radius");
    if (kind == "AlignedBox") {
        const V3 lo = at("min"), hi = at("max");
        out.a    = (lo + hi) * 0.5;
        out.size = hi - lo;
    } else if (kind == "OrientedBox") {
        const Placed p = placed(nodes, kid(nodes, index, "pose"));
        out.a    = p.at;
        out.turn = p.turn;
        out.size = at("size");
    } else if (kind == "Plane") {
        out.a = at("position");
        out.b = at("normal");
    } else if (kind == "Segment") {
        out.a = at("a");
        out.b = at("b");
    } else if (kind == "Sphere") {
        out.a = at("center");
    } else if (kind == "Capsule" || kind == "Cylinder") {
        const int axis = kid(nodes, index, "axis");
        if (axis >= 0) {
            out.a = vec(nodes, kid(nodes, axis, "a"));
            out.b = vec(nodes, kid(nodes, axis, "b"));
        }
    } else if (kind == "Cone") {
        out.a = at("base");
        out.b = at("tip");
    } else if (kind == "Polygon") {
        const int points = kid(nodes, index, "points");
        if (points >= 0)
            for (const int k : children_of(nodes, points)) out.points.push_back(vec(nodes, k));
    }
    return out;
}

bool is_box(const Solid& o) { return o.kind == "AlignedBox" || o.kind == "OrientedBox"; }
bool has_axis(const Solid& o) { return o.kind == "Segment" || o.kind == "Capsule" || o.kind == "Cylinder" || o.kind == "Cone"; }

/* A box's corners, the bits of the index picking the side on x, y and z. */
std::vector<V3> corners(const Solid& o)
{
    std::vector<V3> out;
    for (int i = 0; i < 8; i++) {
        const V3 half{ (i & 1 ? 0.5 : -0.5) * o.size.x, (i & 2 ? 0.5 : -0.5) * o.size.y, (i & 4 ? 0.5 : -0.5) * o.size.z };
        out.push_back(o.a + rotate(o.turn, half));
    }
    return out;
}

/* Where a solid's label and drop line go. */
V3 middle(const Solid& o)
{
    if (has_axis(o)) return (o.a + o.b) * 0.5;
    if (o.kind == "Polygon" && !o.points.empty()) {
        V3 sum;
        for (const V3& p : o.points) sum = sum + p;
        return sum * (1.0 / o.points.size());
    }
    return o.a;
}

/* Grows a reach to hold a solid. A plane has no end, so its patch is drawn at a share of the
   reach, held by keeping its point inside half of it. */
void hold(Reach& reach, const Solid& o)
{
    auto take = [&reach](const V3& p, double pad) { reach.seen = std::max(reach.seen, length(p) + pad); };
    if (o.kind == "Plane")
        take(o.a * 2, 0);
    else if (is_box(o))
        for (const V3& p : corners(o)) take(p, 0);
    else if (o.kind == "Polygon")
        for (const V3& p : o.points) take(p, 0);
    else if (o.kind == "Cone") {
        take(o.a, o.radius);
        take(o.b, 0);
    } else {
        take(o.a, o.radius);
        if (has_axis(o)) take(o.b, o.radius);
    }
}

/* A solid as lines: the edges of a box, the outline and rings of a round one, a plane as a
   square patch with its normal. */
void draw_solid(Canvas& c, const Scene& s, const Solid& o, Rml::Colourb colour)
{
    const float rk = (float)(o.radius * s.k);
    if (is_box(o)) {
        const std::vector<V3> p = corners(o);
        for (int i = 0; i < 8; i++)
            for (const int bit : { 1, 2, 4 })
                if (!(i & bit)) c.line(s(p[i]), s(p[i | bit]), colour, STROKE);
    } else if (o.kind == "Plane") {
        const V3 n = unit(o.b);
        V3 u, v;
        basis(n, u, v);
        const double h = 0.35 * s.r;
        c.polyline({ s(o.a + (u + v) * h), s(o.a + (u - v) * h), s(o.a - (u + v) * h), s(o.a - (u - v) * h) }, colour, STROKE, true);
        c.line(s(o.a - u * h), s(o.a + u * h), faded(colour, 0.5f), THIN);
        c.line(s(o.a - v * h), s(o.a + v * h), faded(colour, 0.5f), THIN);
        c.arrow(s(o.a), s(o.a + n * (0.3 * s.r)), colour, ARROW);
    } else if (o.kind == "Segment") {
        c.line(s(o.a), s(o.b), colour, STROKE);
        c.dot(s(o.a), colour, 4);
        c.dot(s(o.b), colour, 4);
    } else if (o.kind == "Sphere") {
        c.polyline(circle(s(o.a), rk), colour, STROKE);
        c.polyline(ring(s, o.a, V3{ 0, 0, 1 }, o.radius), faded(colour, 0.5f), THIN, true);
    } else if (o.kind == "Cylinder" || o.kind == "Capsule") {
        const Rml::Vector2f a = s(o.a), b = s(o.b), d = b - a;
        const float len = std::sqrt(d.x * d.x + d.y * d.y);
        const bool capsule = o.kind == "Capsule";
        const std::vector<Rml::Vector2f> ra = ring(s, o.a, o.b - o.a, o.radius), rb = ring(s, o.b, o.b - o.a, o.radius);
        c.polyline(ra, capsule ? faded(colour, 0.5f) : colour, capsule ? THIN : STROKE, true);
        c.polyline(rb, capsule ? faded(colour, 0.5f) : colour, capsule ? THIN : STROKE, true);
        if (len < 1e-3f) {
            if (capsule) c.polyline(circle(a, rk), colour, STROKE);
            return;
        }
        const Rml::Vector2f along = d / len;
        if (capsule) {
            /* the outline of two balls joined: an arc around each end, a line along each side */
            const Rml::Vector2f n(-along.y * rk, along.x * rk);
            const double angle = std::atan2(along.y, along.x), half = 1.5707963267949;
            c.line(a + n, b + n, colour, STROKE);
            c.line(a - n, b - n, colour, STROKE);
            c.polyline(circle(a, rk, angle + half, angle + 3 * half), colour, STROKE);
            c.polyline(circle(b, rk, angle - half, angle + half), colour, STROKE);
        } else {
            const auto ea = sides(ra, along), eb = sides(rb, along);
            c.line(ea.first, eb.first, colour, STROKE);
            c.line(ea.second, eb.second, colour, STROKE);
        }
    } else if (o.kind == "Cone") {
        const std::vector<Rml::Vector2f> base = ring(s, o.a, o.b - o.a, o.radius);
        c.polyline(base, colour, STROKE, true);
        /* the lines from the tip touch the base where they turn furthest each way */
        const Rml::Vector2f tip = s(o.b), toward = s(o.a) - tip;
        const float len = std::sqrt(toward.x * toward.x + toward.y * toward.y);
        if (len > 1e-3f) {
            const Rml::Vector2f d = toward / len;
            float lo = 1e30f, hi = -1e30f;
            Rml::Vector2f left = base[0], right = base[0];
            for (const Rml::Vector2f& p : base) {
                const Rml::Vector2f q = p - tip;
                const float turn = std::atan2(d.x * q.y - d.y * q.x, d.x * q.x + d.y * q.y);
                if (turn < lo) { lo = turn; left = p; }
                if (turn > hi) { hi = turn; right = p; }
            }
            /* a tip inside the base's outline has no outline lines */
            if (hi - lo < 3.14159f) {
                c.line(tip, left, colour, STROKE);
                c.line(tip, right, colour, STROKE);
            }
        }
        c.dot(tip, colour, 4);
    } else if (o.kind == "Polygon") {
        std::vector<Rml::Vector2f> p;
        for (const V3& v : o.points) p.push_back(s(v));
        c.polyline(p, colour, STROKE, true);
        for (const Rml::Vector2f& v : p) c.dot(v, colour, 3.5f);
    }
}

std::string point(const V3& p)
{
    return "(" + f3(p.x) + ", " + f3(p.y) + ", " + f3(p.z) + ")";
}

/* What a solid measures, and where it is, for the bottom of its card. */
std::pair<std::string, std::string> describe(const Solid& o)
{
    const std::string r = "radius " + f3(o.radius);
    if (is_box(o)) return { "size " + f3(o.size.x) + " x " + f3(o.size.y) + " x " + f3(o.size.z), "at " + point(o.a) };
    if (o.kind == "Plane")    return { "normal " + point(unit(o.b)), "at " + point(o.a) };
    if (o.kind == "Segment")  return { "length " + f3(length(o.b - o.a)), "from " + point(o.a) };
    if (o.kind == "Sphere")   return { r, "at " + point(o.a) };
    if (o.kind == "Cone")     return { r + "  height " + f3(length(o.b - o.a)), "base " + point(o.a) };
    if (o.kind == "Polygon")  return { format_number((double)o.points.size()) + " points", {} };
    return { r + "  length " + f3(length(o.b - o.a)), "from " + point(o.a) };
}

/* An array's count by its element's kind: 3 spheres, 2 boxes, 4 poses. */
std::string counted(size_t n, const std::string& kind)
{
    std::string noun = kind;
    if (noun.size() > 2 && noun.compare(noun.size() - 2, 2, "2D") == 0) noun.resize(noun.size() - 2);
    if (noun == "AlignedBox" || noun == "OrientedBox") noun = "box";
    std::transform(noun.begin(), noun.end(), noun.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
    return format_number((double)n) + " " + noun + (n == 1 ? "" : noun.back() == 'x' ? "es" : "s");
}

/* ------------------------------------------------------------------ figures */

/* The box of a 2D scene fitted to an area, with the grid and both axes, x right and y up,
   each named at its end. Its reach is a largest component, since the box is square. */
struct Plane {
    double k = 1, cx = 0, cy = 0;
    Rml::Vector2f operator()(double x, double y) const { return { (float)(cx + x * k), (float)(cy - y * k) }; }
    Rml::Vector2f operator()(const V3& p) const { return (*this)(p.x, p.y); }
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
    const double end = r * 0.92;
    c.line(p(-r, 0), p(end, 0), axis_colour(0), THIN);
    c.line(p(0, -r), p(0, end), axis_colour(1), THIN);
    c.text("x", p(end, 0).x + c.dp(8), p(end, 0).y + c.dp(5), Canvas::Center, axis_colour(0), LABEL);
    c.text("y", p(0, end).x, p(0, end).y - c.dp(6), Canvas::Center, axis_colour(1), LABEL);
    return p;
}

/* A Pose2D: where it is, and its angle in radians from +x toward +y. */
struct Placed2D {
    V3     at;
    double angle = 0;
};

Placed2D placed_2d(const Nodes& nodes, int index)
{
    if (index < -1) return Placed2D{};
    const int p = kid(nodes, index, "position");
    return Placed2D{ p >= 0 ? vec(nodes, p) : V3{}, number(nodes, index, "angle") };
}

/* A pose on a plane: a dot where it is and an arrow along its heading. */
void heading(Canvas& c, const Plane& p, const Placed2D& at, double len, Rml::Colourb colour)
{
    const V3 tip{ at.at.x + std::cos(at.angle) * len, at.at.y + std::sin(at.angle) * len, 0 };
    c.arrow(p(at.at), p(tip), colour, ARROW);
    c.dot(p(at.at), colour, 4);
}

/* An angle in degrees, wrapped to a half turn either way. */
std::string degrees(double radians)
{
    return print("%.1f\xC2\xB0", std::remainder(radians, 6.28318530717959) * 180 / 3.14159265358979);
}

/* A standard 2D shape read from its node, as the corners of its outline, or a circle. */
struct Figure {
    std::string     kind;
    V3              centre;
    V3              size;      /* a box's edge lengths */
    double          angle = 0, radius = 0;
    std::vector<V3> points;    /* a box's or a polygon's corners in order */
};

Figure figure(const Nodes& nodes, int index, const std::string& kind)
{
    Figure out;
    out.kind = kind;
    auto at = [&](const char* name) {
        const int k = kid(nodes, index, name);
        return k >= 0 ? vec(nodes, k) : V3{};
    };
    if (kind == "AlignedBox2D") {
        const V3 lo = at("min"), hi = at("max");
        out.centre = (lo + hi) * 0.5;
        out.size   = hi - lo;
        out.points = { lo, V3{ hi.x, lo.y, 0 }, hi, V3{ lo.x, hi.y, 0 } };
    } else if (kind == "OrientedBox2D") {
        const Placed2D p = placed_2d(nodes, kid(nodes, index, "pose"));
        out.centre = p.at;
        out.angle  = p.angle;
        out.size   = at("size");
        const double ca = std::cos(p.angle), sa = std::sin(p.angle);
        for (const V3& h : { V3{ -0.5, -0.5, 0 }, V3{ 0.5, -0.5, 0 }, V3{ 0.5, 0.5, 0 }, V3{ -0.5, 0.5, 0 } }) {
            const double x = h.x * out.size.x, y = h.y * out.size.y;
            out.points.push_back(V3{ p.at.x + x * ca - y * sa, p.at.y + x * sa + y * ca, 0 });
        }
    } else if (kind == "Circle") {
        out.centre = at("center");
        out.radius = number(nodes, index, "radius");
    } else if (kind == "Polygon2D") {
        const int points = kid(nodes, index, "points");
        if (points >= 0)
            for (const int k : children_of(nodes, points)) out.points.push_back(vec(nodes, k));
        for (const V3& v : out.points) out.centre = out.centre + v * (1.0 / out.points.size());
    }
    return out;
}

/* Grows a plane's reach to hold a figure. */
void hold(Reach& reach, const Figure& f)
{
    auto take = [&reach](const V3& p, double pad) {
        reach.seen = std::max({ reach.seen, std::fabs(p.x) + pad, std::fabs(p.y) + pad });
    };
    if (f.kind == "Circle") take(f.centre, f.radius);
    for (const V3& p : f.points) take(p, 0);
}

void draw_figure(Canvas& c, const Plane& p, const Figure& f, Rml::Colourb colour)
{
    if (f.kind == "Circle") {
        c.polyline(circle(p(f.centre), (float)(f.radius * p.k)), colour, STROKE);
        c.dot(p(f.centre), colour, 3);
        return;
    }
    std::vector<Rml::Vector2f> corners;
    for (const V3& v : f.points) corners.push_back(p(v));
    c.polyline(corners, colour, STROKE, true);
    if (f.kind == "Polygon2D")
        for (const Rml::Vector2f& v : corners) c.dot(v, colour, 3.5f);
}

std::pair<std::string, std::string> describe(const Figure& f)
{
    const std::string at = "at (" + f3(f.centre.x) + ", " + f3(f.centre.y) + ")";
    if (f.kind == "Circle")    return { "radius " + f3(f.radius), at };
    if (f.kind == "Polygon2D") return { format_number((double)f.points.size()) + " points", {} };
    return { "size " + f3(f.size.x) + " x " + f3(f.size.y), at };
}

/* A turn about an axis through the origin: an arc of radius rho sweeping sweep radians the
   way the turn goes by the right hand rule, with a head at its end. */
void curl(Canvas& c, const Scene& s, const V3& axis, double rho, double sweep, Rml::Colourb colour)
{
    V3 a, b;
    basis(unit(axis), a, b);
    const int steps = 32;
    std::vector<Rml::Vector2f> arc;
    for (int i = 0; i <= steps; i++) {
        const double t = sweep * i / steps;
        arc.push_back(s((a * std::cos(t) + b * std::sin(t)) * rho));
    }
    const Rml::Vector2f tip = arc.back();
    arc.pop_back();
    c.polyline(arc, colour, STROKE);
    c.arrow(arc.back(), tip, colour, STROKE);
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
    case Visual::Vec3: case Visual::Quat: case Visual::Pose: case Visual::Twist: case Visual::Wrench:
    case Visual::Solid: case Visual::Vec3s: case Visual::Poses: case Visual::Solids:
        return true;
    default:
        return false;
    }
}

bool is_html(Visual v)
{
    switch (v) {
    case Visual::None: case Visual::Text: case Visual::Time: case Visual::Duration: case Visual::Uuid:
    case Visual::Hex: case Visual::Chips: case Visual::Kv: case Visual::Cells:
    case Visual::Table: case Visual::Matrix: case Visual::Multi: case Visual::Stream:
        return true;
    default:
        return false;
    }
}

/* A table wants the box its columns and rows fill. Past TABLE_ROWS it asks for that
   height and scrolls inside. */
constexpr double TABLE_COL = 100, TABLE_ROW = 24;
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
        topic_   = GetAttribute<Rml::String>("topic", "");
        history_ = GetAttribute<Rml::String>("history", "on") != "off";
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
        case Visual::Vec4:       vec4(c, nodes); break;
        case Visual::State:      state(c, node); break;
        case Visual::Image:      picture(c, nodes); break;
        case Visual::Video:      video(c); break;
        case Visual::Bars:       bars(c, nodes); break;
        case Visual::Vec2:       vec2(c, nodes); break;
        case Visual::Vec3:       vec3(c, nodes); break;
        case Visual::Quat:       quaternion(c, nodes); break;
        case Visual::Pose:       pose(c, nodes, node); break;
        case Visual::Twist:      twist(c, nodes); break;
        case Visual::Wrench:     wrench(c, nodes); break;
        case Visual::Solid:      solid_one(c, nodes, node); break;
        case Visual::Solids:     solids(c, nodes, node); break;
        case Visual::Geo:        geo(c, nodes); break;
        case Visual::Color:      colour(c, nodes); break;
        case Visual::Joints:     joints(c, nodes); break;
        case Visual::Vec2s:      vec2s(c, nodes); break;
        case Visual::Pose2D:     pose_2d(c, nodes); break;
        case Visual::Poses2D:    poses_2d(c, nodes); break;
        case Visual::Figure:     figure_one(c, nodes, node); break;
        case Visual::Figures:    figures(c, nodes, node); break;
        case Visual::Vec3s:      vec3s(c, nodes); break;
        case Visual::Poses:      poses(c, nodes, node); break;
        case Visual::Geos:       geos(c, nodes); break;
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
        case Visual::Uuid:
            html("uuid", "<div class=\"big mono slot\"></div>");
            put(0, uuid_text(nodes, found_), node.path);
            break;
        case Visual::Stream: {
            /* a stream handed over by URL: its name, the URL, and the hints about it */
            html("stream", "<div class=\"big text slot\"></div><div class=\"stream-url mono slot\"></div>"
                           "<div class=\"chips\"><span class=\"chip mono slot\"></span>"
                           "<span class=\"chip mono slot\"></span><span class=\"chip mono slot\"></span></div>");
            auto field = [&](const char* name) -> const ValueNode* {
                const int k = kid(nodes, found_, name);
                return k >= 0 ? &nodes[k] : nullptr;
            };
            const ValueNode *name = field("name"), *url = field("url"), *kind = field("kind"), *codec = field("codec");
            const int64_t width = field("width") ? field("width")->integer : 0, height = field("height") ? field("height")->integer : 0;
            put(0, name && !name->text.empty() ? name->text : "unnamed stream");
            if (url) put(1, url->text, url->path);
            put(2, kind ? value_text(*kind) : "");
            put(3, codec ? value_text(*codec) : "");
            put(4, width > 0 ? std::to_string(width) + " x " + std::to_string(height) : "size unstated");
            break;
        }
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

    /* Lines over the last ten seconds against a grid of round steps, one per traced path.
       The range is every value seen since the card opened plus a margin, eased so it never
       jumps. Before any trace it is around the first value. */
    void lines(Canvas& c, const std::vector<std::string>& paths, const std::vector<Rml::Colourb>& colours,
               double first, bool is_float)
    {
        const double now = Capture::now_s();
        for (const std::string& path : paths)
            for (const Capture::TracePoint& p : trace(path)) {
                if (!seen_) { lo_seen_ = hi_seen_ = p.v; seen_ = true; }
                lo_seen_ = std::min(lo_seen_, p.v);
                hi_seen_ = std::max(hi_seen_, p.v);
            }
        if (!seen_) { lo_seen_ = hi_seen_ = first; seen_ = true; }
        double lo = lo_seen_, hi = hi_seen_;
        if (hi - lo < 1e-9) { lo -= 1; hi += 1; }
        else { const double m = (hi - lo) * 0.1; lo -= m; hi += m; }
        ease_range(lo, hi);

        const float top = c.dp(TOP), bottom = c.h() - c.dp(BOT);
        const float x0 = c.dp(GUTTER), x1 = c.w() - c.dp(10);
        auto X = [&](double t) { return (float)(x0 + (x1 - x0) * (1 - (now - t) / PLOT_SECONDS)); };
        auto Y = [&](double v) { return (float)(top + (bottom - top) * (1 - (v - lo_) / (hi_ - lo_))); };

        time_grid(c, X, now, top, bottom);
        value_grid(c, x0, x1, lo_, hi_, Y, is_float);
        /* Each line is cut to the plot so its tail slides off the left edge and a new extreme
           the range has not eased out to yet stays inside. The newest value holds until now. */
        for (size_t k = 0; k < paths.size(); k++) {
            const auto& points = trace(paths[k]);
            Columns columns;
            for (const Capture::TracePoint& p : points) columns.add({ X(p.t), Y(p.v) });
            if (!points.empty() && points.back().t < now) columns.add({ X(now), Y(points.back().v) });
            const std::vector<Rml::Vector2f>& line = columns.points();
            std::vector<Rml::Vector2f> run;
            for (size_t i = 1; i < line.size(); i++) {
                float ax = line[i - 1].x, ay = line[i - 1].y, bx = line[i].x, by = line[i].y;
                if (!clip(ax, ay, bx, by, x0, top, x1, bottom)) continue;
                if (run.empty() || run.back().x != ax || run.back().y != ay) {
                    c.polyline(run, colours[k], STROKE);
                    run.assign(1, { ax, ay });
                }
                run.push_back({ bx, by });
            }
            c.polyline(run, colours[k], STROKE);
            if (!points.empty()) {
                const float y = Y(points.back().v);
                if (y >= top && y <= bottom) c.dot(X(std::max(now, points.back().t)), y, colours[k], 4);
            }
        }
    }

    /* The value over time, or without history the value alone. */
    void plot(Canvas& c, const ValueNode& node)
    {
        if (!history_) {
            alone(c, c.all(), value_text(node));
            return;
        }
        lines(c, { path_ }, { ink.accent }, node.number, node.is_float);
        corner(c, c.all(), value_text(node));
    }

    /* A Float4, Double4 or Int4: a line per component in its own colour, named at the top
       right, or without history the four values side by side. */
    void vec4(Canvas& c, const Nodes& nodes)
    {
        const char* names[4] = { "x", "y", "z", "w" };
        std::vector<std::string> paths, texts;
        std::vector<Rml::Colourb> colours;
        bool is_float = false;
        for (int i = 0; i < 4; i++) {
            const int k = kid(nodes, found_, names[i]);
            paths.push_back(child_path(path_, names[i]));
            texts.push_back(k >= 0 ? value_text(nodes[k]) : "");
            colours.push_back(component_colour(i));
            is_float = is_float || (k >= 0 && nodes[k].is_float);
        }
        if (!history_) {
            side_by_side(c, c.all(), { "x", "y", "z", "w" }, texts, colours);
            return;
        }
        const int x = kid(nodes, found_, "x");
        lines(c, paths, colours, x >= 0 ? nodes[x].number : 0, is_float);
        /* each component named in its colour at the top right, with its value while there is room */
        std::vector<std::string> legend;
        float wide = 0;
        for (int i = 0; i < 4; i++) {
            legend.push_back(std::string(names[i]) + " " + texts[i]);
            wide += c.text_width(legend.back(), LABEL) + c.dp(12);
        }
        if (wide > c.w() - c.dp(54 + 120))
            for (int i = 0; i < 4; i++) legend[i] = names[i];
        float right = c.w() - c.dp(54);
        for (int i = 3; i >= 0; i--) {
            c.text(legend[i], right, c.dp(19), Canvas::Right, colours[i], LABEL);
            right -= c.text_width(legend[i], LABEL) + c.dp(12);
        }
    }

    void ease_range(double lo, double hi)
    {
        if (!eased_) { lo_ = lo; hi_ = hi; eased_ = true; }
        lo_ = ease(lo_, lo, dt_);
        hi_ = ease(hi_, hi, dt_);
    }

    /* A bool or an enum over ten seconds: a band per run of equal values on whole pixels.
       A pixel shows the first run to reach it, so a fast toggle costs the width. Without
       history it is the value alone, in its band's colour. */
    void state(Canvas& c, const ValueNode& node)
    {
        auto colour = [&](double v) {
            if (node.kind == ValueNode::Bool) return v != 0 ? ink.green : ink.border;
            return palette((long long)v);
        };
        if (!history_) {
            /* false reads on the dark card, where its band's colour would not */
            alone(c, c.all(), value_text(node), node.kind == ValueNode::Bool && node.number == 0 ? ink.dim : colour(node.number));
            return;
        }
        const auto& points = trace(path_);
        const double now = Capture::now_s();
        const float  left = c.dp(10), right = c.w() - c.dp(10);
        auto X = [&](double t) {
            return std::max(left, (float)(left + (right - left) * (1 - (now - t) / PLOT_SECONDS)));
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
    void bars(Canvas& c, const Nodes& nodes)
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
        const float top = c.dp(TOP), bottom = c.h() - c.dp(BOT), x0 = c.dp(GUTTER), x1 = c.w() - c.dp(10);
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
            if (n <= LABELLED) c.text(std::to_string(first), x + width * (1 - gap) / 2, c.h() - c.dp(6), Canvas::Center, ink.faint, LABEL);
        }
        foot(c, c.all(), {}, format_number((double)n) + " values");
    }

    /* ---------------------------------------------------------------- planes */

    void vec2(Canvas& c, const Nodes& nodes)
    {
        const V3 v = vec(nodes, found_);
        const double r = reach_[0].grow({ v.x, v.y }, dt_);
        const Plane p = plane(c, c.all(), r);
        if (history_) {
            Dots dots(c, ink.trail, 2);
            for (const V3& t : trail(child_path(path_, "x"), child_path(path_, "y"))) dots.add(p(t.x, t.y));
        }
        c.arrow(p(0, 0), p(v.x, v.y), ink.accent, ARROW);
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
            c.arrow(p(0, 0), p(v[i].x, v[i].y), palette((long long)i), ARROW);
            index_label(c, p(v[i].x, v[i].y), i);
        }
        foot(c, c.all(), format_number((double)v.size()) + " vectors");
    }

    /* A Pose2D: its heading at its position joined to the origin, and the position's trail. */
    void pose_2d(Canvas& c, const Nodes& nodes)
    {
        const Placed2D at = placed_2d(nodes, found_);
        /* the margin keeps the heading inside */
        const double r = reach_[0].grow({ at.at.x, at.at.y }, dt_, 1.3);
        const Plane p = plane(c, c.all(), r);
        if (history_) {
            const std::string base = child_path(path_, "position");
            Dots dots(c, ink.trail, 2);
            for (const V3& t : trail(base + ".x", base + ".y")) dots.add(p(t.x, t.y));
        }
        c.line(p(0, 0), p(at.at), ink.faint, THIN);
        heading(c, p, at, 0.2 * r, ink.accent);
        note(c, c.all(), "angle " + degrees(at.angle));
        foot(c, c.all(), "x " + f3(at.at.x) + "  y " + f3(at.at.y));
    }

    /* A chain of Pose2Ds: consecutive ones joined, a heading at each. */
    void poses_2d(Canvas& c, const Nodes& nodes)
    {
        std::vector<Placed2D> list;
        for (const int i : children_of(nodes, found_)) {
            list.push_back(placed_2d(nodes, i));
            reach_[0].seen = std::max({ reach_[0].seen, std::fabs(list.back().at.x), std::fabs(list.back().at.y) });
        }
        const double r = reach_[0].grow({}, dt_, 1.2);
        const Plane p = plane(c, c.all(), r);
        for (size_t i = 1; i < list.size(); i++) c.line(p(list[i - 1].at), p(list[i].at), ink.dim, THIN);
        for (size_t i = 0; i < list.size(); i++) {
            heading(c, p, list[i], 0.15 * r, palette((long long)i));
            index_label(c, p(list[i].at), i);
        }
        foot(c, c.all(), counted(list.size(), "Pose2D"));
    }

    /* One standard 2D shape on a plane that grows to hold it. */
    void figure_one(Canvas& c, const Nodes& nodes, const ValueNode& node)
    {
        const Figure one = figure(nodes, found_, node.std_name);
        hold(reach_[0], one);
        const Plane p = plane(c, c.all(), reach_[0].grow({}, dt_));
        draw_figure(c, p, one, ink.accent);
        if (one.kind == "OrientedBox2D") note(c, c.all(), "angle " + degrees(one.angle));
        const auto text = describe(one);
        foot(c, c.all(), text.first, text.second);
    }

    /* An array of one kind of 2D shape on one plane, each in its own colour. */
    void figures(Canvas& c, const Nodes& nodes, const ValueNode& node)
    {
        std::vector<Figure> list;
        for (const int i : children_of(nodes, found_)) {
            list.push_back(figure(nodes, i, node.elem_std));
            hold(reach_[0], list.back());
        }
        const Plane p = plane(c, c.all(), reach_[0].grow({}, dt_));
        for (size_t i = 0; i < list.size(); i++) {
            draw_figure(c, p, list[i], palette((long long)i));
            index_label(c, p(list[i].centre), i);
        }
        foot(c, c.all(), counted(list.size(), node.elem_std));
    }

    /* ---------------------------------------------------------------- scenes */

    /* An arrow from the origin in a scene, its foot, drop line and trail. */
    void vec3(Canvas& c, const Nodes& nodes)
    {
        const V3 v = vec(nodes, found_);
        const double r = reach_[0].grow({ length(v) }, dt_);
        const Scene s = scene(c, c.all(), r, camera());
        const std::vector<V3> t = history_ ? trail(child_path(path_, "x"), child_path(path_, "y"), child_path(path_, "z"))
                                           : std::vector<V3>();
        layered(c, s, v, &t, [&] { c.arrow(s(0, 0, 0), s(v), ink.accent, ARROW); });
        foot(c, c.all(), "x " + f3(v.x) + "  y " + f3(v.y) + "  z " + f3(v.z));
    }

    void quaternion(Canvas& c, const Nodes& nodes)
    {
        const Quat q = quat(nodes, found_);
        const Scene s = scene(c, c.all(), 1, camera());
        axes(c, s);
        triad(c, s, V3{}, q, 0.9);
        foot(c, c.all(), euler(q));
    }

    /* A Pose or a Transform: its triad at its position joined to the origin, and the
       position's trail. The turn is at the top right, a Transform's parent frame at the
       bottom right. */
    void pose(Canvas& c, const Nodes& nodes, const ValueNode& node)
    {
        const bool framed = node.std_name == "Transform";
        const Placed p = placed(nodes, framed ? kid(nodes, found_, "pose") : found_);
        /* the margin keeps the triad inside */
        const double r = reach_[0].grow({ length(p.at) }, dt_, 1.3);
        const Scene s = scene(c, c.all(), r, camera());
        const std::string base = child_path(framed ? child_path(path_, "pose") : path_, "position");
        const std::vector<V3> t = history_ ? trail(base + ".x", base + ".y", base + ".z") : std::vector<V3>();
        layered(c, s, p.at, &t, [&] {
            c.line(s(0, 0, 0), s(p.at), ink.faint, THIN);
            triad(c, s, p.at, p.turn, 0.2 * r);
        });
        note(c, c.all(), euler(p.turn));
        const int parent = framed ? kid(nodes, found_, "parent") : -2;
        foot(c, c.all(), "x " + f3(p.at.x) + "  y " + f3(p.at.y) + "  z " + f3(p.at.z),
             parent >= 0 && !nodes[parent].text.empty() ? "in " + nodes[parent].text : std::string());
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
        auto hold = [this](const V3& p) { reach_[0].seen = std::max(reach_[0].seen, length(p)); };
        for (const V3& p : path) hold(p);
        if (turning && length(centre) <= 2 * extent) hold(centre);
        const double r = reach_[0].grow({ length(v) }, dt_);
        const Scene s = scene(c, c.all(), r, camera());

        if (turning) {
            const V3 axis = w * (1 / spin);
            /* a turn centre this far out is a straight run, and its line would leave the view */
            if (length(centre) < 20 * r) {
                c.dashed(s(centre - axis * r), s(centre + axis * r), ink.dim, THIN);
                c.dot(s(centre), ink.dim, 4);
            }
        }
        /* its own colour, since a forward run lies along the x axis */
        std::vector<Rml::Vector2f> drawn;
        for (const V3& p : path) drawn.push_back(s(p));
        c.polyline(drawn, ink.purple, STROKE);
        c.dot(drawn.back(), ink.purple, 4);
        /* the curl sweeps most of a turn at the fastest turn seen */
        const double fastest = reach_[1].grow({ spin }, dt_, 1.0);
        if (turning) curl(c, s, w * (1 / spin), 0.15 * r, 5 * spin / fastest, ink.amber);
        layered(c, s, v, nullptr, [&] { c.arrow(s(0, 0, 0), s(v), ink.accent, ARROW); });
        foot(c, c.all(), "v " + f3(length(v)) + " m/s  \xCF\x89 " + f3(spin) + " rad/s", "next " + print("%.1f", ahead) + " s");
    }

    void vec3s(Canvas& c, const Nodes& nodes)
    {
        std::vector<V3> v;
        for (const int i : children_of(nodes, found_)) v.push_back(vec(nodes, i));
        for (const V3& e : v) reach_[0].seen = std::max(reach_[0].seen, length(e));
        const Scene s = scene(c, c.all(), reach_[0].grow({}, dt_), camera());
        back_to_front(c, s, v, [&](size_t i) {
            c.arrow(s(0, 0, 0), s(v[i]), palette((long long)i), ARROW);
            index_label(c, s(v[i]), i);
        });
        foot(c, c.all(), format_number((double)v.size()) + " vectors");
    }

    /* A chain of Poses or Transforms: consecutive ones joined in the air, a triad at each. */
    void poses(Canvas& c, const Nodes& nodes, const ValueNode& node)
    {
        const bool framed = node.elem_std == "Transform";
        std::vector<Placed> p;
        std::vector<V3> at;
        for (const int i : children_of(nodes, found_)) {
            p.push_back(placed(nodes, framed ? kid(nodes, i, "pose") : i));
            at.push_back(p.back().at);
        }
        for (const V3& e : at) reach_[0].seen = std::max(reach_[0].seen, length(e));
        const double r = reach_[0].grow({}, dt_, 1.2);
        const Scene s = scene(c, c.all(), r, camera());
        back_to_front(c, s, at, [&](size_t i) {
            if (i) c.line(s(at[i - 1]), s(at[i]), ink.dim, THIN);
            triad(c, s, at[i], p[i].turn, 0.15 * r);
            index_label(c, s(at[i]), i, ink.dim);
        });
        foot(c, c.all(), format_number((double)p.size()) + (framed ? " transforms" : " poses"));
    }

    /* A force and a torque about the frame origin: the force as an arrow with its trail,
       the torque as a turn about its axis that sweeps most of a turn at the strongest seen. */
    void wrench(Canvas& c, const Nodes& nodes)
    {
        const int fi = kid(nodes, found_, "force"), ti = kid(nodes, found_, "torque");
        const V3 f = fi >= 0 ? vec(nodes, fi) : V3{}, t = ti >= 0 ? vec(nodes, ti) : V3{};
        const double r = reach_[0].grow({ length(f) }, dt_);
        const Scene s = scene(c, c.all(), r, camera());
        const double torque = length(t), strongest = reach_[1].grow({ torque }, dt_, 1.0);
        if (torque > 1e-9) {
            const V3 axis = t * (1 / torque);
            c.dashed(s(axis * -r), s(axis * r), faded(ink.amber, 0.6f), THIN);
            curl(c, s, axis, 0.2 * r, 5 * torque / strongest, ink.amber);
        }
        const std::string base = child_path(path_, "force");
        const std::vector<V3> tail = history_ ? trail(base + ".x", base + ".y", base + ".z") : std::vector<V3>();
        layered(c, s, f, &tail, [&] { c.arrow(s(0, 0, 0), s(f), ink.accent, ARROW); });
        foot(c, c.all(), "F " + f3(length(f)) + " N  \xCF\x84 " + f3(torque) + " N m");
    }

    /* One standard shape in a scene that grows to hold it, with its drop line. */
    void solid_one(Canvas& c, const Nodes& nodes, const ValueNode& node)
    {
        const Solid one = solid(nodes, found_, node.std_name);
        hold(reach_[0], one);
        const Scene s = scene(c, c.all(), reach_[0].grow({}, dt_), camera());
        layered(c, s, middle(one), nullptr, [&] { draw_solid(c, s, one, ink.accent); });
        if (one.kind == "OrientedBox") note(c, c.all(), euler(one.turn));
        const auto text = describe(one);
        foot(c, c.all(), text.first, text.second);
    }

    /* An array of one kind of shape in one scene, each in its own colour. */
    void solids(Canvas& c, const Nodes& nodes, const ValueNode& node)
    {
        std::vector<Solid> list;
        std::vector<V3> at;
        for (const int i : children_of(nodes, found_)) {
            list.push_back(solid(nodes, i, node.elem_std));
            at.push_back(middle(list.back()));
            hold(reach_[0], list.back());
        }
        const Scene s = scene(c, c.all(), reach_[0].grow({}, dt_), camera());
        back_to_front(c, s, at, [&](size_t i) {
            draw_solid(c, s, list[i], palette((long long)i));
            index_label(c, s(at[i]), i);
        });
        foot(c, c.all(), counted(list.size(), node.elem_std));
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
        c.text("N", c.w() - c.dp(16), top + c.dp(12), Canvas::Center, ink.dim, LABEL);
        c.arrow({ c.w() - c.dp(16), top + c.dp(36) }, { c.w() - c.dp(16), top + c.dp(17) }, ink.dim, THIN);
    }

    void geo(Canvas& c, const Nodes& nodes)
    {
        const V3 now{ number(nodes, found_, "lat"), number(nodes, found_, "lon"), number(nodes, found_, "alt") };
        std::vector<V3> points = history_ ? trail(child_path(path_, "lat"), child_path(path_, "lon")) : std::vector<V3>();
        if (points.empty()) points.push_back(now);
        map(c, points, [&](auto P, float left, float top, float right, float bottom) {
            /* a point within half a pixel of the last one drawn adds nothing to the path */
            std::vector<Rml::Vector2f> run{ P(points[0].x, points[0].y) };
            Rml::Vector2f from = run[0];
            for (size_t i = 1; i < points.size(); i++) {
                Rml::Vector2f a = from, b = P(points[i].x, points[i].y);
                if (i + 1 < points.size() && std::fabs(b.x - a.x) < 0.5f && std::fabs(b.y - a.y) < 0.5f) continue;
                from = b;
                if (!clip(a.x, a.y, b.x, b.y, left, top, right, bottom)) continue;
                if (run.back().x != a.x || run.back().y != a.y) {
                    c.polyline(run, ink.accent, STROKE);
                    run.assign(1, a);
                }
                run.push_back(b);
            }
            c.polyline(run, ink.accent, STROKE);
            const Rml::Vector2f at = P(now.x, now.y);
            if (inside(at, left, top, right, bottom)) c.dot(at, ink.accent, 5);
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
                c.dot(at, palette((long long)i), 5);
                index_label(c, at, i);
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
        const float width = items.empty() ? 0 : (c.w() - c.dp(20)) / items.size();
        for (size_t i = 0; i < items.size(); i++) {
            c.rect(c.dp(10) + i * width + 1, c.dp(TOP), width - 2, c.h() - c.dp(TOP + BOT + 18), rgba(nodes, items[i]));
            if (items.size() <= LABELLED)
                c.text(std::to_string(i), c.dp(10) + i * width + width / 2, c.h() - c.dp(BOT + 4), Canvas::Center, ink.faint, LABEL);
        }
        foot(c, c.all(), {}, format_number((double)items.size()) + " colors");
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
        const float gutter = c.dp(28), gap = c.dp(10), top = c.dp(TOP) + c.dp(16), bottom = c.h() - c.dp(8);
        const float col_w = (c.w() - gutter - c.dp(8) - 2 * gap) / 3;
        const float row_h = rows ? std::min(c.dp(26), (bottom - top) / rows) : 0, bar_h = std::max(1.f, row_h - c.dp(4));
        for (int f = 0; f < 3; f++) {
            double biggest = 0;
            for (const int i : columns[f]) biggest = std::max(biggest, std::fabs(nodes[i].number));
            scale_[f].seen = std::max(scale_[f].seen, biggest);
            const double m = scale_[f].grow({}, dt_, 1.0);
            const float x = gutter + f * (col_w + gap);
            c.text(std::string(names[f]) + "  \xC2\xB1" + short_number(m), x, c.dp(TOP) + c.dp(6), Canvas::Left, ink.faint, LABEL);
            for (size_t r = 0; r < columns[f].size(); r++) {
                const float y = top + r * row_h;
                const double v = nodes[columns[f][r]].number;
                c.rect(x, y, col_w, bar_h, ink.panel);
                for (int q = 1; q < 4; q++) c.line(x + col_w * q / 4, y, x + col_w * q / 4, y + bar_h, ink.grid);
                const float mid = x + col_w / 2, len = (float)(std::max(-1.0, std::min(1.0, v / m)) * col_w / 2);
                c.rect(std::min(mid, mid + len), y, std::fabs(len), bar_h, ink.accent);
                c.line(mid, y, mid, y + bar_h, ink.border);
                c.text(f3(v), x + col_w - c.dp(5), y + bar_h / 2 + c.dp(4), Canvas::Right, ink.text, LABEL);
            }
        }
        for (size_t r = 0; r < rows; r++)
            c.text(std::to_string(r), gutter - c.dp(6), top + r * row_h + bar_h / 2 + c.dp(4), Canvas::Right, ink.faint, LABEL);
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
            c.text(why, c.w() / 2, c.h() / 2, Canvas::Center, ink.faint, LABEL);
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
        case Visual::Plot: case Visual::Vec4: case Visual::State: case Visual::Bars:
            return Tile{ 3, 1 };
        case Visual::Text: case Visual::Time: case Visual::Duration: case Visual::Uuid: case Visual::Hex:
        case Visual::Chips: case Visual::Cells: case Visual::Colors: case Visual::None:
            return Tile{ 3, 0.5 };
        case Visual::Stream:
            return Tile{ 2.5, 0.5 };
        case Visual::Color:
            return Tile{ 1.5, 0.5 };
        case Visual::Vec3: case Visual::Quat: case Visual::Pose: case Visual::Twist: case Visual::Wrench:
        case Visual::Solid: case Visual::Vec3s: case Visual::Poses: case Visual::Solids:
            return Tile{ 1, 1.5 };
        case Visual::Vec2: case Visual::Vec2s: case Visual::Pose2D: case Visual::Poses2D:
        case Visual::Figure: case Visual::Figures:
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
    bool                       history_ = true;  /* draw what came before, else only the newest */
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

bool viz_history(Visual visual)
{
    switch (visual) {
    case Visual::Plot: case Visual::Vec4: case Visual::State: case Visual::Vec2: case Visual::Vec3: case Visual::Pose:
    case Visual::Pose2D: case Visual::Wrench: case Visual::Geo:
        return true;
    default:
        return false;
    }
}

VizFeeds viz_feeds(const Capture::WatchView& watch, const std::set<std::string>& shown,
                   const std::set<std::string>& still)
{
    VizFeeds out;
    auto add = [&out](const std::string& path, std::initializer_list<const char*> names) {
        for (const char* name : names) out.traces.push_back(Capture::TraceSpec{ child_path(path, name), TRAIL_SECONDS });
    };
    for (const std::string& path : shown) {
        const int index = find_node(watch.nodes, watch, path);
        const Visual visual = visual_at(watch, watch.nodes, index);
        if (visual == Visual::Video) out.streams.push_back(path);
        if (still.count(path) || !viz_history(visual)) continue;
        const std::string std_name = index == -1 ? watch.root_std : index >= 0 ? watch.nodes[index].std_name : "";
        switch (visual) {
        case Visual::Plot:
        case Visual::State: out.traces.push_back(Capture::TraceSpec{ path, PLOT_SECONDS }); break;
        case Visual::Vec4:
            for (const char* name : { "x", "y", "z", "w" })
                out.traces.push_back(Capture::TraceSpec{ child_path(path, name), PLOT_SECONDS });
            break;
        case Visual::Vec2:  add(path, { "x", "y" }); break;
        case Visual::Vec3:  add(path, { "x", "y", "z" }); break;
        case Visual::Pose:
            add(child_path(std_name == "Transform" ? child_path(path, "pose") : path, "position"), { "x", "y", "z" });
            break;
        case Visual::Pose2D: add(child_path(path, "position"), { "x", "y" }); break;
        case Visual::Wrench: add(child_path(path, "force"), { "x", "y", "z" }); break;
        case Visual::Geo:   add(path, { "lat", "lon" }); break;
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
