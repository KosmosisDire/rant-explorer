#include "canvas.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Core/FontEngineInterface.h>
#include <RmlUi/Core/Geometry.h>
#include <RmlUi/Core/MeshUtilities.h>
#include <RmlUi/Core/RenderManager.h>

#include <algorithm>
#include <cmath>

Ink ink;

namespace {

const struct {
    const char*  name;
    Rml::Colourb Ink::*member;
} INK_PROPERTIES[] = {
    { "viz-text", &Ink::text },     { "viz-dim", &Ink::dim },       { "viz-faint", &Ink::faint },
    { "viz-accent", &Ink::accent }, { "viz-green", &Ink::green },   { "viz-amber", &Ink::amber },
    { "viz-red", &Ink::red },       { "viz-blue", &Ink::blue },     { "viz-purple", &Ink::purple },
    { "viz-gray", &Ink::gray },     { "viz-panel", &Ink::panel },   { "viz-border", &Ink::border },
    { "viz-grid", &Ink::grid },     { "viz-trail", &Ink::trail },   { "viz-ground", &Ink::ground },
};

Rml::Vertex vertex(Rml::Vector2f p, Rml::ColourbPremultiplied colour)
{
    return Rml::Vertex{ p, colour, Rml::Vector2f(0, 0) };
}

Rml::FontFaceHandle mono_face(int size)
{
    return Rml::GetFontEngineInterface()->GetFontFaceHandle("ui-mono", Rml::Style::FontStyle::Normal,
                                                            Rml::Style::FontWeight::Normal, size);
}

const Rml::String& no_language()
{
    static const Rml::String language;
    return language;
}

} /* namespace */

void ink_init()
{
    for (const auto& p : INK_PROPERTIES)
        Rml::StyleSheetSpecification::RegisterProperty(p.name, "black", true, false).AddParser("color");
}

void read_ink(Rml::Element& element)
{
    for (const auto& p : INK_PROPERTIES) ink.*p.member = element.GetProperty<Rml::Colourb>(p.name);
}

Rml::Colourb faded(Rml::Colourb colour, float share)
{
    colour.alpha = (Rml::byte)std::lround(colour.alpha * std::max(0.f, std::min(share, 1.f)));
    return colour;
}

Canvas::Canvas(Rml::Element& element, Rml::Mesh& mesh) : element_(element), mesh_(mesh)
{
    mesh_.vertices.clear();
    mesh_.indices.clear();
    const Rml::Context* context = element.GetContext();
    ratio_  = context ? context->GetDensityIndependentPixelRatio() : 1.f;
    origin_ = element.GetAbsoluteOffset(Rml::BoxArea::Content);
    size_   = element.GetBox().GetSize(Rml::BoxArea::Content);
}

void Canvas::line(float x0, float y0, float x1, float y1, Rml::Colourb colour, float width_dp)
{
    const float dx = x1 - x0, dy = y1 - y0, len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-4f) return;
    const float half = dp(width_dp) / 2, nx = -dy / len * half, ny = dx / len * half;
    quad({ x0 + nx, y0 + ny }, { x1 + nx, y1 + ny }, { x1 - nx, y1 - ny }, { x0 - nx, y0 - ny }, colour);
}

void Canvas::dashed(Rml::Vector2f a, Rml::Vector2f b, Rml::Colourb colour)
{
    const float dx = b.x - a.x, dy = b.y - a.y, len = std::sqrt(dx * dx + dy * dy), dash = dp(3);
    for (float t = 0; t < len; t += 2 * dash) {
        const float e = std::min(len, t + dash);
        line(a.x + dx * t / len, a.y + dy * t / len, a.x + dx * e / len, a.y + dy * e / len, colour);
    }
}

void Canvas::arrow(Rml::Vector2f a, Rml::Vector2f b, Rml::Colourb colour, float width_dp)
{
    line(a, b, colour, width_dp);
    const float angle = std::atan2(b.y - a.y, b.x - a.x), l = dp(7);
    if (std::hypot(b.x - a.x, b.y - a.y) < 1e-3f) return;
    triangle(b, { b.x - l * std::cos(angle - 0.4f), b.y - l * std::sin(angle - 0.4f) },
             { b.x - l * std::cos(angle + 0.4f), b.y - l * std::sin(angle + 0.4f) }, colour);
}

void Canvas::rect(float x, float y, float w, float h, Rml::Colourb colour)
{
    quad({ x, y }, { x + w, y }, { x + w, y + h }, { x, y + h }, colour);
}

void Canvas::rounded(float x, float y, float w, float h, float r_dp, Rml::Colourb colour)
{
    const float r = std::min({ dp(r_dp), w / 2, h / 2 });
    rect(x + r, y, w - 2 * r, h, colour);
    rect(x, y + r, r, h - 2 * r, colour);
    rect(x + w - r, y + r, r, h - 2 * r, colour);
    /* A quarter disc per corner, fanned from its centre. */
    const Rml::ColourbPremultiplied pm = colour.ToPremultiplied();
    const Rml::Vector2f centres[] = { { x + w - r, y + h - r }, { x + r, y + h - r }, { x + r, y + r }, { x + w - r, y + r } };
    const int steps = 6;
    for (int corner = 0; corner < 4; corner++) {
        const int base = (int)mesh_.vertices.size();
        mesh_.vertices.push_back(vertex(centres[corner], pm));
        for (int i = 0; i <= steps; i++) {
            const float a = 1.5707963f * (corner + (float)i / steps);
            mesh_.vertices.push_back(vertex({ centres[corner].x + r * std::cos(a), centres[corner].y + r * std::sin(a) }, pm));
        }
        for (int i = 0; i < steps; i++) mesh_.indices.insert(mesh_.indices.end(), { base, base + 1 + i, base + 2 + i });
    }
}

void Canvas::frame(float x, float y, float w, float h, Rml::Colourb colour, float width_dp)
{
    line(x, y, x + w, y, colour, width_dp);
    line(x + w, y, x + w, y + h, colour, width_dp);
    line(x + w, y + h, x, y + h, colour, width_dp);
    line(x, y + h, x, y, colour, width_dp);
}

void Canvas::triangle(Rml::Vector2f a, Rml::Vector2f b, Rml::Vector2f c, Rml::Colourb colour)
{
    const Rml::ColourbPremultiplied pm = colour.ToPremultiplied();
    const int base = (int)mesh_.vertices.size();
    for (const Rml::Vector2f& p : { a, b, c }) mesh_.vertices.push_back(vertex(p, pm));
    mesh_.indices.insert(mesh_.indices.end(), { base, base + 1, base + 2 });
}

void Canvas::dot(float x, float y, Rml::Colourb colour, float radius_dp)
{
    const float r = dp(radius_dp);
    const int sides = std::max(6, std::min(16, (int)std::lround(r * 3)));
    const float turn = 6.2831853f / sides;
    const Rml::ColourbPremultiplied pm = colour.ToPremultiplied();
    const int base = (int)mesh_.vertices.size();
    mesh_.vertices.push_back(vertex({ x, y }, pm));
    for (int i = 0; i < sides; i++) {
        mesh_.vertices.push_back(vertex({ x + r * std::cos(turn * i), y + r * std::sin(turn * i) }, pm));
        mesh_.indices.insert(mesh_.indices.end(), { base, base + 1 + i, base + 1 + (i + 1) % sides });
    }
}

void Canvas::text(const std::string& s, float x, float y, Align align, Rml::Colourb colour, float size_dp)
{
    labels_.push_back(Label{ s, x, y, align, colour, (int)std::lround(dp(size_dp)) });
}

float Canvas::text_width(const std::string& s, float size_dp) const
{
    const Rml::FontFaceHandle face = mono_face((int)std::lround(dp(size_dp)));
    if (!face) return 0;
    return (float)Rml::GetFontEngineInterface()->GetStringWidth(face, s, Rml::TextShapingContext{ no_language() });
}

void Canvas::picture(float x, float y, float w, float h, Rml::Texture texture, Rml::Vector2i texels)
{
    const Rml::Vector2f inset(0.5f / std::max(1, texels.x), 0.5f / std::max(1, texels.y));
    pictures_.push_back(Picture{ { x, y }, { w, h }, texture, inset });
}

void Canvas::render()
{
    Rml::RenderManager* rm = element_.GetRenderManager();
    if (!rm) return;
    /* Nothing draws outside the element: a scene's floor grid runs past its edges. */
    const Rml::RenderState saved = rm->GetState();
    const Rml::Rectanglei box = Rml::Rectanglei::FromPositionSize(
        Rml::Vector2i((int)std::floor(origin_.x), (int)std::floor(origin_.y)),
        Rml::Vector2i((int)std::ceil(size_.x), (int)std::ceil(size_.y)));
    rm->SetScissorRegion(box.IntersectIfValid(saved.scissor_region));
    for (const Picture& p : pictures_) {
        Rml::Mesh quad;
        Rml::MeshUtilities::GenerateQuad(quad, p.at, p.size, Rml::ColourbPremultiplied(255, 255, 255, 255),
                                         p.inset, Rml::Vector2f(1, 1) - p.inset);
        rm->MakeGeometry(std::move(quad)).Render(origin_, p.texture);
    }
    if (!mesh_.indices.empty()) {
        Rml::Geometry shapes = rm->MakeGeometry(std::move(mesh_));
        shapes.Render(origin_);
        mesh_ = shapes.Release(Rml::Geometry::ReleaseMode::ClearMesh);
    }

    /* Every label's glyphs join one mesh per font texture, so the text is a draw or two. */
    Rml::FontEngineInterface* fonts = Rml::GetFontEngineInterface();
    const Rml::TextShapingContext shaping{ no_language() };
    std::vector<std::pair<int, Rml::FontFaceHandle>> faces;
    Rml::TexturedMeshList batches, meshes;
    for (const Label& label : labels_) {
        auto found = std::find_if(faces.begin(), faces.end(), [&](const auto& f) { return f.first == label.size; });
        if (found == faces.end()) {
            faces.emplace_back(label.size, mono_face(label.size));
            found = faces.end() - 1;
        }
        const Rml::FontFaceHandle face = found->second;
        if (!face) continue;
        const float width = (float)fonts->GetStringWidth(face, label.s, shaping);
        const float x = label.align == Right ? label.x - width : label.align == Center ? label.x - width / 2 : label.x;
        meshes.clear();
        fonts->GenerateString(*rm, face, 0, label.s, { std::round(x), std::round(label.y) },
                              label.colour.ToPremultiplied(), 1.f, shaping, meshes);
        for (Rml::TexturedMesh& mesh : meshes) {
            auto batch = std::find_if(batches.begin(), batches.end(), [&](const Rml::TexturedMesh& b) { return b.texture == mesh.texture; });
            if (batch == batches.end()) {
                batches.push_back(std::move(mesh));
                continue;
            }
            const int base = (int)batch->mesh.vertices.size();
            batch->mesh.vertices.insert(batch->mesh.vertices.end(), mesh.mesh.vertices.begin(), mesh.mesh.vertices.end());
            for (const int i : mesh.mesh.indices) batch->mesh.indices.push_back(base + i);
        }
    }
    for (Rml::TexturedMesh& batch : batches) rm->MakeGeometry(std::move(batch.mesh)).Render(origin_, batch.texture);
    rm->SetState(saved);
}

void Canvas::quad(Rml::Vector2f a, Rml::Vector2f b, Rml::Vector2f c, Rml::Vector2f d, Rml::Colourb colour)
{
    const Rml::ColourbPremultiplied pm = colour.ToPremultiplied();
    const int base = (int)mesh_.vertices.size();
    for (const Rml::Vector2f& p : { a, b, c, d }) mesh_.vertices.push_back(vertex(p, pm));
    mesh_.indices.insert(mesh_.indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
}
