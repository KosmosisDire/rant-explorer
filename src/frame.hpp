/* The window without a system title bar: the topbar stands in for it. */
#ifndef FRAME_HPP
#define FRAME_HPP

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>

/* A point on an element marked window-drag moves the window, and a band along each edge
   resizes it. Call once the context exists. */
void frame_init(SDL_Window* window, Rml::Context* context);

#endif /* FRAME_HPP */
