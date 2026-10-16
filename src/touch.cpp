#include "touch.hpp"

#include <RmlUi/Core/Element.h>
#include <SDL3/SDL_timer.h>

#include <cmath>

namespace {

constexpr float    TAP_DP   = 8.f;     /* further than this is a scroll or a drag, not a tap */
constexpr uint64_t HOLD_MS  = 500;     /* held still this long is a right click */
constexpr float    PINCH    = 1.15f;   /* the spread changes this much for each wheel step */

Rml::Vector2f where(const SDL_Event& event, const Rml::Context* context)
{
    const Rml::Vector2i size = context->GetDimensions();
    return { event.tfinger.x * size.x, event.tfinger.y * size.y };
}

float apart(Rml::Vector2f a, Rml::Vector2f b)
{
    return std::hypot(a.x - b.x, a.y - b.y);
}

/* RmlUi forgets a finger it was following, without a click where it lifts. */
void let_go(Rml::Context* context, SDL_FingerID id, Rml::Vector2f at)
{
    context->ProcessMouseLeave();
    context->ProcessTouchCancel(Rml::TouchList{ Rml::Touch{ (Rml::TouchId)id, at } });
}

/* An element marked hold does its own holding, so a held finger there is no right click. */
bool holds_itself(Rml::Element* element)
{
    for (; element; element = element->GetParentNode())
        if (element->HasAttribute("hold")) return true;
    return false;
}

/* A visual zooms on the wheel, so a pinch over anything else does nothing. */
bool pinchable(Rml::Element* element)
{
    for (; element; element = element->GetParentNode())
        if (element->GetTagName() == "viz" || element->GetTagName() == "mesh-graph") return true;
    return false;
}

}

bool Touch::before(const SDL_Event& event, Rml::Context* context)
{
    switch (event.type) {
    case SDL_EVENT_FINGER_DOWN:
        if (down_++ == 0) {
            first_   = { event.tfinger.fingerID, where(event, context), where(event, context) };
            pressed_ = SDL_GetTicks();
            moved_ = taken_ = false;
            armed_ = true;
            return false;
        }
        if (down_ == 2) {
            second_ = { event.tfinger.fingerID, where(event, context), where(event, context) };
            if (!taken_) let_go(context, first_.id, first_.at);
            taken_ = true;
            span_  = apart(first_.at, second_.at);
        }
        return true;

    case SDL_EVENT_FINGER_MOTION:
        if (event.tfinger.fingerID == first_.id) first_.at = where(event, context);
        else if (event.tfinger.fingerID == second_.id) second_.at = where(event, context);
        if (down_ >= 2 && span_ > 0) {
            const Rml::Vector2f middle = (first_.at + second_.at) * 0.5f;
            if (!pinchable(context->GetElementAtPoint(middle))) return true;
            context->ProcessMouseMove((int)middle.x, (int)middle.y, 0);
            for (float spread = apart(first_.at, second_.at); spread > span_ * PINCH; span_ *= PINCH)
                context->ProcessMouseWheel(-1.f, 0);
            for (float spread = apart(first_.at, second_.at); spread < span_ / PINCH; span_ /= PINCH)
                context->ProcessMouseWheel(1.f, 0);
            context->ProcessMouseLeave();
            return true;
        }
        if (taken_) return true;
        if (apart(first_.at, first_.from) > TAP_DP * context->GetDensityIndependentPixelRatio()) {
            moved_ = true;
            armed_ = false;
        }
        return false;

    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED: {
        down_ = down_ > 0 ? down_ - 1 : 0;
        if (taken_) {
            if (down_ == 0) context->ProcessMouseLeave();
            return true;
        }
        /* The backend skips a canceled finger, which RmlUi must still let go of. */
        if (event.type == SDL_EVENT_FINGER_CANCELED) {
            let_go(context, event.tfinger.fingerID, where(event, context));
            return true;
        }
        return false;
    }
    default:
        return false;
    }
}

void Touch::after(const SDL_Event& event, Rml::Context* context)
{
    /* A finger only hovers while it might still be a tap, so a scroll lights no row. */
    if (event.type == SDL_EVENT_FINGER_UP || (event.type == SDL_EVENT_FINGER_MOTION && moved_))
        context->ProcessMouseLeave();
}

void Touch::tick(Rml::Context* context)
{
    if (!armed_ || down_ != 1 || taken_ || SDL_GetTicks() - pressed_ < HOLD_MS) return;
    armed_ = false;
    if (holds_itself(context->GetElementAtPoint(first_.at))) return;
    taken_ = true;
    let_go(context, first_.id, first_.at);
    context->ProcessMouseMove((int)first_.at.x, (int)first_.at.y, 0);
    context->ProcessMouseButtonDown(1, 0);
    context->ProcessMouseButtonUp(1, 0);
}
