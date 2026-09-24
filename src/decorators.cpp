#include "decorators.hpp"

#include <RmlUi/Core.h>

#include <algorithm>
#include <cmath>

namespace {

/* Both are built each render from the computed style, so a hover colour needs no new data.
   A zero handle stops a decorator rendering, and there is no data to keep. */
class Drawn : public Rml::Decorator {
public:
    Rml::DecoratorDataHandle GenerateElementData(Rml::Element*, Rml::BoxArea) const override { return 1; }
    void ReleaseElementData(Rml::DecoratorDataHandle) const override {}

protected:
    static Rml::ColourbPremultiplied colour_of(Rml::Element* element)
    {
        const Rml::Style::ComputedValues& computed = element->GetComputedValues();
        return computed.color().ToPremultiplied(computed.opacity());
    }
    static void quad(Rml::Mesh& mesh, Rml::Vector2f a, Rml::Vector2f b, Rml::Vector2f c, Rml::Vector2f d,
                     Rml::ColourbPremultiplied colour)
    {
        const int at = (int)mesh.vertices.size();
        for (const Rml::Vector2f& p : { a, b, c, d }) mesh.vertices.push_back({ p, colour, {} });
        mesh.indices.insert(mesh.indices.end(), { at, at + 1, at + 2, at, at + 2, at + 3 });
    }
    static void render(Rml::Element* element, Rml::Mesh&& mesh)
    {
        if (Rml::RenderManager* rm = element->GetRenderManager())
            rm->MakeGeometry(std::move(mesh)).Render(element->GetAbsoluteOffset(Rml::BoxArea::Border));
    }
};

class DashedRing : public Drawn {
public:
    DashedRing(int dashes, Rml::NumericValue width) : dashes_(dashes), width_(width) {}

    void RenderElement(Rml::Element* element, Rml::DecoratorDataHandle) const override
    {
        const Rml::Vector2f size   = element->GetBox().GetSize(Rml::BoxArea::Border);
        const Rml::Vector2f centre = size * 0.5f;
        const float outer = std::min(size.x, size.y) * 0.5f;
        const float inner = std::max(0.f, outer - element->ResolveLength(width_));
        const Rml::ColourbPremultiplied colour = colour_of(element);
        constexpr int   STEPS = 4;   /* segments per dash */
        constexpr float TAU   = 6.2831853f;

        Rml::Mesh mesh;
        for (int d = 0; d < dashes_; d++)
            for (int s = 0; s < STEPS; s++) {
                const float a0 = (d + 0.5f * s / STEPS) * TAU / dashes_;
                const float a1 = (d + 0.5f * (s + 1) / STEPS) * TAU / dashes_;
                const Rml::Vector2f u0(std::cos(a0), std::sin(a0)), u1(std::cos(a1), std::sin(a1));
                quad(mesh, centre + u0 * outer, centre + u1 * outer, centre + u1 * inner, centre + u0 * inner, colour);
            }
        render(element, std::move(mesh));
    }

private:
    int               dashes_;
    Rml::NumericValue width_;
};

class DottedUnderline : public Drawn {
public:
    DottedUnderline(Rml::NumericValue size, Rml::NumericValue gap) : size_(size), gap_(gap) {}

    /* Whole pixels, so every dot comes out the same. */
    void RenderElement(Rml::Element* element, Rml::DecoratorDataHandle) const override
    {
        const Rml::Box&     box   = element->GetBox();
        const Rml::Vector2f at    = box.GetPosition(Rml::BoxArea::Content);
        const Rml::Vector2f area  = box.GetSize(Rml::BoxArea::Content);
        const float         size  = std::max(1.f, std::round(element->ResolveLength(size_)));
        const float         step  = size + std::max(1.f, std::round(element->ResolveLength(gap_)));
        const float         y     = std::round(at.y + area.y - size);
        const Rml::ColourbPremultiplied colour = colour_of(element);

        Rml::Mesh mesh;
        for (float x = std::round(at.x); x + size <= at.x + area.x; x += step)
            quad(mesh, { x, y }, { x + size, y }, { x + size, y + size }, { x, y + size }, colour);
        render(element, std::move(mesh));
    }

private:
    Rml::NumericValue size_, gap_;
};

class DashedRingInstancer : public Rml::DecoratorInstancer {
public:
    DashedRingInstancer()
    {
        dashes_ = RegisterProperty("dashes", "8").AddParser("number").GetId();
        width_  = RegisterProperty("width", "1dp").AddParser("length").GetId();
        RegisterShorthand("decorator", "dashes, width", Rml::ShorthandType::FallThrough);
    }

    Rml::SharedPtr<Rml::Decorator> InstanceDecorator(const Rml::String&, const Rml::PropertyDictionary& properties,
                                                     const Rml::DecoratorInstancerInterface&) override
    {
        const Rml::Property* p      = properties.GetProperty(dashes_);
        const Rml::Property* w      = properties.GetProperty(width_);
        const int            dashes = p ? p->Get<int>() : 0;
        if (dashes <= 0 || !w) return nullptr;
        return Rml::MakeShared<DashedRing>(dashes, w->GetNumericValue());
    }

private:
    Rml::PropertyId dashes_, width_;
};

class DottedUnderlineInstancer : public Rml::DecoratorInstancer {
public:
    DottedUnderlineInstancer()
    {
        size_ = RegisterProperty("size", "1dp").AddParser("length").GetId();
        gap_  = RegisterProperty("gap", "2dp").AddParser("length").GetId();
        RegisterShorthand("decorator", "size, gap", Rml::ShorthandType::FallThrough);
    }

    Rml::SharedPtr<Rml::Decorator> InstanceDecorator(const Rml::String&, const Rml::PropertyDictionary& properties,
                                                     const Rml::DecoratorInstancerInterface&) override
    {
        const Rml::Property* s = properties.GetProperty(size_);
        const Rml::Property* g = properties.GetProperty(gap_);
        if (!s || !g) return nullptr;
        return Rml::MakeShared<DottedUnderline>(s->GetNumericValue(), g->GetNumericValue());
    }

private:
    Rml::PropertyId size_, gap_;
};

} /* namespace */

void decorators_init()
{
    static DashedRingInstancer      ring;
    static DottedUnderlineInstancer underline;
    Rml::Factory::RegisterDecoratorInstancer("dashed-ring", &ring);
    Rml::Factory::RegisterDecoratorInstancer("dotted-underline", &underline);
}
