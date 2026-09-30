/* The drawing kit the custom elements share: the theme's colours read off an element, and a
   canvas that batches shapes into one mesh and labels into a mesh per font texture. */
#ifndef CANVAS_HPP
#define CANVAS_HPP

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Mesh.h>
#include <RmlUi/Core/Texture.h>

#include <string>
#include <vector>

/* The drawing colours, the viz-* properties in components.rcss, so they follow the theme.
   Each element reads them into ink before it draws. The trail is a point's history in the
   air, the ground its foot and drop line. */
struct Ink {
    Rml::Colourb text, dim, faint, accent, green, amber, red, blue, purple, gray;
    Rml::Colourb panel, border, grid, trail, ground;
};
extern Ink ink;

/* Registers the viz-* properties. Once, before a document loads. */
void ink_init();
void read_ink(Rml::Element& element);

/* A colour at a share of its alpha. */
Rml::Colourb faded(Rml::Colourb colour, float share);

/* A rectangle of a drawing, in the element's pixels. */
struct Area {
    float x = 0, y = 0, w = 0, h = 0;
};

/* A drawing in an element's content box: pictures, then shapes in one mesh, then text.
   The mesh is the element's, so its memory is kept from frame to frame. */
class Canvas {
public:
    enum Align { Left, Center, Right };

    Canvas(Rml::Element& element, Rml::Mesh& mesh);

    float w() const { return size_.x; }
    float h() const { return size_.y; }
    float dp(float v) const { return v * ratio_; }
    Rml::Vector2f origin() const { return origin_; }
    Area all() const { return Area{ 0, 0, size_.x, size_.y }; }

    void line(float x0, float y0, float x1, float y1, Rml::Colourb colour, float width_dp = 1);
    void line(Rml::Vector2f a, Rml::Vector2f b, Rml::Colourb colour, float width_dp = 1)
    {
        line(a.x, a.y, b.x, b.y, colour, width_dp);
    }
    /* A line of dashes three dp long with three dp gaps. */
    void dashed(Rml::Vector2f a, Rml::Vector2f b, Rml::Colourb colour);
    /* A line with a filled head at its end. */
    void arrow(Rml::Vector2f a, Rml::Vector2f b, Rml::Colourb colour, float width_dp = 2);
    void rect(float x, float y, float w, float h, Rml::Colourb colour);
    /* A filled rectangle with corners of radius r_dp. */
    void rounded(float x, float y, float w, float h, float r_dp, Rml::Colourb colour);
    /* The outline of a rectangle. */
    void frame(float x, float y, float w, float h, Rml::Colourb colour, float width_dp = 1);
    void triangle(Rml::Vector2f a, Rml::Vector2f b, Rml::Vector2f c, Rml::Colourb colour);
    /* A disc with as many sides as its size in pixels needs, 6 for a trail dot, 16 at most. */
    void dot(float x, float y, Rml::Colourb colour, float radius_dp = 3);
    void dot(Rml::Vector2f p, Rml::Colourb colour, float radius_dp = 3) { dot(p.x, p.y, colour, radius_dp); }

    /* Text on a baseline at y, aligned on x. */
    void text(const std::string& s, float x, float y, Align align, Rml::Colourb colour, float size_dp = 10);
    /* How wide text would draw, in pixels. */
    float text_width(const std::string& s, float size_dp = 10) const;

    /* A texture stretched over a rectangle, drawn under the shapes and the text. Samples
       stay half a texel inside the edge, so smoothing never wraps the far edge in. */
    void picture(float x, float y, float w, float h, Rml::Texture texture, Rml::Vector2i texels);

    void render();

private:
    struct Picture {
        Rml::Vector2f at, size;
        Rml::Texture  texture;
        Rml::Vector2f inset;
    };
    struct Label {
        std::string  s;
        float        x, y;
        Align        align;
        Rml::Colourb colour;
        int          size;
    };

    void quad(Rml::Vector2f a, Rml::Vector2f b, Rml::Vector2f c, Rml::Vector2f d, Rml::Colourb colour);

    Rml::Element&        element_;
    float                ratio_ = 1;
    Rml::Vector2f        origin_, size_;
    Rml::Mesh&           mesh_;
    std::vector<Label>   labels_;
    std::vector<Picture> pictures_;
};

#endif /* CANVAS_HPP */
