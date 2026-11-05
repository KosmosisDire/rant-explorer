#include "gpu.hpp"

#include <RmlUi/Core/Core.h>
#include <RmlUi_Renderer_GL3.h>
#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
#define SHADER_VERSION "#version 300 es\nprecision highp float;\n"
#else
#include <RmlUi_Include_GL3.h>   /* the GL entry points the renderer loaded */
#define SHADER_VERSION "#version 330\n"
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <vector>

namespace {

/* A stroke is a quad laid out in window pixels around its line, wide enough for its width
   plus a pixel to smooth the edge. Its ends take its points' depths. The lines it joins are
   laid in its own frame: along it from a, then across. */
const char* const STROKE_VERTEX = R"(
uniform mat4 to_clip;
uniform vec2 window;
uniform float dp;
layout(location = 0) in vec3 from;
layout(location = 1) in vec3 to;
layout(location = 2) in vec4 colour;
layout(location = 3) in vec2 shape;
layout(location = 4) in vec3 before;
layout(location = 5) in vec3 after;
out vec4 tint;
out vec2 at;
flat out vec3 form;
flat out vec4 sides;
flat out vec2 joins;
const vec2 corners[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
                                vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));
void main()
{
    vec4 a = to_clip * vec4(from, 1.0), b = to_clip * vec4(to, 1.0);
    vec2 pa = (a.xy * 0.5 + 0.5) * window, pb = (b.xy * 0.5 + 0.5) * window;
    float len = length(pb - pa);
    vec2 along = len > 0.001 ? (pb - pa) / len : vec2(1.0, 0.0), across = vec2(-along.y, along.x);
    float mark = shape.y, half_width = mark > 1.5 ? shape.x : shape.x * 0.5;
    float reach = half_width + 1.0;
    vec2 pc = ((to_clip * vec4(before, 1.0)).xy * 0.5 + 0.5) * window - pa;
    vec2 pd = ((to_clip * vec4(after, 1.0)).xy * 0.5 + 0.5) * window - pa;
    sides = vec4(dot(pc, along), dot(pc, across), dot(pd, along), dot(pd, across));
    joins = vec2(any(notEqual(before, from)) ? 1.0 : 0.0, any(notEqual(after, to)) ? 1.0 : 0.0);
    vec2 corner = corners[gl_VertexID];
    float t = mix(-reach, len + reach, corner.x), s = mix(-reach, reach, corner.y);
    float depth = mix(a.z, b.z, len > 0.001 ? clamp(t / len, 0.0, 1.0) : 0.0);
    gl_Position = vec4((pa + along * t + across * s) / window * 2.0 - 1.0, depth, 1.0);
    tint = vec4(colour.rgb * colour.a, colour.a);
    at = vec2(t, s);
    form = vec3(len, half_width, mark);
}
)";

/* How much of each pixel the stroke covers, from its distance to the shape in pixels. A dash
   is 3 dp on and 3 off. A pixel of a polyline clearly nearer the line before or after is
   left to that line, so each is drawn once, and depth settles a tie. Part 1 draws only the
   pixels a stroke covers whole, part 2 only its soft edge, and part 0 both. */
const char* const STROKE_FRAGMENT = R"(
uniform float dp;
uniform float part;
in vec4 tint;
in vec2 at;
flat in vec3 form;
flat in vec4 sides;
flat in vec2 joins;
out vec4 frag;
float apart(vec2 p, vec2 u, vec2 v)
{
    vec2 e = v - u;
    float k = dot(e, e) > 0.0 ? clamp(dot(p - u, e) / dot(e, e), 0.0, 1.0) : 0.0;
    return length(p - u - e * k);
}
void main()
{
    float len = form.x, half_width = form.y, mark = form.z;
    float cover;
    if (mark > 1.5) {
        cover = half_width + 0.5 - length(at);
    } else {
        float own = length(vec2(at.x - clamp(at.x, 0.0, len), at.y));
        if (joins.x > 0.5 && apart(at, sides.xy, vec2(0.0)) < own - 0.05) discard;
        if (joins.y > 0.5 && apart(at, vec2(len, 0.0), sides.zw) < own - 0.05) discard;
        cover = half_width + 0.5 - own;
        if (mark > 0.5) {
            float m = mod(at.x, 6.0 * dp);
            cover = min(cover, min(m, 3.0 * dp - m) + 0.5);
        }
    }
    if (cover <= 0.0 || (part > 0.5 && part < 1.5 && cover < 1.0) || (part > 1.5 && cover >= 1.0)) discard;
    frag = tint * min(cover, 1.0);
}
)";

const char* const FACE_VERTEX = R"(
uniform mat4 to_clip;
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec4 colour;
out vec3 facing;
out vec4 tint;
void main()
{
    gl_Position = to_clip * vec4(position, 1.0);
    facing = normal;
    tint = colour;
}
)";

/* Lit from one side so a solid reads as round or square. A face seen from behind is lit as
   its front would be, so an open shape looks the same from either side. */
const char* const FACE_FRAGMENT = R"(
uniform vec3 light;
in vec3 facing;
in vec4 tint;
out vec4 frag;
void main()
{
    vec3 n = normalize(facing);
    if (!gl_FrontFacing) n = -n;
    float shade = 0.45 + 0.55 * max(dot(n, light), 0.0);
    frag = vec4(tint.rgb * shade * tint.a, tint.a);
}
)";

struct Gpu {
    bool   tried = false, ready = false;
    GLuint strokes = 0, faces = 0;
    GLint  stroke_to_clip = -1, stroke_window = -1, stroke_dp = -1, stroke_part = -1;
    GLint  face_to_clip = -1, face_light = -1;
    GLuint stroke_array = 0, stroke_buffer = 0, face_array = 0, face_buffer = 0, face_indices = 0;
};

GLuint compile(GLenum kind, const char* body)
{
    const char* parts[2] = { SHADER_VERSION, body };
    const GLuint shader = glCreateShader(kind);
    glShaderSource(shader, 2, parts, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {};
        glGetShaderInfoLog(shader, sizeof log, nullptr, log);
        std::fprintf(stderr, "3D shader: %s\n", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint link(const char* vertex, const char* fragment)
{
    const GLuint vs = compile(GL_VERTEX_SHADER, vertex), fs = compile(GL_FRAGMENT_SHADER, fragment);
    GLuint program = 0;
    if (vs && fs) {
        program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glLinkProgram(program);
        GLint ok = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[1024] = {};
            glGetProgramInfoLog(program, sizeof log, nullptr, log);
            std::fprintf(stderr, "3D program: %s\n", log);
            glDeleteProgram(program);
            program = 0;
        }
    }
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    return program;
}

void attribute(GLuint index, GLint size, GLenum type, bool normalized, GLsizei stride, size_t offset)
{
    glEnableVertexAttribArray(index);
    glVertexAttribPointer(index, size, type, normalized ? GL_TRUE : GL_FALSE, stride, (const void*)offset);
}

/* Points the stroke array at the bound buffer from stroke first on, since GL 3.3 and WebGL 2
   cannot start an instanced draw at an instance. */
void point_strokes(size_t first)
{
    using Stroke = Batch::Stroke;
    const size_t at = first * sizeof(Stroke);
    attribute(0, 3, GL_FLOAT, false, sizeof(Stroke), at + offsetof(Stroke, a));
    attribute(1, 3, GL_FLOAT, false, sizeof(Stroke), at + offsetof(Stroke, b));
    attribute(2, 4, GL_UNSIGNED_BYTE, true, sizeof(Stroke), at + offsetof(Stroke, colour));
    attribute(3, 2, GL_FLOAT, false, sizeof(Stroke), at + offsetof(Stroke, size));
    attribute(4, 3, GL_FLOAT, false, sizeof(Stroke), at + offsetof(Stroke, before));
    attribute(5, 3, GL_FLOAT, false, sizeof(Stroke), at + offsetof(Stroke, after));
}

/* The programs and buffers, made on the first draw and kept for the process. */
Gpu& gpu()
{
    static Gpu g;
    if (g.tried) return g;
    g.tried   = true;
    g.strokes = link(STROKE_VERTEX, STROKE_FRAGMENT);
    g.faces   = link(FACE_VERTEX, FACE_FRAGMENT);
    if (!g.strokes || !g.faces) return g;
    g.stroke_to_clip = glGetUniformLocation(g.strokes, "to_clip");
    g.stroke_window  = glGetUniformLocation(g.strokes, "window");
    g.stroke_dp      = glGetUniformLocation(g.strokes, "dp");
    g.stroke_part    = glGetUniformLocation(g.strokes, "part");
    g.face_to_clip   = glGetUniformLocation(g.faces, "to_clip");
    g.face_light     = glGetUniformLocation(g.faces, "light");

    glGenVertexArrays(1, &g.stroke_array);
    glGenBuffers(1, &g.stroke_buffer);
    glBindVertexArray(g.stroke_array);
    glBindBuffer(GL_ARRAY_BUFFER, g.stroke_buffer);
    point_strokes(0);
    for (GLuint i = 0; i < 6; i++) glVertexAttribDivisor(i, 1);

    using Corner = Batch::Corner;
    glGenVertexArrays(1, &g.face_array);
    glGenBuffers(1, &g.face_buffer);
    glGenBuffers(1, &g.face_indices);
    glBindVertexArray(g.face_array);
    glBindBuffer(GL_ARRAY_BUFFER, g.face_buffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.face_indices);
    attribute(0, 3, GL_FLOAT, false, sizeof(Corner), offsetof(Corner, at));
    attribute(1, 3, GL_FLOAT, false, sizeof(Corner), offsetof(Corner, normal));
    attribute(2, 4, GL_UNSIGNED_BYTE, true, sizeof(Corner), offsetof(Corner, colour));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    g.ready = true;
    return g;
}

} /* namespace */

float Batch::extent() const
{
    float far = 0;
    auto take = [&far](const Rml::Vector3f& p) { far = std::max(far, std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z)); };
    for (const Stroke& s : strokes) {
        take(s.a);
        take(s.b);
    }
    for (const Corner& c : corners) take(c.at);
    return far;
}

void draw_batch(const Batch& batch, const Rml::Matrix4f& to_clip, Rml::Vector3f light, float dp, Rml::Vector2i window)
{
    const Gpu& g = gpu();
    if (!g.ready || (batch.strokes.empty() && batch.triangles.empty())) return;

    /* RmlUi draws with premultiplied blending and no depth, so only the depth state changes.
       The clear stays inside the scissor, the card's own box. */
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glClear(GL_DEPTH_BUFFER_BIT);

    /* the opaque triangles first, then the faint ones */
    std::vector<uint32_t> order;
    order.reserve(batch.triangles.size());
    size_t opaque = 0;
    for (int faint = 0; faint < 2; faint++) {
        for (size_t t = 0; t + 2 < batch.triangles.size(); t += 3) {
            const uint32_t* c = &batch.triangles[t];
            const bool whole = batch.corners[c[0]].colour.alpha == 255 && batch.corners[c[1]].colour.alpha == 255 &&
                               batch.corners[c[2]].colour.alpha == 255;
            if (whole != (faint == 1)) order.insert(order.end(), c, c + 3);
        }
        if (!faint) opaque = order.size();
    }
    auto faces = [&](size_t from, size_t to) {
        glDrawElements(GL_TRIANGLES, (GLsizei)(to - from), GL_UNSIGNED_INT, (const void*)(from * sizeof(uint32_t)));
    };
    if (!order.empty()) {
        glUseProgram(g.faces);
        glUniformMatrix4fv(g.face_to_clip, 1, GL_FALSE, to_clip.data());
        glUniform3f(g.face_light, light.x, light.y, light.z);
        glBindVertexArray(g.face_array);
        glBindBuffer(GL_ARRAY_BUFFER, g.face_buffer);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(batch.corners.size() * sizeof(Batch::Corner)), batch.corners.data(), GL_STREAM_DRAW);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(order.size() * sizeof(uint32_t)), order.data(), GL_STREAM_DRAW);
    }
    /* Faces are set back a little so a line on one stays in front, guides a little more so a
       face on the floor hides the grid under it. */
    glEnable(GL_POLYGON_OFFSET_FILL);
    if (opaque) {
        glPolygonOffset(1, 1);
        faces(0, opaque);
    }

    /* Each stroke's whole pixels hide what lies behind them. The guides then show only where
       nothing is in front, hiding nothing. The soft edges go last, each pixel taking the
       nearest once, so a near edge never cuts a far stroke and two strokes on one line blend
       once. */
    if (!batch.strokes.empty()) {
        glUseProgram(g.strokes);
        glUniformMatrix4fv(g.stroke_to_clip, 1, GL_FALSE, to_clip.data());
        glUniform2f(g.stroke_window, (float)window.x, (float)window.y);
        glUniform1f(g.stroke_dp, dp);
        glBindVertexArray(g.stroke_array);
        glBindBuffer(GL_ARRAY_BUFFER, g.stroke_buffer);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(batch.strokes.size() * sizeof(Batch::Stroke)), batch.strokes.data(), GL_STREAM_DRAW);
        const size_t under = std::min(batch.under, batch.strokes.size()), rest = batch.strokes.size() - under;
        if (rest) {
            point_strokes(under);
            glUniform1f(g.stroke_part, 1);
            glPolygonOffset(0, 0);
            glDrawArraysInstanced(GL_TRIANGLES, 0, 6, (GLsizei)rest);
        }
        if (under) {
            point_strokes(0);
            glUniform1f(g.stroke_part, 0);
            glPolygonOffset(2, 2);
            glDepthMask(GL_FALSE);
            glDrawArraysInstanced(GL_TRIANGLES, 0, 6, (GLsizei)under);
            glDepthMask(GL_TRUE);
        }
        if (rest) {
            point_strokes(under);
            glUniform1f(g.stroke_part, 2);
            glPolygonOffset(0, 0);
            glDrawArraysInstanced(GL_TRIANGLES, 0, 6, (GLsizei)rest);
        }
    }

    /* Faint faces go last. A pass of depth alone first keeps the nearest face of each pixel, so
       a solid tints what is behind it once and the strokes behind show through it. */
    if (opaque < order.size()) {
        glUseProgram(g.faces);
        glBindVertexArray(g.face_array);
        glPolygonOffset(1, 1);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        faces(opaque, order.size());
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_FALSE);
        glDepthFunc(GL_LEQUAL);
        faces(opaque, order.size());
        glDepthMask(GL_TRUE);
        glDepthFunc(GL_LESS);
    }
    glDisable(GL_POLYGON_OFFSET_FILL);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisable(GL_DEPTH_TEST);
    /* the renderer keeps which program it used last, so it binds its own again */
    static_cast<RenderInterface_GL3*>(Rml::GetRenderInterface())->ResetProgram();
}

void sharp_texels(bool on)
{
    /* a sampler bound to the unit RmlUi draws with overrides its texture's own filters */
    static GLuint sampler = 0;
    if (on && !sampler) {
        glGenSamplers(1, &sampler);
        glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindSampler(0, on ? sampler : 0);
}
