#include "viz.hpp"

#include "canvas.hpp"
#include "format.hpp"
#include "image.hpp"
#include "values.hpp"
#include "video.hpp"

#include "rant_math.hpp"

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
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <memory>
#include <variant>

namespace {

using ValueNode = Capture::ValueNode;
using Nodes     = std::vector<ValueNode>;

namespace math  = rant::math;
namespace types = rant::types;
using types::Double2;
using types::Double3;
using types::Pose;
using types::Pose2D;
using types::Quaternion;

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

/* The strips kept free for text at the top and bottom of a drawing, in dp, and the line a
   reading takes above the time labels of a view over time. */
constexpr float TOP = 28, BOT = 22, LINE = 18;

/* Text sizes in dp: a label, and a value shown alone. */
constexpr float LABEL = 12, ALONE = 48;

/* Line widths in dp: an axis or an outline, a data line or a shape, an arrow. Grids are 1. */
constexpr float THIN = 1.5f, STROKE = 2.5f, ARROW = 3.5f;

/* How much history a plot shows and a trail keeps, and how far ahead a twist's path looks. */
constexpr double PLOT_SECONDS = 10, TRAIL_SECONDS = 30, TWIST_AHEAD = 2;

/* A picture zooms in until one of its texels is this many dp. */
constexpr float ZOOM_TEXEL = 32;

/* An array up to this long is labelled per element. */
constexpr size_t LABELLED = 16;

/* ------------------------------------------------------------------ what a card shows */

/* A part of a visual its card can show or hide. Colours off draws every element in one. */
enum class Show { History, Arrows, Orientation, Line, Path, Turn, Torque, Labels, Colours, Drops, Grid, Axes, Readings };

const VizShow SHOW_NAMES[] = {
    { "history", "History" }, { "arrows", "Arrows" }, { "orientation", "Orientation" }, { "line", "Line" },
    { "path", "Path" },       { "turn", "Turn" },     { "torque", "Torque" },           { "labels", "Labels" },
    { "colors", "Colors" },   { "drops", "Drop lines" }, { "grid", "Grid" },           { "axes", "Axes" },
    { "readings", "Readings" },
};

/* A set of parts, a bit per Show. */
constexpr unsigned bits(std::initializer_list<Show> parts)
{
    unsigned out = 0;
    for (const Show s : parts) out |= 1u << (int)s;
    return out;
}

/* What every plane offers, and every scene, which adds drop lines. */
constexpr unsigned PLANE_PARTS = bits({ Show::Grid, Show::Axes, Show::Readings });
constexpr unsigned SCENE_PARTS = bits({ Show::Drops }) | PLANE_PARTS;

/* A card's choices from its shows attribute, as viz_shows_text writes them. */
ShowChoices read_choices(const std::string& text)
{
    ShowChoices out;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find(' ', at);
        if (end == std::string::npos) end = text.size();
        const size_t colon = text.find(':', at);
        if (colon < end) out[text.substr(at, colon - at)] = text.compare(colon + 1, end - colon - 1, "1") == 0;
        at = end + 1;
    }
    return out;
}

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

/* The node is nodes[index], or an element read in place with no index. */
void brief_into(const Nodes& nodes, const ValueNode& node, int index, const std::vector<uint8_t>& message,
                std::string& out)
{
    if (node.kind != ValueNode::Struct && node.kind != ValueNode::Array) {
        out += node.kind == ValueNode::Gap ? "..." : human(node);
        return;
    }
    out += node.kind == ValueNode::Array ? '[' : '(';
    bool first = true;
    each_element(nodes, index, message, [&](const ValueNode& e, int at) {
        if (out.size() > BRIEF) return false;
        if (!first) out += ", ";
        first = false;
        brief_into(nodes, e, at, message, out);
        return true;
    });
    out += node.kind == ValueNode::Array ? ']' : ')';
}

std::string brief(const Nodes& nodes, const ValueNode& node, int index, const std::vector<uint8_t>& message)
{
    std::string out;
    brief_into(nodes, node, index, message, out);
    if (out.size() > BRIEF) out = out.substr(0, BRIEF) + "...";
    return out;
}

/* A whole message in one line: a struct's fields each named, a bare root as its value.
   A call's parts list every element they show, so they need no message. */
std::string brief_message(const Nodes& nodes)
{
    static const std::vector<uint8_t> listed;
    const std::vector<int> top = children_of(nodes, -1);
    if (top.empty()) return DASH;
    if (top.size() == 1 && nodes[top[0]].path.empty()) return brief(nodes, nodes[top[0]], top[0], listed);
    std::string out;
    for (const int k : top) {
        if (out.size() > BRIEF) break;
        if (!out.empty()) out += ", ";
        out += nodes[k].name + ": ";
        brief_into(nodes, nodes[k], k, listed, out);
    }
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

/* A rotation that turns nothing, for a pose with no orientation. */
constexpr Quaternion NO_TURN{ 0, 0, 0, 1 };

Double3 vec(const Nodes& nodes, int index)
{
    return Double3{ number(nodes, index, "x"), number(nodes, index, "y"), number(nodes, index, "z") };
}

Double2 vec_2d(const Nodes& nodes, int index)
{
    return Double2{ number(nodes, index, "x"), number(nodes, index, "y") };
}

Quaternion quat(const Nodes& nodes, int index)
{
    return Quaternion{ number(nodes, index, "x"), number(nodes, index, "y"), number(nodes, index, "z"),
                       number(nodes, index, "w") };
}

/* A Pose, the identity for no node. */
Pose placed(const Nodes& nodes, int index)
{
    Pose out{ Double3{}, NO_TURN };
    if (index < -1) return out;
    const int p = kid(nodes, index, "position"), o = kid(nodes, index, "orientation");
    if (p >= 0) out.position = vec(nodes, p);
    if (o >= 0) out.orientation = quat(nodes, o);
    return out;
}

/* A Pose2D, the origin facing +x for no node. */
Pose2D placed_2d(const Nodes& nodes, int index)
{
    if (index < -1) return Pose2D{};
    const int p = kid(nodes, index, "position");
    return Pose2D{ p >= 0 ? vec_2d(nodes, p) : Double2{}, number(nodes, index, "angle") };
}

/* Roll, pitch and yaw in degrees. */
std::string euler(const Quaternion& q)
{
    const std::array<double, 3> a = math::quaternion_to_euler(q, math::EulerOrder::FixedXyz);
    const double deg = 180 / 3.14159265358979;
    return "roll " + print("%.1f\xC2\xB0", a[0] * deg) + "  pitch " + print("%.1f\xC2\xB0", a[1] * deg) +
           "  yaw " + print("%.1f\xC2\xB0", a[2] * deg);
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

/* The time axis of a strip: a line every two seconds unless the grid is hidden, each named
   along the bottom, now at the right. */
template <class X>
void time_grid(Canvas& c, X x_of, double now, float y0, float y1, bool grid)
{
    for (int sec = 2; sec <= 10; sec += 2) {
        const float x = x_of(now - sec);
        if (grid) c.line(x, y0, x, y1, ink.grid);
        c.text("-" + std::to_string(sec) + " s", x, c.h() - c.dp(6), sec == 10 ? Canvas::Left : Canvas::Center, ink.faint, LABEL);
    }
    c.text("now", x_of(now), c.h() - c.dp(6), Canvas::Right, ink.faint, LABEL);
}

/* Horizontal lines at round steps across a value range unless the grid is hidden, labelled
   in the left gutter. */
template <class Y>
void value_grid(Canvas& c, float x0, float x1, double lo, double hi, Y y_of, bool is_float, bool grid)
{
    const double step = nice((hi - lo) / 4);
    for (double v = std::ceil(lo / step) * step; v <= hi + 1e-9; v += step) {
        if (grid) c.line(x0, y_of(v), x1, y_of(v), ink.grid);
        c.text(axis_label(std::fabs(v) < step * 1e-6 ? 0 : v, is_float), x0 - c.dp(6), y_of(v) + c.dp(4), Canvas::Right, ink.faint, LABEL);
    }
}

/* The left gutter a value grid labels in. */
constexpr float GUTTER = 72;

/* A piece of a reading, in the colour of the line it names, else dim. */
struct Part {
    std::string  s;
    Rml::Colourb colour;
};

/* Every reading of a visual goes along the bottom of its area, in order from the left, and
   its context at the right, faint. What does not fit is left out, the context first, then
   the last readings. */
void draw_foot(Canvas& c, const Area& a, const std::vector<Part>& left, const std::string& right)
{
    const float room = a.w - c.dp(16), gap = c.text_width("  ", LABEL);
    std::vector<float> widths;
    float all = 0;
    for (const Part& part : left) {
        widths.push_back(part.s.empty() ? -gap : c.text_width(part.s, LABEL));
        all += widths.back() + gap;
    }
    const float rw = right.empty() ? 0 : c.text_width(right, LABEL);
    const bool context = !right.empty() && all + rw <= room;
    size_t n = left.size();
    for (; n > 0 && all - gap > room; n--) all -= widths[n - 1] + gap;
    float x = a.x + c.dp(8);
    const float y = a.y + a.h - c.dp(6);
    for (size_t i = 0; i < n; i++) {
        if (left[i].s.empty()) continue;
        c.text(left[i].s, x, y, Canvas::Left, left[i].colour, LABEL);
        x += widths[i] + gap;
    }
    if (context) c.text(right, a.x + a.w - c.dp(8), y, Canvas::Right, ink.faint, LABEL);
}

/* The area of a view over time down to its foot, which sits above the time labels. */
Area over_time(const Canvas& c)
{
    return Area{ 0, 0, c.w(), c.h() - c.dp(LINE) };
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

/* An element's index beside the point it is drawn at, in its colour. */
void index_label(Canvas& c, Rml::Vector2f at, size_t i, Rml::Colourb colour)
{
    c.text(std::to_string(i), at.x + c.dp(8), at.y - c.dp(6), Canvas::Left, colour, LABEL);
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

/* What a drawing shows around what it draws, as its card chose: the grid, the axes, and a
   scene's foot and drop line under each point. */
struct Backdrop {
    bool grid = true, axes = true, drops = true;
};

/* A view of a cube around the origin reaching r, z up, scaled so the reach meets the
   area's edge with the origin centred between the text strips. Depth is toward the
   viewer, so a scene can paint back to front. */
struct Scene {
    double   ca = 1, sa = 0, ce = 1, se = 0, k = 1, cx = 0, cy = 0, r = 1;
    Backdrop look;

    Rml::Vector2f operator()(double x, double y, double z) const
    {
        const double u = x * ca - y * sa, v = x * sa + y * ca;
        return { (float)(cx + u * k), (float)(cy + (v * se - z * ce) * k) };
    }
    Rml::Vector2f operator()(const Double3& p) const { return (*this)(p.x, p.y, p.z); }
    double depth(const Double3& p) const { return (p.x * sa + p.y * ca) * ce + p.z * se; }
    bool   front(const Double3& p) const { return depth(p) >= 0; }
};

Scene scene(Canvas& c, const Area& a, double r, const Camera& cam, const Backdrop& look)
{
    const double rad = 3.14159265358979 / 180;
    Scene s;
    s.look = look;
    s.ca = std::cos(cam.a * rad); s.sa = std::sin(cam.a * rad);
    s.ce = std::cos(cam.e * rad); s.se = std::sin(cam.e * rad);
    s.r  = r;
    s.k  = std::min(a.w - c.dp(16), a.h - c.dp(TOP + BOT + 4)) / (2 * r) * cam.zoom;
    s.cx = a.x + a.w / 2;
    s.cy = a.y + c.dp(TOP) + (a.h - c.dp(TOP + BOT)) / 2;
    /* the floor grid at a round step, running off the sides where it will */
    const double step = nice(2 * r / 4);
    for (double m = std::ceil(-r / step) * step; look.grid && m <= r + 1e-9; m += step) {
        c.line(s(m, -r, 0), s(m, r, 0), ink.grid);
        c.line(s(-r, m, 0), s(r, m, 0), ink.grid);
    }
    return s;
}

/* The axes from the origin toward the reach, each named at its end. */
void axes(Canvas& c, const Scene& s)
{
    if (!s.look.axes) return;
    const double r = s.r * 0.92;
    const Double3 ends[3] = { { r, 0, 0 }, { 0, r, 0 }, { 0, 0, r } };
    const char* names[3] = { "x", "y", "z" };
    for (int i = 0; i < 3; i++) {
        c.line(s(0, 0, 0), s(ends[i]), axis_colour(i), THIN);
        const Rml::Vector2f e = s(ends[i]);
        c.text(names[i], e.x + (i == 2 ? 0 : c.dp(8)), e.y + (i == 2 ? -c.dp(6) : c.dp(5)), Canvas::Center, axis_colour(i), LABEL);
    }
}

/* A point's trail in the air, and its foot on the floor with a dashed line up to it,
   the part on one side of the depth split. Nothing else is projected. */
void shadow(Canvas& c, const Scene& s, const Double3& v, const std::vector<Double3>* trail, bool front)
{
    if (trail) {
        Dots dots(c, ink.trail, 2);
        for (const Double3& p : *trail)
            if (s.front(p) == front) dots.add(s(p));
    }
    if (s.front(v) != front || !s.look.drops) return;
    c.dot(s(v.x, v.y, 0), ink.ground, 3);
    c.dashed(s(v.x, v.y, 0), s(v), ink.ground, THIN);
}

/* Draws a scene back to front: what is behind, the axes, what is in front. */
template <class Body>
void layered(Canvas& c, const Scene& s, const Double3& v, const std::vector<Double3>* trail, Body body)
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
void back_to_front(Canvas& c, const Scene& s, const std::vector<Double3>& at, Body body)
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
void triad(Canvas& c, const Scene& s, const Double3& at, const Quaternion& q, double len)
{
    const Double3 units[3] = { { len, 0, 0 }, { 0, len, 0 }, { 0, 0, len } };
    for (int i = 0; i < 3; i++) c.arrow(s(at), s(at + q * units[i]), axis_colour(i), ARROW);
}

/* ------------------------------------------------------------------ solids */

/* Two unit vectors square to an axis and to each other. A zero axis is taken as z. */
void basis(const Double3& axis, Double3& a, Double3& b)
{
    Double3 n = math::double3_normalize(axis);
    if (math::double3_length_squared(n) == 0) n = Double3{ 0, 0, 1 };
    const Double3 up = std::fabs(n.z) < 0.9 ? Double3{ 0, 0, 1 } : Double3{ 1, 0, 0 };
    a = math::double3_normalize(math::double3_cross(up, n));
    b = math::double3_cross(n, a);
}

/* A circle in space around an axis, as screen points. */
std::vector<Rml::Vector2f> ring(const Scene& s, const Double3& centre, const Double3& axis, double radius)
{
    Double3 a, b;
    basis(axis, a, b);
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

/* A standard 3D shape read from its node, nothing for a kind with no drawing. */
using Solid = std::variant<std::monostate, types::AlignedBox, types::OrientedBox, types::Plane, types::Segment,
                           types::Sphere, types::Capsule, types::Cylinder, types::Cone, types::Polygon>;

Solid solid(const Nodes& nodes, int index, const std::string& kind)
{
    auto at = [&](const char* name) {
        const int k = kid(nodes, index, name);
        return k >= 0 ? vec(nodes, k) : Double3{};
    };
    auto axis = [&] {
        const int k = kid(nodes, index, "axis");
        return k >= 0 ? types::Segment{ vec(nodes, kid(nodes, k, "a")), vec(nodes, kid(nodes, k, "b")) } : types::Segment{};
    };
    const double radius = number(nodes, index, "radius");
    if (kind == "AlignedBox")  return types::AlignedBox{ at("min"), at("max") };
    if (kind == "OrientedBox") return types::OrientedBox{ placed(nodes, kid(nodes, index, "pose")), at("size") };
    if (kind == "Plane")       return types::Plane{ at("position"), at("normal") };
    if (kind == "Segment")     return types::Segment{ at("a"), at("b") };
    if (kind == "Sphere")      return types::Sphere{ at("center"), radius };
    if (kind == "Capsule")     return types::Capsule{ axis(), radius };
    if (kind == "Cylinder")    return types::Cylinder{ axis(), radius };
    if (kind == "Cone")        return types::Cone{ at("base"), at("tip"), radius };
    if (kind == "Polygon") {
        types::Polygon out;
        const int points = kid(nodes, index, "points");
        if (points >= 0)
            for (const int k : children_of(nodes, points)) out.points.push_back(vec(nodes, k));
        return out;
    }
    return std::monostate{};
}

/* Where a solid's label and drop line go. */
Double3 middle_of(const std::monostate&) { return {}; }
Double3 middle_of(const types::AlignedBox& b) { return math::aligned_box_centroid(b); }
Double3 middle_of(const types::OrientedBox& b) { return b.pose.position; }
Double3 middle_of(const types::Plane& p) { return p.position; }
Double3 middle_of(const types::Segment& l) { return math::double3_lerp(l.a, l.b, 0.5); }
Double3 middle_of(const types::Sphere& o) { return o.center; }
Double3 middle_of(const types::Capsule& o) { return math::capsule_centroid(o); }
Double3 middle_of(const types::Cylinder& o) { return math::cylinder_centroid(o); }
Double3 middle_of(const types::Cone& o) { return math::double3_lerp(o.base, o.tip, 0.5); }
Double3 middle_of(const types::Polygon& p) { return math::polygon_centroid(p.points); }

Double3 middle(const Solid& o)
{
    return std::visit([](const auto& v) { return middle_of(v); }, o);
}

/* Grows a reach, a distance from the origin, to hold a solid. A plane has no end, so its patch
   is drawn at a share of the reach, held by keeping its point inside half of it. */
void take(Reach& reach, const Double3& p, double pad)
{
    reach.seen = std::max(reach.seen, math::double3_length(p) + pad);
}

void hold_one(Reach&, const std::monostate&) {}
void hold_one(Reach& r, const types::AlignedBox& b) { for (const Double3& p : math::aligned_box_corners(b)) take(r, p, 0); }
void hold_one(Reach& r, const types::OrientedBox& b) { for (const Double3& p : math::oriented_box_corners(b)) take(r, p, 0); }
void hold_one(Reach& r, const types::Plane& p) { take(r, p.position * 2, 0); }
void hold_one(Reach& r, const types::Segment& l) { take(r, l.a, 0); take(r, l.b, 0); }
void hold_one(Reach& r, const types::Sphere& o) { take(r, o.center, o.radius); }
void hold_one(Reach& r, const types::Capsule& o) { take(r, o.axis.a, o.radius); take(r, o.axis.b, o.radius); }
void hold_one(Reach& r, const types::Cylinder& o) { take(r, o.axis.a, o.radius); take(r, o.axis.b, o.radius); }
void hold_one(Reach& r, const types::Cone& o) { take(r, o.base, o.radius); take(r, o.tip, 0); }
void hold_one(Reach& r, const types::Polygon& p) { for (const Double3& v : p.points) take(r, v, 0); }

void hold(Reach& reach, const Solid& o)
{
    std::visit([&](const auto& v) { hold_one(reach, v); }, o);
}

/* A solid as lines: the edges of a box, the outline and rings of a round one, a plane as a
   square patch with its normal. */
void edges(Canvas& c, const Scene& s, const std::array<types::Segment, 12>& lines, Rml::Colourb colour)
{
    for (const types::Segment& l : lines) c.line(s(l.a), s(l.b), colour, STROKE);
}

void draw_one(Canvas&, const Scene&, const std::monostate&, Rml::Colourb) {}
void draw_one(Canvas& c, const Scene& s, const types::AlignedBox& b, Rml::Colourb colour) { edges(c, s, math::aligned_box_edges(b), colour); }
void draw_one(Canvas& c, const Scene& s, const types::OrientedBox& b, Rml::Colourb colour) { edges(c, s, math::oriented_box_edges(b), colour); }

void draw_one(Canvas& c, const Scene& s, const types::Plane& o, Rml::Colourb colour)
{
    Double3 u, v;
    basis(o.normal, u, v);
    const double h = 0.35 * s.r;
    const Double3& at = o.position;
    c.polyline({ s(at + (u + v) * h), s(at + (u - v) * h), s(at - (u + v) * h), s(at - (u - v) * h) }, colour, STROKE, true);
    c.line(s(at - u * h), s(at + u * h), faded(colour, 0.5f), THIN);
    c.line(s(at - v * h), s(at + v * h), faded(colour, 0.5f), THIN);
    c.arrow(s(at), s(at + math::double3_normalize(o.normal) * (0.3 * s.r)), colour, ARROW);
}

void draw_one(Canvas& c, const Scene& s, const types::Segment& l, Rml::Colourb colour)
{
    c.line(s(l.a), s(l.b), colour, STROKE);
    c.dot(s(l.a), colour, 4);
    c.dot(s(l.b), colour, 4);
}

void draw_one(Canvas& c, const Scene& s, const types::Sphere& o, Rml::Colourb colour)
{
    c.polyline(circle(s(o.center), (float)(o.radius * s.k)), colour, STROKE);
    c.polyline(ring(s, o.center, Double3{ 0, 0, 1 }, o.radius), faded(colour, 0.5f), THIN, true);
}

/* A cylinder or a capsule: a ring at each end, and their outline along the axis. */
void draw_round(Canvas& c, const Scene& s, const types::Segment& axis, double radius, bool capsule, Rml::Colourb colour)
{
    const float rk = (float)(radius * s.k);
    const Rml::Vector2f a = s(axis.a), b = s(axis.b), d = b - a;
    const float len = std::sqrt(d.x * d.x + d.y * d.y);
    const std::vector<Rml::Vector2f> ra = ring(s, axis.a, axis.b - axis.a, radius), rb = ring(s, axis.b, axis.b - axis.a, radius);
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
}

void draw_one(Canvas& c, const Scene& s, const types::Capsule& o, Rml::Colourb colour) { draw_round(c, s, o.axis, o.radius, true, colour); }
void draw_one(Canvas& c, const Scene& s, const types::Cylinder& o, Rml::Colourb colour) { draw_round(c, s, o.axis, o.radius, false, colour); }

void draw_one(Canvas& c, const Scene& s, const types::Cone& o, Rml::Colourb colour)
{
    const std::vector<Rml::Vector2f> base = ring(s, o.base, o.tip - o.base, o.radius);
    c.polyline(base, colour, STROKE, true);
    /* the lines from the tip touch the base where they turn furthest each way */
    const Rml::Vector2f tip = s(o.tip), toward = s(o.base) - tip;
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
}

void draw_one(Canvas& c, const Scene& s, const types::Polygon& o, Rml::Colourb colour)
{
    std::vector<Rml::Vector2f> p;
    for (const Double3& v : o.points) p.push_back(s(v));
    c.polyline(p, colour, STROKE, true);
    for (const Rml::Vector2f& v : p) c.dot(v, colour, 3.5f);
}

void draw_solid(Canvas& c, const Scene& s, const Solid& o, Rml::Colourb colour)
{
    std::visit([&](const auto& v) { draw_one(c, s, v, colour); }, o);
}

std::string point(const Double3& p)
{
    return "(" + f3(p.x) + ", " + f3(p.y) + ", " + f3(p.z) + ")";
}

std::string point(const Double2& p)
{
    return "(" + f3(p.x) + ", " + f3(p.y) + ")";
}

/* What a shape measures, and where it is, for the bottom of its card. */
using Described = std::pair<std::string, std::string>;

std::string radius_text(double r) { return "radius " + f3(r); }

Described describe_one(const std::monostate&) { return {}; }
Described describe_one(const types::AlignedBox& b)
{
    const Double3 size = b.max - b.min;
    return { "size " + f3(size.x) + " x " + f3(size.y) + " x " + f3(size.z), "at " + point(math::aligned_box_centroid(b)) };
}
Described describe_one(const types::OrientedBox& b)
{
    return { "size " + f3(b.size.x) + " x " + f3(b.size.y) + " x " + f3(b.size.z), "at " + point(b.pose.position) };
}
Described describe_one(const types::Plane& p) { return { "normal " + point(math::double3_normalize(p.normal)), "at " + point(p.position) }; }
Described describe_one(const types::Segment& l) { return { "length " + f3(math::segment_length(l)), "from " + point(l.a) }; }
Described describe_one(const types::Sphere& o) { return { radius_text(o.radius), "at " + point(o.center) }; }
Described describe_one(const types::Capsule& o)
{
    return { radius_text(o.radius) + "  length " + f3(math::segment_length(o.axis)), "from " + point(o.axis.a) };
}
Described describe_one(const types::Cylinder& o)
{
    return { radius_text(o.radius) + "  length " + f3(math::segment_length(o.axis)), "from " + point(o.axis.a) };
}
Described describe_one(const types::Cone& o)
{
    return { radius_text(o.radius) + "  height " + f3(math::double3_distance(o.base, o.tip)), "base " + point(o.base) };
}
Described describe_one(const types::Polygon& p) { return { format_number((double)p.points.size()) + " points", {} }; }

Described describe(const Solid& o)
{
    return std::visit([](const auto& v) { return describe_one(v); }, o);
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
    double k = 1, cx = 0, cy = 0, r = 1;
    Rml::Vector2f operator()(double x, double y) const { return { (float)(cx + x * k), (float)(cy - y * k) }; }
    Rml::Vector2f operator()(const Double2& p) const { return (*this)(p.x, p.y); }
};

Plane plane(Canvas& c, const Area& a, double r, const Backdrop& look)
{
    Plane p;
    p.r  = r;
    p.k  = std::min(a.w - c.dp(16), a.h - c.dp(TOP + BOT + 4)) / (2 * r);
    p.cx = a.x + a.w / 2;
    p.cy = a.y + c.dp(TOP) + (a.h - c.dp(TOP + BOT)) / 2;
    const double step = nice(r / 2);
    for (double m = std::ceil(-r / step) * step; look.grid && m <= r + 1e-9; m += step) {
        c.line(p(m, -r), p(m, r), ink.grid);
        c.line(p(-r, m), p(r, m), ink.grid);
    }
    if (!look.axes) return p;
    const double end = r * 0.92;
    c.line(p(-r, 0), p(end, 0), axis_colour(0), THIN);
    c.line(p(0, -r), p(0, end), axis_colour(1), THIN);
    c.text("x", p(end, 0).x + c.dp(8), p(end, 0).y + c.dp(5), Canvas::Center, axis_colour(0), LABEL);
    c.text("y", p(0, end).x, p(0, end).y - c.dp(6), Canvas::Center, axis_colour(1), LABEL);
    return p;
}

/* A pose on a plane: a dot where it is and an arrow along its heading. */
void heading(Canvas& c, const Plane& p, const Pose2D& at, double len, Rml::Colourb colour)
{
    c.arrow(p(at.position), p(at.position + math::double2_from_heading(at.angle) * len), colour, ARROW);
    c.dot(p(at.position), colour, 4);
}

/* An angle in degrees, wrapped to a half turn either way. */
std::string degrees(double radians)
{
    return print("%.1f\xC2\xB0", math::angle_wrap(radians) * 180 / 3.14159265358979);
}

/* A standard 2D shape read from its node, nothing for a kind with no drawing. */
using Figure = std::variant<std::monostate, types::AlignedBox2D, types::OrientedBox2D, types::Circle, types::Polygon2D,
                            types::Plane2D, types::Segment2D, types::Capsule2D>;

Figure figure(const Nodes& nodes, int index, const std::string& kind)
{
    auto at = [&](const char* name) {
        const int k = kid(nodes, index, name);
        return k >= 0 ? vec_2d(nodes, k) : Double2{};
    };
    const double radius = number(nodes, index, "radius");
    if (kind == "AlignedBox2D")  return types::AlignedBox2D{ at("min"), at("max") };
    if (kind == "OrientedBox2D") return types::OrientedBox2D{ placed_2d(nodes, kid(nodes, index, "pose")), at("size") };
    if (kind == "Circle")        return types::Circle{ at("center"), radius };
    if (kind == "Plane2D")       return types::Plane2D{ at("position"), at("normal") };
    if (kind == "Segment2D")     return types::Segment2D{ at("a"), at("b") };
    if (kind == "Capsule2D") {
        const int k = kid(nodes, index, "axis");
        const types::Segment2D axis = k >= 0 ? types::Segment2D{ vec_2d(nodes, kid(nodes, k, "a")), vec_2d(nodes, kid(nodes, k, "b")) }
                                             : types::Segment2D{};
        return types::Capsule2D{ axis, radius };
    }
    if (kind == "Polygon2D") {
        types::Polygon2D out;
        const int points = kid(nodes, index, "points");
        if (points >= 0)
            for (const int k : children_of(nodes, points)) out.points.push_back(vec_2d(nodes, k));
        return out;
    }
    return std::monostate{};
}

/* Where a figure's label goes. */
Double2 middle_of(const types::AlignedBox2D& b) { return math::aligned_box2d_centroid(b); }
Double2 middle_of(const types::OrientedBox2D& b) { return b.pose.position; }
Double2 middle_of(const types::Circle& o) { return o.center; }
Double2 middle_of(const types::Polygon2D& p) { return math::polygon2d_centroid(p.points); }
Double2 middle_of(const types::Plane2D& p) { return p.position; }
Double2 middle_of(const types::Segment2D& l) { return math::double2_lerp(l.a, l.b, 0.5); }
Double2 middle_of(const types::Capsule2D& o) { return math::double2_lerp(o.axis.a, o.axis.b, 0.5); }

Double2 middle(const Figure& f)
{
    return std::visit([](const auto& v) {
        const auto m = middle_of(v);
        return Double2{ m.x, m.y };
    }, f);
}

/* A capsule's outline: its sides and the half circles around both ends. */
std::vector<Double2> stadium(const types::Capsule2D& o)
{
    const double len = math::segment2d_length(o.axis);
    const Double2 d = len > 1e-12 ? (o.axis.b - o.axis.a) * (1 / len) : Double2{ 1, 0 };
    const double side = std::atan2(d.x, -d.y);   /* the left of the axis, from +x */
    std::vector<Double2> out;
    for (const auto& end : { std::make_pair(o.axis.b, side), std::make_pair(o.axis.a, side + 3.14159265358979) })
        for (int i = 0; i <= 24; i++)
            out.push_back(end.first + math::double2_from_heading(end.second - 3.14159265358979 * i / 24) * o.radius);
    return out;
}

/* Grows a plane's reach, a largest component, to hold a figure. */
void take_2d(Reach& reach, const Double2& p, double pad)
{
    reach.seen = std::max({ reach.seen, std::fabs(p.x) + pad, std::fabs(p.y) + pad });
}

void hold_one(Reach& r, const types::AlignedBox2D& b) { for (const Double2& p : math::aligned_box2d_corners(b)) take_2d(r, p, 0); }
void hold_one(Reach& r, const types::OrientedBox2D& b) { for (const Double2& p : math::oriented_box2d_corners(b)) take_2d(r, p, 0); }
void hold_one(Reach& r, const types::Circle& o) { take_2d(r, o.center, o.radius); }
void hold_one(Reach& r, const types::Polygon2D& p) { for (const Double2& v : p.points) take_2d(r, v, 0); }
/* its point at most half way out, so the line shows */
void hold_one(Reach& r, const types::Plane2D& p) { take_2d(r, p.position * 2, 0); }
void hold_one(Reach& r, const types::Segment2D& l) { take_2d(r, l.a, 0); take_2d(r, l.b, 0); }
void hold_one(Reach& r, const types::Capsule2D& o) { take_2d(r, o.axis.a, o.radius); take_2d(r, o.axis.b, o.radius); }

void hold(Reach& reach, const Figure& f)
{
    std::visit([&](const auto& v) { hold_one(reach, v); }, f);
}

void outline(Canvas& c, const Plane& p, const std::array<Double2, 4>& corners, Rml::Colourb colour)
{
    std::vector<Rml::Vector2f> drawn;
    for (const Double2& v : corners) drawn.push_back(p(v));
    c.polyline(drawn, colour, STROKE, true);
}

void draw_one(Canvas&, const Plane&, const std::monostate&, Rml::Colourb) {}
void draw_one(Canvas& c, const Plane& p, const types::AlignedBox2D& b, Rml::Colourb colour) { outline(c, p, math::aligned_box2d_corners(b), colour); }
void draw_one(Canvas& c, const Plane& p, const types::OrientedBox2D& b, Rml::Colourb colour) { outline(c, p, math::oriented_box2d_corners(b), colour); }

void draw_one(Canvas& c, const Plane& p, const types::Circle& o, Rml::Colourb colour)
{
    c.polyline(circle(p(o.center), (float)(o.radius * p.k)), colour, STROKE);
    c.dot(p(o.center), colour, 3);
}

void draw_one(Canvas& c, const Plane& p, const types::Plane2D& o, Rml::Colourb colour)
{
    /* the endless line cut to the grid, and its normal from its point */
    Double2 n = math::double2_normalize(o.normal);
    if (math::double2_length(n) == 0) n = Double2{ 0, 1 };
    const Double2 along = math::double2_perpendicular(n);
    Rml::Vector2f a = p(o.position + along * (-4 * p.r)), b = p(o.position + along * (4 * p.r));
    const Rml::Vector2f lo = p(-p.r, p.r), hi = p(p.r, -p.r);
    if (clip(a.x, a.y, b.x, b.y, lo.x, lo.y, hi.x, hi.y)) c.line(a, b, colour, STROKE);
    c.arrow(p(o.position), p(o.position + n * (0.2 * p.r)), colour, STROKE);
    c.dot(p(o.position), colour, 3);
}

void draw_one(Canvas& c, const Plane& p, const types::Segment2D& l, Rml::Colourb colour)
{
    c.line(p(l.a), p(l.b), colour, STROKE);
    c.dot(p(l.a), colour, 3.5f);
    c.dot(p(l.b), colour, 3.5f);
}

void draw_one(Canvas& c, const Plane& p, const types::Capsule2D& o, Rml::Colourb colour)
{
    std::vector<Rml::Vector2f> drawn;
    for (const Double2& v : stadium(o)) drawn.push_back(p(v));
    c.polyline(drawn, colour, STROKE, true);
    c.dashed(p(o.axis.a), p(o.axis.b), faded(colour, 0.6f), THIN);
}

void draw_one(Canvas& c, const Plane& p, const types::Polygon2D& o, Rml::Colourb colour)
{
    std::vector<Rml::Vector2f> drawn;
    for (const Double2& v : o.points) drawn.push_back(p(v));
    c.polyline(drawn, colour, STROKE, true);
    for (const Rml::Vector2f& v : drawn) c.dot(v, colour, 3.5f);
}

void draw_figure(Canvas& c, const Plane& p, const Figure& f, Rml::Colourb colour)
{
    std::visit([&](const auto& v) { draw_one(c, p, v, colour); }, f);
}

Described describe_one(const types::AlignedBox2D& b)
{
    const Double2 size = b.max - b.min;
    return { "size " + f3(size.x) + " x " + f3(size.y), "at " + point(math::aligned_box2d_centroid(b)) };
}
Described describe_one(const types::OrientedBox2D& b) { return { "size " + f3(b.size.x) + " x " + f3(b.size.y), "at " + point(b.pose.position) }; }
Described describe_one(const types::Circle& o) { return { radius_text(o.radius), "at " + point(o.center) }; }
Described describe_one(const types::Polygon2D& p) { return { format_number((double)p.points.size()) + " points", {} }; }
Described describe_one(const types::Plane2D& p) { return { "normal " + point(math::double2_normalize(p.normal)), "at " + point(p.position) }; }
Described describe_one(const types::Segment2D& l) { return { "length " + f3(math::segment2d_length(l)), "from " + point(l.a) }; }
Described describe_one(const types::Capsule2D& o)
{
    return { radius_text(o.radius) + "  length " + f3(math::segment2d_length(o.axis)), "from " + point(o.axis.a) };
}

Described describe(const Figure& f)
{
    return std::visit([](const auto& v) { return describe_one(v); }, f);
}

/* A turn about an axis through the origin: an arc of radius rho sweeping sweep radians the
   way the turn goes by the right hand rule, with a head at its end. */
void curl(Canvas& c, const Scene& s, const Double3& axis, double rho, double sweep, Rml::Colourb colour)
{
    Double3 a, b;
    basis(axis, a, b);
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

/* A turn on a plane: an arc about centre of radius rho, sweeping sweep radians from +x,
   toward +y when positive, with a head at its end. */
void curl_2d(Canvas& c, const Plane& p, const Double2& centre, double rho, double sweep, Rml::Colourb colour)
{
    const int steps = 32;
    std::vector<Rml::Vector2f> arc;
    for (int i = 0; i <= steps; i++) arc.push_back(p(centre + math::double2_from_heading(sweep * i / steps) * rho));
    const Rml::Vector2f tip = arc.back();
    arc.pop_back();
    c.polyline(arc, colour, STROKE);
    c.arrow(arc.back(), tip, colour, STROKE);
}

/* ------------------------------------------------------------------ the element */

/* What a visual draws into: the canvas, a scene the card orbits, a picture it pans and
   zooms, or child elements. */
enum class Kind { Canvas, Scene, Picture, Html };

/* What a card's history traces: the named fields of the value, or of its field under, or
   the value itself when none are named. A stream decodes the value's every frame instead. */
struct Feed {
    double      seconds;
    const char* under;
    const char* names[4];
    bool        stream;
};

/* The trace keys a feed fills for the value at path. A Transform traces its pose. */
std::vector<std::string> fed_paths(const Feed& feed, const std::string& path, const std::string& std_name)
{
    std::string at = std_name == "Transform" || std_name == "Transform2D" ? child_path(path, "pose") : path;
    if (feed.under) at = child_path(at, feed.under);
    std::vector<std::string> out;
    for (const char* name : feed.names)
        if (name) out.push_back(child_path(at, name));
    if (out.empty()) out.push_back(at);
    return out;
}

/* A table wants the box its columns and rows fill. Past TABLE_ROWS it asks for that
   height and scrolls inside. */
constexpr double TABLE_COL = 100, TABLE_ROW = 24;
constexpr size_t TABLE_ROWS = 12;

Tile grid_tile(size_t cols, size_t rows)
{
    const size_t shown = std::max<size_t>(1, std::min(rows, TABLE_ROWS));
    return Tile{ std::max<size_t>(1, cols) * TABLE_COL / ((shown + 1) * TABLE_ROW), 1 };
}

class VizElement : public Rml::Element, public Rml::EventListener {
public:
    using Paint = void (VizElement::*)(Canvas&, const Nodes&, const ValueNode&);
    using Fill  = void (VizElement::*)(const Nodes&, const ValueNode&);
    using Fit   = Tile (VizElement::*)(const Nodes*) const;

    /* What a card wants from the tiling: a fixed tile, or what fit measures from the value. */
    struct Size {
        Tile tile;
        Fit  fit = nullptr;
        constexpr Size(double aspect, double stiff) : tile{ aspect, stiff } {}
        constexpr Size(Fit f) : fit(f) {}
    };

    /* How a visual draws: paint each frame, or fill its child elements as the value changes. */
    struct Draw {
        Paint paint = nullptr;
        Fill  fill  = nullptr;
        constexpr Draw() = default;
        constexpr Draw(Paint p) : paint(p) {}
        constexpr Draw(Fill f) : fill(f) {}
    };

    /* One visual: what it draws into and how, its tile, the parts its card offers and which
       of them start hidden, and what its history traces. */
    struct Row {
        Visual   visual;
        Kind     kind;
        Size     size;
        Draw     draw;
        unsigned parts;    /* a bit per Show, which the menu lists in Show's order */
        unsigned hidden;
        Feed     feed;

        /* The parts that show, a bit per Show, by the card's choices over the defaults. */
        unsigned shown(const ShowChoices& choices) const
        {
            unsigned out = 0;
            for (size_t s = 0; s < std::size(SHOW_NAMES); s++) {
                if (!(parts >> s & 1)) continue;
                const auto chosen = choices.find(SHOW_NAMES[s].key);
                if (chosen != choices.end() ? chosen->second : !(hidden >> s & 1)) out |= 1u << s;
            }
            return out;
        }
    };

    static const Row& row_of(Visual v);

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
        const Kind kind = row_of(visual_).kind;
        if (kind != Kind::Scene && kind != Kind::Picture) return;
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
        found_   = -2;
        gathers_ = is_gathered(path);
        Visual visual = Visual::None;
        bool   empty  = false;   /* the value came but its arrays hold nothing to gather */
        const Capture::WatchView* watch = source ? source->view(topic_) : nullptr;
        if (watch) {
            if (gathers_) {
                const Nodes& whole = source->packed(topic_);
                const std::string key = std::to_string(watch->seq) + ":" + std::to_string(whole.size()) + ":" + path;
                if (key != gathered_key_) {
                    gathered_key_ = key;
                    gathered_     = gather(whole, path);
                }
                empty = !whole.empty() && gathered_.empty();
            }
            const Nodes& nodes = this->nodes();
            found_ = find_node(nodes, *watch, path);
            visual = visual_at(*watch, nodes, found_);
            if (found_ == -1) {
                held_          = ValueNode();
                held_.kind     = ValueNode::Struct;
                held_.std_name = watch->root_std;
                held_.depth    = -1;
            } else if (found_ == -2 && packed_element(nodes, path, watch->message, held_)) {
                found_ = -1;
                visual = visual_for(held_);
            }
        }
        if (path != path_ || visual != visual_ || !built_) build(path, visual);
        const std::string shows = GetAttribute<Rml::String>("shows", "");
        if (shows != shows_text_) {
            shows_text_ = shows;
            shown_      = row_of(visual_).shown(read_choices(shows));
        }
        if (visual_ == Visual::None)
            html(empty ? "empty" : "none", std::string("<span class=\"faint\">") +
                                               (empty ? "no elements" : "waiting for a value") + "</span>");
        const Fill fill = row_of(visual_).draw.fill;
        if (found_ >= -1 && fill) {
            const Nodes& nodes = this->nodes();
            (this->*fill)(nodes, found_ < 0 ? held_ : nodes[found_]);
        }
    }

    void OnRender() override
    {
        Rml::Element::OnRender();
        const Paint paint = row_of(visual_).draw.paint;
        if (!source || found_ < -1 || !paint) return;
        const Nodes& nodes = this->nodes();
        if (found_ >= (int)nodes.size()) return;

        const double now = Capture::now_s();
        dt_ = last_frame_ > 0 ? std::min(now - last_frame_, 0.5) : 1.0;
        last_frame_ = now;

        Canvas c(*this, shapes_);
        if (c.w() < 1 || c.h() < 1) return;
        read_ink(*this);

        (this->*paint)(c, nodes, found_ < 0 ? held_ : nodes[found_]);
        c.render();
        turn_  = Rml::Vector2f(0, 0);
        wheel_ = 0;
        reset_ = false;
    }

private:
    static constexpr Rml::EventId GESTURES[] = { Rml::EventId::Dragstart, Rml::EventId::Drag,
                                                 Rml::EventId::Mousescroll, Rml::EventId::Dblclick };

    /* The value the path is found in: the topic's whole value packed, or what the path
       gathers from it. */
    const Nodes& nodes() const
    {
        return gathers_ ? gathered_ : source->packed(topic_);
    }

    /* The message the nodes' packed arrays lie in. */
    const std::vector<uint8_t>& message() const
    {
        static const std::vector<uint8_t> none;
        const Capture::WatchView* watch = source ? source->view(topic_) : nullptr;
        return watch ? watch->message : none;
    }

    bool on(Show part) const
    {
        return (shown_ >> (int)part & 1) != 0;
    }

    Backdrop backdrop() const
    {
        return Backdrop{ on(Show::Grid), on(Show::Axes), on(Show::Drops) };
    }

    /* An element's own colour, or one for all when the card hides colours. */
    Rml::Colourb tint(size_t i) const
    {
        return on(Show::Colours) ? palette((long long)i) : ink.accent;
    }

    /* An element's index beside it, in its colour, else dim. */
    void label(Canvas& c, Rml::Vector2f at, size_t i) const
    {
        if (on(Show::Labels)) index_label(c, at, i, on(Show::Colours) ? palette((long long)i) : ink.dim);
    }

    /* A vector as an arrow, or as a dot at its tip when the card hides arrows. */
    void ray(Canvas& c, Rml::Vector2f from, Rml::Vector2f to, Rml::Colourb colour) const
    {
        if (on(Show::Arrows)) c.arrow(from, to, colour, ARROW);
        else                  c.dot(to, colour, 4);
    }

    /* The readings along the bottom, unless the card hides them. */
    void foot(Canvas& c, const Area& a, const std::vector<Part>& left, const std::string& right = {}) const
    {
        if (on(Show::Readings)) draw_foot(c, a, left, right);
    }

    void foot(Canvas& c, const Area& a, const std::string& left, const std::string& right = {}) const
    {
        foot(c, a, std::vector<Part>{ { left, ink.dim } }, right);
    }

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
        shows_text_.assign(1, '\x01');   /* the new visual's parts are read again */
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

        const Kind kind = row_of(visual).kind;
        SetClass("html", kind == Kind::Html);
        SetClass("orbit", kind == Kind::Scene);
        SetClass("pans", kind == Kind::Picture);
        view_frame_ = Rml::Vector2f(0, 0);   /* the next picture starts fitted */
        if (kind != Kind::Html) SetInnerRML("");
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

    /* ---------------------------------------------------------------- html */

    void text(const Nodes&, const ValueNode& node)
    {
        html("text", "<div class=\"big text mono slot\"></div>");
        put(0, value_text(node));
    }

    void time(const Nodes&, const ValueNode& node)
    {
        html("big", "<div class=\"big mono slot\"></div>");
        put(0, format_timestamp(node.integer));
    }

    void duration(const Nodes&, const ValueNode& node)
    {
        html("big", "<div class=\"big mono slot\"></div>");
        put(0, format_span(node.integer));
    }

    void uuid(const Nodes& nodes, const ValueNode& node)
    {
        html("uuid", "<div class=\"big mono slot\"></div>");
        put(0, uuid_text(nodes, found_, message()), node.path);
    }

    /* A stream handed over by URL: its name, the URL, and the hints about it. */
    void stream(const Nodes& nodes, const ValueNode&)
    {
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
    }

    void hex(const Nodes&, const ValueNode& node)
    {
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
    }

    /* A chip per element. JointNames shows its names, each numbered as a JointState counts its joints. */
    void chips(const Nodes& nodes, const ValueNode& node)
    {
        const bool named = node.kind == ValueNode::Struct;
        std::vector<std::pair<std::string, std::string>> chips;   /* each text and path */
        each_element(nodes, named ? kid(nodes, found_, "name") : found_, message(), [&](const ValueNode& e, int) {
            chips.emplace_back((named ? std::to_string(chips.size()) + " " : "") + human(e), e.path);
            return true;
        });
        std::string rml = "<div class=\"chips\">";
        for (size_t i = 0; i < chips.size(); i++) rml += "<span class=\"chip mono slot\"></span>";
        if (chips.empty()) rml += "<span class=\"faint\">empty</span>";
        html("chips:" + std::to_string(chips.size()), rml + "</div>");
        for (size_t i = 0; i < chips.size(); i++) put(i, chips[i].first, chips[i].second);
    }

    void kv(const Nodes& nodes, const ValueNode&)
    {
        const std::vector<int> children = children_of(nodes, found_);
        const std::string count = std::to_string(children.size());
        std::string rml = "<div class=\"pairs\">";
        for (size_t i = 0; i < children.size(); i++)
            rml += "<div class=\"pair\"><span class=\"pair-key slot\"></span><span class=\"pair-value slot\"></span></div>";
        html("kv:" + count, rml + "</div>");
        for (size_t i = 0; i < children.size(); i++) {
            const ValueNode& member = nodes[children[i]];
            put(i * 2, member.name);
            put(i * 2 + 1, brief(nodes, member, children[i], message()), member.path);
        }
    }

    void cells(const Nodes& nodes, const ValueNode&)
    {
        const std::vector<double> values = numbers_of(nodes, found_, message());
        const std::string count = std::to_string(values.size());
        std::string rml = "<div class=\"cells\">";
        for (size_t i = 0; i < values.size(); i++)
            rml += "<span class=\"cell\"><span class=\"lamp\"></span><span class=\"idx mono\">" + std::to_string(i) + "</span></span>";
        html("cells:" + count, rml + "</div>");
        for (size_t i = 0; i < values.size() && i < lamps_.size(); i++) {
            const int on = values[i] != 0 ? 1 : 0;
            if (lamp_states_[i] == on) continue;
            lamp_states_[i] = on;
            lamps_[i]->SetClass("on", on != 0);
        }
    }

    /* Each child is a row, an array of its numbers. */
    void matrix(const Nodes& nodes, const ValueNode&)
    {
        const std::vector<int> children = children_of(nodes, found_);
        const std::string count = std::to_string(children.size());
        std::string rml = "<div class=\"matrix mono\">";
        for (const int row : children) {
            rml += "<div class=\"matrix-row\">";
            for (size_t col = 0; col < element_count(nodes, row); col++) rml += "<span><span class=\"slot\"></span></span>";
            rml += "</div>";
        }
        html("matrix:" + count, rml + "</div>");
        size_t slot = 0;
        for (const int row : children)
            each_element(nodes, row, message(), [&](const ValueNode& cell, int) {
                put(slot++, value_text(cell), cell.path);
                return true;
            });
    }

    /* A column per member of the element, a row per element. */
    void table(const Nodes& nodes, const ValueNode&)
    {
        const std::vector<int> children = children_of(nodes, found_);
        std::vector<std::string> columns;
        if (!children.empty())
            each_element(nodes, children[0], message(), [&columns](const ValueNode& e, int) {
                columns.push_back(e.name);
                return true;
            });
        std::string key = "table:" + std::to_string(children.size()), rml = "<div class=\"scroll-table\"><table><tr>";
        for (const std::string& col : columns) {
            key += ":" + col;
            rml += "<th>" + Rml::StringUtilities::EncodeRml(col) + "</th>";
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
            each_element(nodes, row, message(), [&](const ValueNode& cell, int at) {
                put(slot++, brief(nodes, cell, at, message()), cell.path);
                return true;
            });
    }

    /* A row per call by any node, newest first. */
    void calls(const Nodes&, const ValueNode&)
    {
        const Capture::WatchView* watch = source->view(topic_);
        if (!watch) return;
        if (watch->calls.empty()) {
            html("calls:" + watch->calls_status,
                 "<span class=\"faint\">" + Rml::StringUtilities::EncodeRml(watch->calls_status) + "</span>");
            return;
        }
        const bool task = watch->kind == Capture::Kind::Task;
        /* who called with when under it, a message column per part, and the state with how
           long it took under it. Only a message column wraps. */
        struct Column {
            const char* label;
            const char* sized;
            int         slots;
        };
        static const Column FUNCTION_COLUMNS[] = { { "Caller", "caller", 2 }, { "Request", "message", 1 },
                                                   { "State", "state", 2 }, { "Reply", "message", 1 } };
        static const Column TASK_COLUMNS[] = { { "Caller", "caller", 2 }, { "Goal", "message", 1 },
                                               { "State", "state", 2 }, { "Progress", "message", 1 },
                                               { "Result", "message", 1 } };
        const Column* columns = task ? TASK_COLUMNS : FUNCTION_COLUMNS;
        const size_t column_count = task ? 5 : 4;
        const std::string key = std::string("calls:") + (task ? "task:" : "function:") + std::to_string(watch->calls.size());
        if (key != html_key_) {
            /* RmlUi takes a column's weight from a col element only, never from its heading */
            std::string rml = "<div class=\"scroll-table calls\"><table>";
            for (size_t col = 0; col < column_count; col++) rml += std::string("<col class=\"") + columns[col].sized + "\"/>";
            rml += "<tr>";
            for (size_t col = 0; col < column_count; col++) rml += std::string("<th>") + columns[col].label + "</th>";
            rml += "</tr>";
            for (size_t r = 0; r < watch->calls.size(); r++) {
                rml += "<tr>";
                for (size_t col = 0; col < column_count; col++)
                    rml += std::string("<td class=\"mono ") + columns[col].sized + "\"><div class=\"slot\"></div>" +
                           (columns[col].slots == 2 ? "<div class=\"slot under\"></div>" : "") + "</td>";
                rml += "</tr>";
            }
            html(key, rml + "</table></div>");
            calls_seq_ = ~0ull;
        }
        if (calls_seq_ == watch->calls_seq) return;
        calls_seq_ = watch->calls_seq;
        size_t slot = 0;
        for (const Capture::CallRow& row : watch->calls) {
            put(slot++, row.caller.empty() ? DASH : row.caller);
            put(slot++, row.time);
            put(slot++, brief_message(row.request));
            if (slot < slots_.size()) {
                slots_[slot]->SetClass("ok", row.tone == 1);
                slots_[slot]->SetClass("bad", row.tone == 2);
                slots_[slot]->SetClass("busy", row.tone == 3);
            }
            put(slot++, row.state);
            put(slot++, row.took);
            if (task) put(slot++, brief_message(row.progress));
            put(slot++, !row.reply.empty() ? brief_message(row.reply) : !row.message.empty() ? row.message : std::string(DASH));
        }
    }

    /* A small copy of the element's own visual per element, each with its own bounds. */
    void multi(const Nodes& nodes, const ValueNode&)
    {
        const std::vector<int> children = children_of(nodes, found_);
        const std::string count = std::to_string(children.size());
        std::string rml = "<div class=\"multi\">";
        for (size_t i = 0; i < children.size(); i++)
            rml += "<div class=\"multi-cell\"><span class=\"idx mono\">" + std::to_string(i) +
                   "</span><viz topic=\"" + Rml::StringUtilities::EncodeRml(topic_) + "\" path=\"" +
                   Rml::StringUtilities::EncodeRml(nodes[children[i]].path) + "\"></viz></div>";
        html("multi:" + count, rml + "</div>");
    }

    /* ---------------------------------------------------------------- traces */

    const std::deque<Capture::TracePoint>& trace(const std::string& path) const
    {
        static const std::deque<Capture::TracePoint> none;
        const std::deque<Capture::TracePoint>* t = source->trace_of(topic_, path);
        return t ? *t : none;
    }

    /* The trace keys the card's feed fills, in the feed's order. */
    std::vector<std::string> fed(const ValueNode& node) const
    {
        return fed_paths(row_of(visual_).feed, path_, node.std_name);
    }

    /* The points of the card's two or three traces that arrived together, oldest first.
       Each message adds to all of them, so they line up from the newest end. */
    std::vector<Double3> trail(const ValueNode& node) const
    {
        const std::vector<std::string> paths = fed(node);
        const bool flat = paths.size() < 3;
        const auto& tx = trace(paths[0]);
        const auto& ty = trace(paths[1]);
        const auto& tz = flat ? tx : trace(paths[2]);
        const size_t n = std::min({ tx.size(), ty.size(), tz.size() });
        std::vector<Double3> out(n);
        for (size_t i = 0; i < n; i++) {
            out[i].x = tx[tx.size() - n + i].v;
            out[i].y = ty[ty.size() - n + i].v;
            out[i].z = flat ? 0 : tz[tz.size() - n + i].v;
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

        const float top = c.dp(TOP), bottom = c.h() - c.dp(BOT + LINE);
        const float x0 = c.dp(GUTTER), x1 = c.w() - c.dp(10);
        auto X = [&](double t) { return (float)(x0 + (x1 - x0) * (1 - (now - t) / PLOT_SECONDS)); };
        auto Y = [&](double v) { return (float)(top + (bottom - top) * (1 - (v - lo_) / (hi_ - lo_))); };

        time_grid(c, X, now, top, bottom, on(Show::Grid));
        value_grid(c, x0, x1, lo_, hi_, Y, is_float, on(Show::Grid));
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
    void plot(Canvas& c, const Nodes&, const ValueNode& node)
    {
        if (!on(Show::History)) {
            alone(c, c.all(), value_text(node));
            return;
        }
        lines(c, { path_ }, { ink.accent }, node.number, node.is_float);
        foot(c, over_time(c), { { value_text(node), ink.accent } });
    }

    /* A Float4, Double4 or Int4: a line per component in its own colour, named in the foot,
       or without history the four values side by side. */
    void vec4(Canvas& c, const Nodes& nodes, const ValueNode& node)
    {
        const char* names[4] = { "x", "y", "z", "w" };
        const std::vector<std::string> paths = fed(node);
        std::vector<std::string> texts;
        std::vector<Rml::Colourb> colours;
        bool is_float = false;
        for (int i = 0; i < 4; i++) {
            const int k = kid(nodes, found_, names[i]);
            texts.push_back(k >= 0 ? value_text(nodes[k]) : "");
            colours.push_back(component_colour(i));
            is_float = is_float || (k >= 0 && nodes[k].is_float);
        }
        if (!on(Show::History)) {
            side_by_side(c, c.all(), { "x", "y", "z", "w" }, texts, colours);
            return;
        }
        const int x = kid(nodes, found_, "x");
        lines(c, paths, colours, x >= 0 ? nodes[x].number : 0, is_float);
        /* each component named in its colour, with its value while all four fit */
        std::vector<Part> legend;
        float wide = 0;
        for (int i = 0; i < 4; i++) {
            legend.push_back({ std::string(names[i]) + " " + texts[i], colours[i] });
            wide += c.text_width(legend.back().s + "  ", LABEL);
        }
        if (wide > c.w() - c.dp(16))
            for (int i = 0; i < 4; i++) legend[i].s = names[i];
        foot(c, over_time(c), legend);
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
    void state(Canvas& c, const Nodes&, const ValueNode& node)
    {
        auto colour = [&](double v) {
            if (node.kind == ValueNode::Bool) return v != 0 ? ink.green : ink.border;
            return palette((long long)v);
        };
        if (!on(Show::History)) {
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
        const float top = c.dp(TOP) + c.dp(4), bottom = c.h() - c.dp(BOT + LINE);
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
        time_grid(c, X, now, c.dp(TOP) + c.dp(2), bottom, on(Show::Grid));
        foot(c, over_time(c), value_text(node));
    }

    /* ---------------------------------------------------------------- arrays of numbers */

    /* Bars from zero, the range grown from every value seen, labelled while few. */
    void bars(Canvas& c, const Nodes& nodes, const ValueNode&)
    {
        const std::vector<double> values = numbers_of(nodes, found_, message());
        const size_t n = values.size();
        if (!seen_) { lo_seen_ = hi_seen_ = 0; seen_ = true; }
        for (const double v : values) {
            lo_seen_ = std::min(lo_seen_, v);
            hi_seen_ = std::max(hi_seen_, v);
        }
        ease_range(lo_seen_, hi_seen_);
        const double lo = lo_, hi = hi_ - lo_ > 1e-12 ? hi_ : lo_ + 1;
        const float top = c.dp(TOP), bottom = c.h() - c.dp(BOT), x0 = c.dp(GUTTER), x1 = c.w() - c.dp(10);
        auto Y = [&](double v) { return (float)(top + (bottom - top) * (1 - (v - lo) / (hi - lo))); };
        bool is_float = false;
        each_element(nodes, found_, message(), [&is_float](const ValueNode& first, int) {
            is_float = first.is_float;
            return false;
        });
        value_grid(c, x0, x1, lo, hi, Y, is_float, on(Show::Grid));
        const float gap = n <= LABELLED ? 0.3f : 0, width = n ? (x1 - x0) / n : 0;
        const float base = Y(std::max(lo_seen_, std::min(0.0, hi_seen_)));
        /* Bars sharing a pixel column draw as one, spanning their extremes and the base. */
        for (size_t i = 0; i < n;) {
            /* whole pixel edges, so dense neighbours meet without a seam */
            const float x = std::floor(x0 + i * width + width * gap / 2);
            float right = std::floor(x0 + i * width + width * (1 - gap / 2));
            float y_top = std::min(base, Y(values[i])), y_bot = std::max(base, Y(values[i]));
            const size_t first = i;
            for (i++; i < n && std::floor(x0 + i * width + width * gap / 2) == x; i++) {
                const float y = Y(values[i]);
                y_top = std::min(y_top, y);
                y_bot = std::max(y_bot, y);
                right = std::floor(x0 + i * width + width * (1 - gap / 2));
            }
            c.rect(x, y_top, std::max(1.f, right - x), std::max(1.f, y_bot - y_top), ink.accent);
            if (n <= LABELLED && on(Show::Labels)) c.text(std::to_string(first), x + width * (1 - gap) / 2, c.h() - c.dp(6), Canvas::Center, ink.faint, LABEL);
        }
        foot(c, c.all(), "", format_number((double)n) + " values");
    }

    /* ---------------------------------------------------------------- planes */

    void vec2(Canvas& c, const Nodes& nodes, const ValueNode& node)
    {
        const Double2 v = vec_2d(nodes, found_);
        const double r = reach_[0].grow({ v.x, v.y }, dt_);
        const Plane p = plane(c, c.all(), r, backdrop());
        if (on(Show::History)) {
            Dots dots(c, ink.trail, 2);
            for (const Double3& t : trail(node)) dots.add(p(t.x, t.y));
        }
        ray(c, p(0, 0), p(v.x, v.y), ink.accent);
        foot(c, c.all(), "x " + f3(v.x) + "  y " + f3(v.y));
    }

    void vec2s(Canvas& c, const Nodes& nodes, const ValueNode&)
    {
        const std::vector<int> items = children_of(nodes, found_);
        std::vector<Double2> v;
        for (const int i : items) v.push_back(vec_2d(nodes, i));
        for (const Double2& e : v) reach_[0].seen = std::max({ reach_[0].seen, std::fabs(e.x), std::fabs(e.y) });
        const Plane p = plane(c, c.all(), reach_[0].grow({}, dt_), backdrop());
        for (size_t i = 1; on(Show::Line) && i < v.size(); i++) c.line(p(v[i - 1]), p(v[i]), ink.dim, THIN);
        for (size_t i = 0; i < v.size(); i++) {
            ray(c, p(0, 0), p(v[i].x, v[i].y), tint(i));
            label(c, p(v[i].x, v[i].y), i);
        }
        foot(c, c.all(), format_number((double)v.size()) + " vectors");
    }

    /* A Pose2D or a Transform2D: its heading at its position joined to the origin, and the
       position's trail. */
    void pose_2d(Canvas& c, const Nodes& nodes, const ValueNode& node)
    {
        const bool framed = node.std_name == "Transform2D";
        const Pose2D at = placed_2d(nodes, framed ? kid(nodes, found_, "pose") : found_);
        /* the margin keeps the heading inside */
        const double r = reach_[0].grow({ at.position.x, at.position.y }, dt_, 1.3);
        const Plane p = plane(c, c.all(), r, backdrop());
        if (on(Show::History)) {
            Dots dots(c, ink.trail, 2);
            for (const Double3& t : trail(node)) dots.add(p(t.x, t.y));
        }
        if (on(Show::Line)) c.line(p(0, 0), p(at.position), ink.faint, THIN);
        if (on(Show::Orientation)) heading(c, p, at, 0.2 * r, ink.accent);
        else                       c.dot(p(at.position), ink.accent, 4);
        const int parent = framed ? kid(nodes, found_, "parent") : -2;
        foot(c, c.all(), { { "x " + f3(at.position.x) + "  y " + f3(at.position.y), ink.dim }, { "angle " + degrees(at.angle), ink.dim } },
             parent >= 0 && !nodes[parent].text.empty() ? "in " + nodes[parent].text : std::string());
    }

    /* A chain of Pose2Ds or Transform2Ds: consecutive ones joined, a heading at each. */
    void poses_2d(Canvas& c, const Nodes& nodes, const ValueNode& node)
    {
        const bool framed = node.elem_std == "Transform2D";
        std::vector<Pose2D> list;
        for (const int i : children_of(nodes, found_)) {
            list.push_back(placed_2d(nodes, framed ? kid(nodes, i, "pose") : i));
            reach_[0].seen = std::max({ reach_[0].seen, std::fabs(list.back().position.x), std::fabs(list.back().position.y) });
        }
        const double r = reach_[0].grow({}, dt_, 1.2);
        const Plane p = plane(c, c.all(), r, backdrop());
        for (size_t i = 1; on(Show::Line) && i < list.size(); i++) c.line(p(list[i - 1].position), p(list[i].position), ink.dim, THIN);
        for (size_t i = 0; i < list.size(); i++) {
            if (on(Show::Orientation)) heading(c, p, list[i], 0.15 * r, tint(i));
            else                       c.dot(p(list[i].position), tint(i), 4);
            label(c, p(list[i].position), i);
        }
        foot(c, c.all(), counted(list.size(), node.elem_std));
    }

    /* One standard 2D shape on a plane that grows to hold it. */
    void figure_one(Canvas& c, const Nodes& nodes, const ValueNode& node)
    {
        const Figure one = figure(nodes, found_, node.std_name);
        hold(reach_[0], one);
        const Plane p = plane(c, c.all(), reach_[0].grow({}, dt_), backdrop());
        draw_figure(c, p, one, ink.accent);
        const auto text = describe(one);
        const auto* box = std::get_if<types::OrientedBox2D>(&one);
        foot(c, c.all(), { { text.first, ink.dim }, { box ? "angle " + degrees(box->pose.angle) : "", ink.dim } }, text.second);
    }

    /* An array of one kind of 2D shape on one plane, each in its own colour. */
    void figures(Canvas& c, const Nodes& nodes, const ValueNode& node)
    {
        std::vector<Figure> list;
        for (const int i : children_of(nodes, found_)) {
            list.push_back(figure(nodes, i, node.elem_std));
            hold(reach_[0], list.back());
        }
        const Plane p = plane(c, c.all(), reach_[0].grow({}, dt_), backdrop());
        for (size_t i = 0; i < list.size(); i++) {
            draw_figure(c, p, list[i], tint(i));
            label(c, p(middle(list[i])), i);
        }
        foot(c, c.all(), counted(list.size(), node.elem_std));
    }

    /* ---------------------------------------------------------------- scenes */

    /* An arrow from the origin in a scene, its foot, drop line and trail. */
    void vec3(Canvas& c, const Nodes& nodes, const ValueNode& node)
    {
        const Double3 v = vec(nodes, found_);
        const double r = reach_[0].grow({ math::double3_length(v) }, dt_);
        const Scene s = scene(c, c.all(), r, camera(), backdrop());
        const std::vector<Double3> t = on(Show::History) ? trail(node) : std::vector<Double3>();
        layered(c, s, v, &t, [&] { ray(c, s(0, 0, 0), s(v), ink.accent); });
        foot(c, c.all(), "x " + f3(v.x) + "  y " + f3(v.y) + "  z " + f3(v.z));
    }

    void quaternion(Canvas& c, const Nodes& nodes, const ValueNode&)
    {
        const Quaternion q = quat(nodes, found_);
        const Scene s = scene(c, c.all(), 1, camera(), backdrop());
        axes(c, s);
        triad(c, s, Double3{}, q, 0.9);
        foot(c, c.all(), euler(q));
    }

    /* A Pose or a Transform: its triad at its position joined to the origin, and the
       position's trail. */
    void pose(Canvas& c, const Nodes& nodes, const ValueNode& node)
    {
        const bool framed = node.std_name == "Transform";
        const Pose p = placed(nodes, framed ? kid(nodes, found_, "pose") : found_);
        /* the margin keeps the triad inside */
        const double r = reach_[0].grow({ math::double3_length(p.position) }, dt_, 1.3);
        const Scene s = scene(c, c.all(), r, camera(), backdrop());
        const std::vector<Double3> t = on(Show::History) ? trail(node) : std::vector<Double3>();
        layered(c, s, p.position, &t, [&] {
            if (on(Show::Line)) c.line(s(0, 0, 0), s(p.position), ink.faint, THIN);
            if (on(Show::Orientation)) triad(c, s, p.position, p.orientation, 0.2 * r);
            else                       c.dot(s(p.position), ink.accent, 4);
        });
        const int parent = framed ? kid(nodes, found_, "parent") : -2;
        foot(c, c.all(), { { "x " + f3(p.position.x) + "  y " + f3(p.position.y) + "  z " + f3(p.position.z), ink.dim }, { euler(p.orientation), ink.dim } },
             parent >= 0 && !nodes[parent].text.empty() ? "in " + nodes[parent].text : std::string());
    }

    /* A twist in the moving body's frame: velocity arrow, turn curl, the path while it
       holds, and the turn axis. The path stops short of a second full turn. */
    void twist(Canvas& c, const Nodes& nodes, const ValueNode&)
    {
        const int li = kid(nodes, found_, "linear"), ai = kid(nodes, found_, "angular");
        const types::Twist body{ li >= 0 ? vec(nodes, li) : Double3{}, ai >= 0 ? vec(nodes, ai) : Double3{} };
        const Double3 &v = body.linear, &w = body.angular;
        const double speed = math::double3_length(v), spin = math::double3_length(w);
        const bool turning = spin > 1e-9;
        const double ahead = turning ? std::min(TWIST_AHEAD, 6.28318530717959 / spin) : TWIST_AHEAD;
        /* the screw the body runs along while the twist holds */
        std::vector<Double3> path{ Double3{} };
        Pose at{ Double3{}, NO_TURN };
        const int steps = 64;
        for (int i = 0; i < steps; i++) {
            at = math::pose_integrate(at, body, ahead / steps);
            path.push_back(at.position);
        }
        /* The view holds the path, and the turn centre while it is near enough to read as a
           turn: a far one would shrink the scene for good, since the reach only grows. */
        const Double3 centre = turning ? math::double3_cross(w, v) * (1 / (spin * spin)) : Double3{};
        double extent = speed;
        for (const Double3& p : path) extent = std::max(extent, math::double3_length(p));
        auto hold = [this](const Double3& p) { reach_[0].seen = std::max(reach_[0].seen, math::double3_length(p)); };
        for (const Double3& p : path) hold(p);
        if (turning && math::double3_length(centre) <= 2 * extent) hold(centre);
        const double r = reach_[0].grow({ speed }, dt_);
        const Scene s = scene(c, c.all(), r, camera(), backdrop());

        if (turning && on(Show::Turn)) {
            const Double3 axis = w * (1 / spin);
            /* a turn centre this far out is a straight run, and its line would leave the view */
            if (math::double3_length(centre) < 20 * r) {
                c.dashed(s(centre - axis * r), s(centre + axis * r), ink.dim, THIN);
                c.dot(s(centre), ink.dim, 4);
            }
        }
        /* its own colour, since a forward run lies along the x axis */
        if (on(Show::Path)) {
            std::vector<Rml::Vector2f> drawn;
            for (const Double3& p : path) drawn.push_back(s(p));
            c.polyline(drawn, ink.purple, STROKE);
            c.dot(drawn.back(), ink.purple, 4);
        }
        /* the curl sweeps most of a turn at the fastest turn seen */
        const double fastest = reach_[1].grow({ spin }, dt_, 1.0);
        if (turning && on(Show::Turn)) curl(c, s, w * (1 / spin), 0.15 * r, 5 * spin / fastest, ink.amber);
        layered(c, s, v, nullptr, [&] { c.arrow(s(0, 0, 0), s(v), ink.accent, ARROW); });
        foot(c, c.all(), "speed " + f3(speed) + " m/s  turn " + f3(spin) + " rad/s", "next " + print("%.1f", ahead) + " s");
    }

    /* A Twist2D in the moving body's frame: the velocity arrow, the turn as an arc about the
       origin, the path while it holds, and the turn centre. The path stops short of a full turn. */
    void twist_2d(Canvas& c, const Nodes& nodes, const ValueNode&)
    {
        const int li = kid(nodes, found_, "linear");
        const types::Twist2D body{ li >= 0 ? vec_2d(nodes, li) : Double2{}, number(nodes, found_, "angular") };
        const Double2& v = body.linear;
        const double w = body.angular, spin = std::fabs(w), speed = math::double2_length(v);
        const bool turning = spin > 1e-9;
        const double ahead = turning ? std::min(TWIST_AHEAD, 6.28318530717959 / spin) : TWIST_AHEAD;
        std::vector<Double2> path{ Double2{} };
        Pose2D at;
        const int steps = 64;
        for (int i = 0; i < steps; i++) {
            at = math::pose2d_integrate(at, body, ahead / steps);
            path.push_back(at.position);
        }
        /* as in 3D, a far turn centre is left out of the reach, which only grows */
        const Double2 centre = turning ? Double2{ -v.y / w, v.x / w } : Double2{};
        double extent = speed;
        auto hold = [this](const Double2& p) { reach_[0].seen = std::max({ reach_[0].seen, std::fabs(p.x), std::fabs(p.y) }); };
        for (const Double2& p : path) {
            extent = std::max(extent, math::double2_length(p));
            hold(p);
        }
        if (turning && math::double2_length(centre) <= 2 * extent) hold(centre);
        const double r = reach_[0].grow({ v.x, v.y }, dt_);
        const Plane p = plane(c, c.all(), r, backdrop());

        if (turning && on(Show::Turn) && math::double2_length(centre) < 20 * r) {
            c.dashed(p(0, 0), p(centre), ink.dim, THIN);
            c.dot(p(centre), ink.dim, 4);
        }
        if (on(Show::Path)) {
            std::vector<Rml::Vector2f> drawn;
            for (const Double2& e : path) drawn.push_back(p(e));
            c.polyline(drawn, ink.purple, STROKE);
            c.dot(drawn.back(), ink.purple, 4);
        }
        const double fastest = reach_[1].grow({ spin }, dt_, 1.0);
        if (turning && on(Show::Turn)) curl_2d(c, p, Double2{}, 0.15 * r, 5 * w / fastest, ink.amber);
        c.arrow(p(0, 0), p(v), ink.accent, ARROW);
        foot(c, c.all(), "speed " + f3(speed) + " m/s  turn " + f3(w) + " rad/s", "next " + print("%.1f", ahead) + " s");
    }

    /* A Wrench2D: the force as an arrow with its trail, the torque as a turn about the
       origin that sweeps most of a turn at the strongest seen. */
    void wrench_2d(Canvas& c, const Nodes& nodes, const ValueNode& node)
    {
        const int fi = kid(nodes, found_, "force");
        const Double2 f = fi >= 0 ? vec_2d(nodes, fi) : Double2{};
        const double torque = number(nodes, found_, "torque");
        const double r = reach_[0].grow({ f.x, f.y }, dt_);
        const Plane p = plane(c, c.all(), r, backdrop());
        const double strongest = reach_[1].grow({ torque }, dt_, 1.0);
        if (std::fabs(torque) > 1e-9 && on(Show::Torque)) curl_2d(c, p, Double2{}, 0.2 * r, 5 * torque / strongest, ink.amber);
        if (on(Show::History)) {
            Dots dots(c, ink.trail, 2);
            for (const Double3& t : trail(node)) dots.add(p(t.x, t.y));
        }
        c.arrow(p(0, 0), p(f), ink.accent, ARROW);
        foot(c, c.all(), "force " + f3(math::double2_length(f)) + " N  torque " + f3(torque) + " N m");
    }

    void vec3s(Canvas& c, const Nodes& nodes, const ValueNode&)
    {
        std::vector<Double3> v;
        for (const int i : children_of(nodes, found_)) v.push_back(vec(nodes, i));
        for (const Double3& e : v) reach_[0].seen = std::max(reach_[0].seen, math::double3_length(e));
        const Scene s = scene(c, c.all(), reach_[0].grow({}, dt_), camera(), backdrop());
        back_to_front(c, s, v, [&](size_t i) {
            if (i && on(Show::Line)) c.line(s(v[i - 1]), s(v[i]), ink.dim, THIN);
            ray(c, s(0, 0, 0), s(v[i]), tint(i));
            label(c, s(v[i]), i);
        });
        foot(c, c.all(), format_number((double)v.size()) + " vectors");
    }

    /* A chain of Poses or Transforms: consecutive ones joined in the air, a triad at each. */
    void poses(Canvas& c, const Nodes& nodes, const ValueNode& node)
    {
        const bool framed = node.elem_std == "Transform";
        std::vector<Pose> p;
        std::vector<Double3> at;
        for (const int i : children_of(nodes, found_)) {
            p.push_back(placed(nodes, framed ? kid(nodes, i, "pose") : i));
            at.push_back(p.back().position);
        }
        for (const Double3& e : at) reach_[0].seen = std::max(reach_[0].seen, math::double3_length(e));
        const double r = reach_[0].grow({}, dt_, 1.2);
        const Scene s = scene(c, c.all(), r, camera(), backdrop());
        back_to_front(c, s, at, [&](size_t i) {
            if (i && on(Show::Line)) c.line(s(at[i - 1]), s(at[i]), ink.dim, THIN);
            if (on(Show::Orientation)) triad(c, s, at[i], p[i].orientation, 0.15 * r);
            else                       c.dot(s(at[i]), ink.accent, 4);
            label(c, s(at[i]), i);
        });
        foot(c, c.all(), format_number((double)p.size()) + (framed ? " transforms" : " poses"));
    }

    /* A force and a torque about the frame origin: the force as an arrow with its trail,
       the torque as a turn about its axis that sweeps most of a turn at the strongest seen. */
    void wrench(Canvas& c, const Nodes& nodes, const ValueNode& node)
    {
        const int fi = kid(nodes, found_, "force"), ti = kid(nodes, found_, "torque");
        const Double3 f = fi >= 0 ? vec(nodes, fi) : Double3{}, t = ti >= 0 ? vec(nodes, ti) : Double3{};
        const double r = reach_[0].grow({ math::double3_length(f) }, dt_);
        const Scene s = scene(c, c.all(), r, camera(), backdrop());
        const double torque = math::double3_length(t), strongest = reach_[1].grow({ torque }, dt_, 1.0);
        if (torque > 1e-9 && on(Show::Torque)) {
            const Double3 axis = t * (1 / torque);
            c.dashed(s(axis * -r), s(axis * r), faded(ink.amber, 0.6f), THIN);
            curl(c, s, axis, 0.2 * r, 5 * torque / strongest, ink.amber);
        }
        const std::vector<Double3> tail = on(Show::History) ? trail(node) : std::vector<Double3>();
        layered(c, s, f, &tail, [&] { c.arrow(s(0, 0, 0), s(f), ink.accent, ARROW); });
        foot(c, c.all(), "force " + f3(math::double3_length(f)) + " N  torque " + f3(torque) + " N m");
    }

    /* One standard shape in a scene that grows to hold it, with its drop line. */
    void solid_one(Canvas& c, const Nodes& nodes, const ValueNode& node)
    {
        const Solid one = solid(nodes, found_, node.std_name);
        hold(reach_[0], one);
        const Scene s = scene(c, c.all(), reach_[0].grow({}, dt_), camera(), backdrop());
        layered(c, s, middle(one), nullptr, [&] { draw_solid(c, s, one, ink.accent); });
        const auto text = describe(one);
        const auto* box = std::get_if<types::OrientedBox>(&one);
        foot(c, c.all(), { { text.first, ink.dim }, { box ? euler(box->pose.orientation) : "", ink.dim } }, text.second);
    }

    /* An array of one kind of shape in one scene, each in its own colour. */
    void solids(Canvas& c, const Nodes& nodes, const ValueNode& node)
    {
        std::vector<Solid> list;
        std::vector<Double3> at;
        for (const int i : children_of(nodes, found_)) {
            list.push_back(solid(nodes, i, node.elem_std));
            at.push_back(middle(list.back()));
            hold(reach_[0], list.back());
        }
        const Scene s = scene(c, c.all(), reach_[0].grow({}, dt_), camera(), backdrop());
        back_to_front(c, s, at, [&](size_t i) {
            draw_solid(c, s, list[i], tint(i));
            label(c, s(at[i]), i);
        });
        foot(c, c.all(), counted(list.size(), node.elem_std));
    }

    /* ---------------------------------------------------------------- maps */

    /* A metric north up plane over every point seen, eased, with a grid and a north mark.
       Draw is handed the projection and the clip rectangle. */
    template <class Draw>
    void map(Canvas& c, const std::vector<Double3>& points, Draw draw)
    {
        /* x is latitude, y longitude */
        for (const Double3& p : points) {
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
            if (x >= left && x <= right && on(Show::Grid)) c.line(x, top, x, bottom, ink.grid);
        }
        for (double m = -std::ceil(c.h() / k / step) * step; m <= c.h() / k; m += step) {
            const float y = (float)(cy + m * k);
            if (y >= top && y <= bottom && on(Show::Grid)) c.line(left, y, right, y, ink.grid);
        }
        draw(P, left, top, right, bottom);
        c.text("N", c.w() - c.dp(16), top + c.dp(12), Canvas::Center, ink.dim, LABEL);
        c.arrow({ c.w() - c.dp(16), top + c.dp(36) }, { c.w() - c.dp(16), top + c.dp(17) }, ink.dim, THIN);
    }

    void geo(Canvas& c, const Nodes& nodes, const ValueNode& node)
    {
        const Double3 now{ number(nodes, found_, "lat"), number(nodes, found_, "lon"), number(nodes, found_, "alt") };
        std::vector<Double3> points = on(Show::History) ? trail(node) : std::vector<Double3>();
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

    void geos(Canvas& c, const Nodes& nodes, const ValueNode&)
    {
        std::vector<Double3> points;
        for (const int i : children_of(nodes, found_)) points.push_back(Double3{ number(nodes, i, "lat"), number(nodes, i, "lon"), 0 });
        map(c, points, [&](auto P, float left, float top, float right, float bottom) {
            for (size_t i = 1; on(Show::Line) && i < points.size(); i++) {
                Rml::Vector2f a = P(points[i - 1].x, points[i - 1].y), b = P(points[i].x, points[i].y);
                if (clip(a.x, a.y, b.x, b.y, left, top, right, bottom)) c.line(a, b, ink.dim, THIN);
            }
            for (size_t i = 0; i < points.size(); i++) {
                const Rml::Vector2f at = P(points[i].x, points[i].y);
                if (!inside(at, left, top, right, bottom)) continue;
                c.dot(at, tint(i), 5);
                label(c, at, i);
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

    void colour(Canvas& c, const Nodes& nodes, const ValueNode&)
    {
        const Rml::Colourb col = rgba(nodes, found_);
        c.rect(c.dp(10), c.dp(TOP), c.w() - c.dp(20), c.h() - c.dp(TOP + BOT), col);
        char hex[16];
        std::snprintf(hex, sizeof hex, "#%02x%02x%02x%02x", col.red, col.green, col.blue, col.alpha);
        foot(c, c.all(), hex, std::to_string(col.red) + " " + std::to_string(col.green) + " " +
                              std::to_string(col.blue) + " " + std::to_string(col.alpha));
    }

    void colours(Canvas& c, const Nodes& nodes, const ValueNode&)
    {
        const std::vector<int> items = children_of(nodes, found_);
        const float width = items.empty() ? 0 : (c.w() - c.dp(20)) / items.size();
        for (size_t i = 0; i < items.size(); i++) {
            c.rect(c.dp(10) + i * width + 1, c.dp(TOP), width - 2, c.h() - c.dp(TOP + BOT + 18), rgba(nodes, items[i]));
            if (items.size() <= LABELLED && on(Show::Labels))
                c.text(std::to_string(i), c.dp(10) + i * width + width / 2, c.h() - c.dp(BOT + 4), Canvas::Center, ink.faint, LABEL);
        }
        foot(c, c.all(), "", format_number((double)items.size()) + " colors");
    }

    /* One row per joint and a column each for position, velocity and effort: a bar from
       the centre, each column scaled to the largest magnitude it has seen, written above. */
    void joints(Canvas& c, const Nodes& nodes, const ValueNode&)
    {
        const char* names[3] = { "position", "velocity", "effort" };
        std::vector<double> columns[3];
        size_t rows = 0;
        for (int f = 0; f < 3; f++) {
            const int k = kid(nodes, found_, names[f]);
            if (k >= 0) columns[f] = numbers_of(nodes, k, message());
            rows = std::max(rows, columns[f].size());
        }
        const float gutter = c.dp(28), gap = c.dp(10), top = c.dp(TOP) + c.dp(16), bottom = c.h() - c.dp(8);
        const float col_w = (c.w() - gutter - c.dp(8) - 2 * gap) / 3;
        const float row_h = rows ? std::min(c.dp(26), (bottom - top) / rows) : 0, bar_h = std::max(1.f, row_h - c.dp(4));
        for (int f = 0; f < 3; f++) {
            double biggest = 0;
            for (const double v : columns[f]) biggest = std::max(biggest, std::fabs(v));
            scale_[f].seen = std::max(scale_[f].seen, biggest);
            const double m = scale_[f].grow({}, dt_, 1.0);
            const float x = gutter + f * (col_w + gap);
            c.text(std::string(names[f]) + "  \xC2\xB1" + short_number(m), x, c.dp(TOP) + c.dp(6), Canvas::Left, ink.faint, LABEL);
            for (size_t r = 0; r < columns[f].size(); r++) {
                const float y = top + r * row_h;
                const double v = columns[f][r];
                c.rect(x, y, col_w, bar_h, ink.panel);
                for (int q = 1; q < 4 && on(Show::Grid); q++) c.line(x + col_w * q / 4, y, x + col_w * q / 4, y + bar_h, ink.grid);
                const float mid = x + col_w / 2, len = (float)(std::max(-1.0, std::min(1.0, v / m)) * col_w / 2);
                c.rect(std::min(mid, mid + len), y, std::fabs(len), bar_h, ink.accent);
                c.line(mid, y, mid, y + bar_h, ink.border);
                if (on(Show::Readings)) c.text(f3(v), x + col_w - c.dp(5), y + bar_h / 2 + c.dp(4), Canvas::Right, ink.text, LABEL);
            }
        }
        for (size_t r = 0; on(Show::Labels) && r < rows; r++)
            c.text(std::to_string(r), gutter - c.dp(6), top + r * row_h + bar_h / 2 + c.dp(4), Canvas::Right, ink.faint, LABEL);
    }

    /* ---------------------------------------------------------------- pictures */

    /* A standard Image, decoded once per message into a texture and fitted inside the
       card, keeping its shape. */
    void picture(Canvas& c, const Nodes& nodes, const ValueNode&)
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
    void video(Canvas& c, const Nodes&, const ValueNode&)
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

    /* ---------------------------------------------------------------- tiles */

    size_t child_count(const Nodes* nodes) const
    {
        return nodes ? children_of(*nodes, found_).size() : 0;
    }

    /* A picture wants its frame's shape and letterboxes when it cannot have it. */
    Tile picture_tile(const Nodes*) const
    {
        return Tile{ frame_size_.x > 0 && frame_size_.y > 0 ? (double)frame_size_.x / frame_size_.y : 4.0 / 3, 4 };
    }

    Tile kv_tile(const Nodes* nodes) const
    {
        return grid_tile(2, child_count(nodes));
    }

    Tile joints_tile(const Nodes* nodes) const
    {
        return grid_tile(4, child_count(nodes));
    }

    Tile matrix_tile(const Nodes* nodes) const
    {
        return grid_tile(child_count(nodes), child_count(nodes));
    }

    Tile table_tile(const Nodes* nodes) const
    {
        const std::vector<int> rows = nodes ? children_of(*nodes, found_) : std::vector<int>();
        return grid_tile(rows.empty() ? 1 : element_count(*nodes, rows[0]), rows.size());
    }

    Tile calls_tile(const Nodes*) const
    {
        const Capture::WatchView* watch = source ? source->view(topic_) : nullptr;
        if (!watch || watch->calls.empty()) return Tile{ 3, 0.5 };
        return grid_tile(watch->kind == Capture::Kind::Task ? 7 : 6, watch->calls.size());
    }

public:
    /* What the card wants from the tiling: the aspect of what it draws, and how much it
       minds another. */
    Tile shape() const
    {
        const Size& size = row_of(visual_).size;
        return size.fit ? (this->*size.fit)(source && found_ >= -1 ? &this->nodes() : nullptr) : size.tile;
    }

    std::vector<VizShow> shows() const
    {
        std::vector<VizShow> out;
        for (size_t part = 0; part < std::size(SHOW_NAMES); part++)
            if (row_of(visual_).parts >> part & 1) {
                out.push_back(SHOW_NAMES[part]);
                out.back().on = on((Show)part);
            }
        return out;
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
    /* The node index, -2 none, -1 one held_ stands for: the struct root, or an element of a
       packed array read from the message. */
    int                        found_ = -2;
    bool                       gathers_ = false; /* the path has [*], so it reads gathered_ */
    Nodes                      gathered_;
    std::string                gathered_key_;    /* the message and path gathered_ was made for */
    std::string                shows_text_;      /* the shows attribute shown_ was read from */
    unsigned                   shown_ = 0;       /* a bit per Show the card draws */
    ValueNode                  held_;

    std::string                html_key_;        /* what the children were built for */
    std::vector<Rml::Element*> slots_;           /* the elements whose text fill writes */
    std::vector<std::string>   texts_;           /* what each slot holds now */
    std::vector<std::string>   paths_;           /* the field each slot shows, for Copy cell */
    std::vector<Rml::Element*> lamps_;
    std::vector<int>           lamp_states_;
    uint64_t                   calls_seq_ = ~0ull;   /* the calls the slots were filled from */

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

/* A row per Visual. */
const VizElement::Row& VizElement::row_of(Visual v)
{
    using V = Visual;
    using K = Kind;
    using S = Show;
    using E = VizElement;
    /* Not constexpr: MSVC miscompiles a constant array of member pointers of a class with two bases. */
    static const Row rows[] = {
        /* visual      kind         tile               draw            parts, hidden, feed */
        { V::None,     K::Html,     { 3, 0.5 } },
        { V::Plot,     K::Canvas,   { 3, 1 },          &E::plot,       bits({ S::History, S::Grid, S::Readings }), 0,
          { PLOT_SECONDS } },
        { V::State,    K::Canvas,   { 3, 1 },          &E::state,      bits({ S::History, S::Grid, S::Readings }), 0,
          { PLOT_SECONDS } },
        { V::Text,     K::Html,     { 3, 0.5 },        &E::text },
        { V::Time,     K::Html,     { 3, 0.5 },        &E::time },
        { V::Duration, K::Html,     { 3, 0.5 },        &E::duration },
        { V::Uuid,     K::Html,     { 3, 0.5 },        &E::uuid },
        { V::Hex,      K::Html,     { 3, 0.5 },        &E::hex },
        { V::Chips,    K::Html,     { 3, 0.5 },        &E::chips },
        { V::Bars,     K::Canvas,   { 3, 1 },          &E::bars,       bits({ S::Labels, S::Grid, S::Readings }) },
        { V::Cells,    K::Html,     { 3, 0.5 },        &E::cells },
        { V::Table,    K::Html,     &E::table_tile,    &E::table },
        { V::Matrix,   K::Html,     &E::matrix_tile,   &E::matrix },
        { V::Multi,    K::Html,     { 1.5, 1 },        &E::multi },
        { V::Calls,    K::Html,     &E::calls_tile,    &E::calls },
        { V::Vec2,     K::Canvas,   { 1.3, 1 },        &E::vec2,       bits({ S::History, S::Arrows }) | PLANE_PARTS, 0,
          { TRAIL_SECONDS, nullptr, { "x", "y" } } },
        { V::Vec3,     K::Scene,    { 1, 1.5 },        &E::vec3,       bits({ S::History, S::Arrows }) | SCENE_PARTS, 0,
          { TRAIL_SECONDS, nullptr, { "x", "y", "z" } } },
        { V::Vec4,     K::Canvas,   { 3, 1 },          &E::vec4,       bits({ S::History, S::Grid, S::Readings }), 0,
          { PLOT_SECONDS, nullptr, { "x", "y", "z", "w" } } },
        { V::Quat,     K::Scene,    { 1, 1.5 },        &E::quaternion, PLANE_PARTS },
        { V::Pose,     K::Scene,    { 1, 1.5 },        &E::pose,       bits({ S::History, S::Orientation, S::Line }) | SCENE_PARTS, 0,
          { TRAIL_SECONDS, "position", { "x", "y", "z" } } },
        { V::Pose2D,   K::Canvas,   { 1.3, 1 },        &E::pose_2d,    bits({ S::History, S::Orientation, S::Line }) | PLANE_PARTS, 0,
          { TRAIL_SECONDS, "position", { "x", "y" } } },
        { V::Twist,    K::Scene,    { 1, 1.5 },        &E::twist,      bits({ S::Path, S::Turn }) | SCENE_PARTS },
        { V::Twist2D,  K::Canvas,   { 1.3, 1 },        &E::twist_2d,   bits({ S::Path, S::Turn }) | PLANE_PARTS },
        { V::Wrench,   K::Scene,    { 1, 1.5 },        &E::wrench,     bits({ S::History, S::Torque }) | SCENE_PARTS, 0,
          { TRAIL_SECONDS, "force", { "x", "y", "z" } } },
        { V::Wrench2D, K::Canvas,   { 1.3, 1 },        &E::wrench_2d,  bits({ S::History, S::Torque }) | PLANE_PARTS, 0,
          { TRAIL_SECONDS, "force", { "x", "y" } } },
        { V::Solid,    K::Scene,    { 1, 1.5 },        &E::solid_one,  SCENE_PARTS },
        { V::Figure,   K::Canvas,   { 1.3, 1 },        &E::figure_one, PLANE_PARTS },
        { V::Geo,      K::Canvas,   { 1.5, 1 },        &E::geo,        bits({ S::History, S::Grid, S::Readings }), 0,
          { TRAIL_SECONDS, nullptr, { "lat", "lon" } } },
        { V::Color,    K::Canvas,   { 1.5, 0.5 },      &E::colour,     bits({ S::Readings }) },
        { V::Joints,   K::Canvas,   &E::joints_tile,   &E::joints,     bits({ S::Labels, S::Grid, S::Readings }) },
        { V::Image,    K::Picture,  &E::picture_tile,  &E::picture },
        { V::Video,    K::Picture,  &E::picture_tile,  &E::video,      bits({ S::Readings }), 0,
          { 0, nullptr, {}, true } },
        { V::Stream,   K::Html,     { 2.5, 0.5 },      &E::stream },
        { V::Vec2s,    K::Canvas,   { 1.3, 1 },        &E::vec2s,      bits({ S::Arrows, S::Line, S::Labels, S::Colours }) | PLANE_PARTS, bits({ S::Line }) },
        { V::Vec3s,    K::Scene,    { 1, 1.5 },        &E::vec3s,      bits({ S::Arrows, S::Line, S::Labels, S::Colours }) | SCENE_PARTS, bits({ S::Line }) },
        { V::Poses,    K::Scene,    { 1, 1.5 },        &E::poses,      bits({ S::Orientation, S::Line, S::Labels }) | SCENE_PARTS },
        { V::Poses2D,  K::Canvas,   { 1.3, 1 },        &E::poses_2d,   bits({ S::Orientation, S::Line, S::Labels, S::Colours }) | PLANE_PARTS },
        { V::Solids,   K::Scene,    { 1, 1.5 },        &E::solids,     bits({ S::Labels, S::Colours }) | SCENE_PARTS },
        { V::Figures,  K::Canvas,   { 1.3, 1 },        &E::figures,    bits({ S::Labels, S::Colours }) | PLANE_PARTS },
        { V::Geos,     K::Canvas,   { 1.5, 1 },        &E::geos,       bits({ S::Line, S::Labels, S::Colours, S::Grid, S::Readings }), bits({ S::Line }) },
        { V::Colors,   K::Canvas,   { 3, 0.5 },        &E::colours,    bits({ S::Labels, S::Readings }) },
        { V::Kv,       K::Html,     &E::kv_tile,       &E::kv },
    };
    static_assert(sizeof rows / sizeof rows[0] == (size_t)Visual::Kv + 1, "a row per Visual");
    for (const Row& row : rows)
        if (row.visual == v) return row;
    return rows[0];
}

Rml::ElementInstancerGeneric<VizElement> instancer;

} /* namespace */

void viz_init(Capture& capture)
{
    source = &capture;
    Rml::Factory::RegisterElementInstancer("viz", &instancer);
}

VizFeeds viz_feeds(const Capture::WatchView& watch, const std::set<std::string>& shown,
                   const std::map<std::string, ShowChoices>& shows)
{
    static const ShowChoices none;
    VizFeeds out;
    for (const std::string& path : shown) {
        const int index = find_node(watch.nodes, watch, path);
        const VizElement::Row& row = VizElement::row_of(visual_at(watch, watch.nodes, index));
        if (row.feed.stream) out.streams.push_back(path);
        const auto chosen = shows.find(path);
        if (row.feed.seconds == 0 || !(row.shown(chosen != shows.end() ? chosen->second : none) >> (int)Show::History & 1))
            continue;
        const std::string std_name = index == -1 ? watch.root_std : index >= 0 ? watch.nodes[index].std_name : "";
        for (const std::string& traced : fed_paths(row.feed, path, std_name))
            out.traces.push_back(Capture::TraceSpec{ traced, row.feed.seconds });
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

std::vector<VizShow> viz_shows(Rml::Element* element)
{
    const auto* viz = dynamic_cast<const VizElement*>(element);
    return viz ? viz->shows() : std::vector<VizShow>();
}

std::string viz_shows_text(const ShowChoices& choices)
{
    std::string out;
    for (const auto& choice : choices) out += (out.empty() ? "" : " ") + choice.first + (choice.second ? ":1" : ":0");
    return out;
}
