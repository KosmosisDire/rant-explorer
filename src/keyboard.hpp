/* A phone raises its keyboard only for a focused page input, which SDL's web backend never
   makes. While an RmlUi text field has focus on touch, a hidden page input carries its text:
   what is typed there sets the field, Enter reaches the field as a key, and closing the
   keyboard blurs the field. The desktop build has its own keyboard, so this does nothing. */
#ifndef KEYBOARD_HPP
#define KEYBOARD_HPP

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/EventListener.h>

class Keyboard final : public Rml::EventListener {
public:
    /* touch is read at each focus, so a page that turns touch on or off follows it. */
    Keyboard(Rml::Context* context, const bool& touch);

    /* Hears focus and blur on every field of the document. Again after a reload. */
    void listen(Rml::ElementDocument* document);

    void ProcessEvent(Rml::Event& event) override;

private:
    const bool& touch_;
};

#endif /* KEYBOARD_HPP */
