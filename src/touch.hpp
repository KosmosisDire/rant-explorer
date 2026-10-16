/* What fingers do beyond the SDL backend, which takes each finger as a mouse of its own. A
   finger that moves on or lifts hovers nothing, one the system takes over lets go, one held
   still is a right click, and two over a visual pinch it as the wheel. */
#ifndef TOUCH_HPP
#define TOUCH_HPP

#include <RmlUi/Core/Context.h>
#include <SDL3/SDL_events.h>

#include <cstdint>

class Touch {
public:
    /* Before the backend sees an event. True when it was used up here. */
    bool before(const SDL_Event& event, Rml::Context* context);
    /* After the backend saw it. */
    void after(const SDL_Event& event, Rml::Context* context);
    /* Once a frame, for a finger held still long enough. */
    void tick(Rml::Context* context);

private:
    struct Finger {
        SDL_FingerID  id = 0;
        Rml::Vector2f at, from;   /* in the context's pixels */
    };
    Finger   first_, second_;
    int      down_ = 0;         /* fingers down now */
    uint64_t pressed_ = 0;      /* when the first came down, in ms */
    bool     moved_ = false;    /* the first went further than a tap */
    bool     armed_ = false;    /* it may still turn into a long press */
    bool     taken_ = false;    /* a long press or a pinch took it, so the backend sees no more of it */
    float    span_ = 0;         /* the pinch's spread at its last move */
};

#endif /* TOUCH_HPP */
