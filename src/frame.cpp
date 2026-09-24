#include "frame.hpp"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#endif

namespace {

SDL_HitTestResult hit_test(SDL_Window* window, const SDL_Point* point, void* data)
{
    if (!(SDL_GetWindowFlags(window) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN))) {
        const int band = 6;
        int w = 0, h = 0;
        SDL_GetWindowSize(window, &w, &h);
        const bool left = point->x < band, right = point->x >= w - band;
        const bool top = point->y < band, bottom = point->y >= h - band;
        if (top && left) return SDL_HITTEST_RESIZE_TOPLEFT;
        if (top && right) return SDL_HITTEST_RESIZE_TOPRIGHT;
        if (bottom && left) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
        if (bottom && right) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
        if (top) return SDL_HITTEST_RESIZE_TOP;
        if (bottom) return SDL_HITTEST_RESIZE_BOTTOM;
        if (left) return SDL_HITTEST_RESIZE_LEFT;
        if (right) return SDL_HITTEST_RESIZE_RIGHT;
    }
    /* Only the element right under the point counts, so a tab or button on the topbar
       still takes its click. */
    const float   density = SDL_GetWindowPixelDensity(window);
    Rml::Context* context = static_cast<Rml::Context*>(data);
    Rml::Element* hit     = context->GetElementAtPoint(Rml::Vector2f(point->x * density, point->y * density));
    return hit && hit->HasAttribute("window-drag") ? SDL_HITTEST_DRAGGABLE : SDL_HITTEST_NORMAL;
}

} // namespace

void frame_init(SDL_Window* window, Rml::Context* context)
{
    SDL_SetWindowHitTest(window, hit_test, context);
#ifdef _WIN32
    /* Windows 11 gives a window without a title bar square corners unless asked. Older
       versions ignore the request. */
    HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                             SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (hwnd) {
        const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
        DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    }
#endif
}
