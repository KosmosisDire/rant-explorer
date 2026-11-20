/* Hover tips: an element with a tip attribute shows its text in the document's #tip box once
   the pointer rests on it. A press or a scroll hides it until the pointer moves to another. */
#ifndef TIPS_HPP
#define TIPS_HPP

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/EventListener.h>

class Tips final : public Rml::EventListener {
public:
    /* touch is read each frame: a finger has no hover, so no tip shows while it is on. */
    Tips(Rml::Context* context, const bool& touch);

    /* Hears presses and scrolls on the document. Again after a reload. */
    void listen(Rml::ElementDocument* document);
    /* Once per frame, before the context updates. */
    void update(double now_s);

    void ProcessEvent(Rml::Event& event) override;

private:
    void hide();

    Rml::Context*                  context_;
    const bool&                    touch_;
    Rml::ObserverPtr<Rml::Element> owner_;   /* the element the pointer rests on */
    Rml::ObserverPtr<Rml::Element> tip_;     /* the box showing its text */
    double                         since_     = 0;
    bool                           shown_     = false;
    bool                           dismissed_ = false;
};

#endif /* TIPS_HPP */
