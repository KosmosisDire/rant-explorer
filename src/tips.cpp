#include "tips.hpp"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/StringUtilities.h>

namespace {

constexpr double DELAY_S = 0.5;
constexpr float  GAP_DP  = 4.f;

Rml::Element* tip_owner(Rml::Element* element)
{
    while (element && !element->HasAttribute("tip")) element = element->GetParentNode();
    return element;
}

} /* namespace */

Tips::Tips(Rml::Context* context, const bool& touch) : context_(context), touch_(touch) {}

void Tips::listen(Rml::ElementDocument* document)
{
    document->AddEventListener(Rml::EventId::Mousedown, this, true);
    document->AddEventListener(Rml::EventId::Mousescroll, this, true);
}

void Tips::ProcessEvent(Rml::Event&)
{
    hide();
    dismissed_ = true;
}

void Tips::hide()
{
    if (!shown_) return;
    shown_ = false;
    if (tip_) tip_->SetClass("shown", false);
}

void Tips::update(double now_s)
{
    Rml::Element* owner = touch_ ? nullptr : tip_owner(context_->GetHoverElement());
    if (owner != owner_.get()) {
        hide();
        owner_     = owner ? owner->GetObserverPtr() : Rml::ObserverPtr<Rml::Element>();
        since_     = now_s;
        dismissed_ = false;
    }
    if (!owner || shown_ || dismissed_ || now_s - since_ < DELAY_S) return;

    Rml::Element* tip = owner->GetOwnerDocument()->GetElementById("tip");
    const Rml::String text = owner->GetAttribute<Rml::String>("tip", "");
    if (!tip || text.empty()) return;
    tip->SetInnerRML(Rml::StringUtilities::EncodeRml(text));

    /* Under the owner, or over it near the bottom, and grown away from the nearer side
       edge, so it stays in the window without measuring it. */
    const float          ratio = context_->GetDensityIndependentPixelRatio();
    const Rml::Vector2f  at    = owner->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f  box   = owner->GetBox().GetSize(Rml::BoxArea::Border);
    const Rml::Vector2f  view(context_->GetDimensions());
    const float          gap   = GAP_DP * ratio;
    auto px = [](float v) { return Rml::Property(v, Rml::Unit::PX); };
    for (const char* side : { "left", "right", "top", "bottom" }) tip->RemoveProperty(side);
    if (at.x + box.x / 2 < view.x / 2) tip->SetProperty(Rml::PropertyId::Left, px(at.x));
    else                               tip->SetProperty(Rml::PropertyId::Right, px(view.x - at.x - box.x));
    if (at.y + box.y < view.y * 0.75f) tip->SetProperty(Rml::PropertyId::Top, px(at.y + box.y + gap));
    else                               tip->SetProperty(Rml::PropertyId::Bottom, px(view.y - at.y + gap));
    tip->SetClass("shown", true);
    tip_   = tip->GetObserverPtr();
    shown_ = true;
}
