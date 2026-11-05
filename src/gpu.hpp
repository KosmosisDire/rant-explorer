/* The GPU half of a 3D card: strokes and faint lit faces in world space, drawn depth tested
   into the frame RmlUi is drawing. */
#ifndef GPU_HPP
#define GPU_HPP

#include <RmlUi/Core/Colour.h>
#include <RmlUi/Core/Types.h>

#include <cstdint>
#include <vector>

/* What one 3D drawing puts on the GPU. A stroke keeps its size in pixels however far away it
   lies. Faces are lit triangles over corners, drawn behind the strokes that lie on them. */
struct Batch {
    /* A line with round ends, a dashed line, a line with a head at b, or a dot at a. */
    enum class Mark { Line, Dashed, Arrow, Dot };

    struct Stroke {
        Rml::Vector3f a, b;
        Rml::Colourb  colour;
        float         size;   /* a line's width or a dot's radius, in pixels */
        float         mark;   /* a Mark, as the shader reads it */
    };
    struct Corner {
        Rml::Vector3f at, normal;
        Rml::Colourb  colour;
    };

    std::vector<Stroke>   strokes;
    std::vector<Corner>   corners;
    std::vector<uint32_t> triangles;   /* three corners each, counterclockwise seen from outside */
    /* The strokes before it are guides, such as the floor grid: they show where nothing is in
       front and hide nothing, so a line lying on one still shows whole. */
    size_t under = 0;

    void clear()
    {
        strokes.clear();
        corners.clear();
        triangles.clear();
        under = 0;
    }

    void stroke(Mark mark, Rml::Vector3f a, Rml::Vector3f b, Rml::Colourb colour, float size)
    {
        strokes.push_back({ a, b, colour, size, (float)mark });
    }

    /* The furthest any point lies from the origin. */
    float extent() const;
};

/* Draws a batch inside RmlUi's frame, into the layer and scissor it has set, and leaves its
   state as it was. to_clip takes the world to clip space, light points at the light, and dp is
   pixels per dp. */
void draw_batch(const Batch& batch, const Rml::Matrix4f& to_clip, Rml::Vector3f light, float dp, Rml::Vector2i window);

/* While on, a texture RmlUi draws magnifies to sharp texels, as a grid's cells want. Turn it
   off before RmlUi draws anything else. */
void sharp_texels(bool on);

#endif /* GPU_HPP */
